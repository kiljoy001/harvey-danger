#!/usr/bin/env python3
"""Exercise the patched LKL CPU idle handoff with a pthread host.

Compiles the actual arch/lkl/kernel/cpu.c against minimal Linux declarations.
This tests host ownership and semaphore ordering, not Linux scheduling policy.
Usage: python3 util/lkl/test_idle.py /path/to/lkl
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

HEADERS = r'''
#ifndef TEST_LKL_H
#define TEST_LKL_H
#include <stdbool.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sched.h>
#include <stdarg.h>
typedef unsigned long lkl_thread_t;
struct lkl_mutex { pthread_mutex_t lock; };
struct lkl_sem {
 pthread_mutex_t lock; pthread_cond_t cond;
 unsigned count; atomic_int waiters;
};
struct task_struct { bool idle; };
struct thread_info { lkl_thread_t tid; int flags; bool resched; struct task_struct *task; };
struct lkl_host_operations {
 struct lkl_mutex *(*mutex_alloc)(int);
 void (*mutex_free)(struct lkl_mutex *);
 void (*mutex_lock)(struct lkl_mutex *);
 void (*mutex_unlock)(struct lkl_mutex *);
 struct lkl_sem *(*sem_alloc)(int);
 void (*sem_free)(struct lkl_sem *);
 void (*sem_up)(struct lkl_sem *);
 void (*sem_down)(struct lkl_sem *);
 lkl_thread_t (*thread_self)(void);
 int (*thread_equal)(lkl_thread_t, lkl_thread_t);
 void (*thread_exit)(void);
};
extern struct lkl_host_operations *lkl_ops;
extern struct thread_info test_ti;
#define current (&test_task)
extern struct task_struct test_task;
#define current_thread_info() (&test_ti)
#define need_resched() (test_ti.resched)
#define is_idle_task(t) ((t)->idle)
#define TIF_HOST_THREAD 1
#define test_ti_thread_flag(t, f) ((t)->flags & (f))
#define single_task_running() 1
#define in_interrupt() 0
#define irqs_disabled() 0
#define local_irq_enable() ((void)0)
void run_irqs(void);
void set_irq_pending(int);
void thread_sched_jb(void);
void wakeup_idle_host_task(void);
_Noreturn void lkl_bug(const char *, ...);
int lkl_cpu_get(void);
void lkl_cpu_put(void);
void lkl_cpu_idle_park(void);
#endif
'''

HARNESS = r'''
#include "test-lkl.h"
#include CPU_SOURCE
struct task_struct test_task = { .idle = true };
struct thread_info test_ti = { .tid = 1, .task = &test_task };
static _Thread_local unsigned long self = 1;
static atomic_int entered, finished;
static int mode, inject;
static unsigned posts;
static bool pending;
static struct lkl_mutex *ma(int recursive) {
 struct lkl_mutex *m = calloc(1, sizeof(*m)); assert(m);
 assert(pthread_mutex_init(&m->lock, NULL) == 0); return m;
}
static void mf(struct lkl_mutex *m) { pthread_mutex_destroy(&m->lock); free(m); }
static void ml(struct lkl_mutex *m) { assert(pthread_mutex_lock(&m->lock) == 0); }
static void mu(struct lkl_mutex *m) {
 assert(pthread_mutex_unlock(&m->lock) == 0);
 if (inject && self == 1 && cpu.idle_parked) {
  inject = 0; self = 2;
  assert(lkl_cpu_try_run_irq(1) == 0); self = 1;
 }
}
static struct lkl_sem *sa(int count) {
 struct lkl_sem *s = calloc(1, sizeof(*s)); assert(s);
 pthread_mutex_init(&s->lock, NULL); pthread_cond_init(&s->cond, NULL);
 s->count = count; return s;
}
static void sf(struct lkl_sem *s) {
 pthread_mutex_destroy(&s->lock); pthread_cond_destroy(&s->cond); free(s);
}
static void su(struct lkl_sem *s) {
 pthread_mutex_lock(&s->lock);
 if (s == cpu.idle_sem) { assert(s->count == 0); posts++; }
 s->count++; pthread_cond_signal(&s->cond); pthread_mutex_unlock(&s->lock);
}
static void sd(struct lkl_sem *s) {
 if (s == cpu.idle_sem) {
  atomic_store(&entered, 1);
  if (mode == 1 || mode == 3)
   while (!atomic_load(&finished)) sched_yield();
 }
 pthread_mutex_lock(&s->lock);
 while (!s->count) {
  atomic_fetch_add(&s->waiters, 1);
  pthread_cond_wait(&s->cond, &s->lock);
  atomic_fetch_sub(&s->waiters, 1);
 }
 s->count--; pthread_mutex_unlock(&s->lock);
}
static unsigned long ts(void) { return self; }
static int te(unsigned long a, unsigned long b) { return a == b; }
static void tx(void) { abort(); }
static struct lkl_host_operations ops = { ma, mf, ml, mu, sa, sf, su, sd, ts, te, tx };
struct lkl_host_operations *lkl_ops = &ops;
void lklhost_note(int op, void *p, int n) {}
void lklhost_cpusem(void *p) {}
_Noreturn void lkl_bug(const char *fmt, ...) {
 va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); abort();
}
void set_irq_pending(int irq) { pending = true; }
void run_irqs(void) { if (pending) test_ti.resched = true; pending = false; }
void thread_sched_jb(void) { abort(); }
void wakeup_idle_host_task(void) { lkl_cpu_idle_park(); }
static void *irq(void *unused) {
 self = 2;
 while (!atomic_load(&entered)) sched_yield();
 if (mode == 2)
  while (!atomic_load(&cpu.idle_sem->waiters)) sched_yield();
 assert(lkl_cpu_try_run_irq(1) == 1);
 if (mode == 3) assert(lkl_cpu_try_run_irq(1) == 1);
 test_ti.resched = true;
 if (mode == 3) {
  lkl_cpu_put(); assert(cpu.owner == 2 && cpu.count == 1 && cpu.idle_parked);
 }
 lkl_cpu_put();
 /* CPU is reserved for idle even if it has not consumed its notification. */
 for (int i = 0; i < 20; i++) assert(lkl_cpu_try_run_irq(1) == 0);
 atomic_store(&finished, 1); return NULL;
}
int main(void) {
 assert(lkl_cpu_init() == 0); assert(lkl_cpu_get() == 1);
 test_ti.resched = true; lkl_cpu_idle_park(); assert(posts == 0);
 for (int i = 0; i < 300; i++) {
  pthread_t thread;
  mode = 1 + i % 3; test_ti.resched = false;
  atomic_store(&entered, 0); atomic_store(&finished, 0);
  assert(pthread_create(&thread, NULL, irq, NULL) == 0);
  lkl_cpu_idle_park(); pthread_join(thread, NULL);
  assert(cpu.owner == 1 && cpu.count == 1 && cpu.idle_ti == NULL);
  assert(!cpu.idle_parked && cpu.idle_sem->count == 0 && posts == i + 1);
  /* Discard queued mock IRQs between independent trials. */
  pending = false; cpu.irqs_pending = false;
 }
 mode = 0; inject = 1; test_ti.resched = false;
 lkl_cpu_idle_park(); /* IRQ arrives after publishing parked, before put. */
 assert(posts == 301 && cpu.owner == 1 && cpu.count == 1);
 assert(cpu.idle_sem->count == 0 && !cpu.idle_ti);
 lkl_cpu_put(); lkl_cpu_cleanup(false);
 puts("PASS: 300 delayed/early/nested IRQ handoffs, duplicate IRQs, pending IRQ on park");
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("lkl", type=Path)
    args = ap.parse_args()
    source = (args.lkl / "arch/lkl/kernel/cpu.c").resolve()
    with tempfile.TemporaryDirectory(prefix="lkl-idle-test-") as tmp:
        root = Path(tmp)
        (root / "test-lkl.h").write_text(HEADERS)
        for include in ("linux/kernel.h", "linux/sched/stat.h", "asm/host_ops.h",
                        "asm/cpu.h", "asm/thread_info.h", "asm/unistd.h",
                        "asm/sched.h", "asm/syscalls.h", "linux/cpu.h"):
            path = root / include
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "test-lkl.h"\n')
        (root / "test.c").write_text(HARNESS)
        binary = root / "test"
        subprocess.run(["cc", "-std=gnu11", "-O2", "-pthread", "-I", str(root),
                        f'-DCPU_SOURCE="{source}"', str(root / "test.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
