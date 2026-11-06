/* p9 - Plan 9 kernel primitives for the plan9ized ESP8266 OS.
 *
 * Phase B substrate (doc/PLAN9-OS.md): Proc, Rendez, QLock and
 * bounded message queues, sized for one core, no MMU, static
 * pools. Portable logic lives in p9.c; the blocking substrate
 * (how a proc actually waits) is three hooks in a p9_os_*.c
 * file - POSIX for host tests today, the Xtensa scheduler in
 * Phase C. Semaphores here are internal; see osi_wifi.h for the
 * blob-facing counting-semaphore API built on these pieces.
 */
#ifndef P9_H
#define P9_H

#include <stddef.h>
#include <stdint.h>

#define P9_BLOCKING 0xFFFFFFFFu
#define P9_TICK_MS 10u /* one tick = 10 ms, as the RTOS config */

typedef struct P9OsLock P9OsLock;
typedef struct P9Wait P9Wait;

/* ---- substrate hooks (implemented per platform) ---- */
void p9_os_lock_init(P9OsLock *l);
void p9_os_lock(P9OsLock *l);
int p9_os_trylock(P9OsLock *l); /* 1 if acquired; never yields */
void p9_os_unlock(P9OsLock *l);
void p9_os_wait_init(P9Wait *w);
void p9_os_wait(P9Wait *w, P9OsLock *l); /* unlock l, sleep, relock */
void p9_os_wake(P9Wait *w, int all);
void *p9_os_thread_create(void (*fn)(void *), void *arg, const char *name);
void p9_os_thread_exit(void);
void *p9_os_current(void);
void p9_os_yield(void);
void p9_os_delay_ms(uint32_t ms);
uint64_t p9_os_now_ms(void);

/* Opaque substrate storage (POSIX impl keeps pointers inside). */
struct P9OsLock { void *impl; };
struct P9Wait { void *impl; };

/* ---- QLock: queued (FIFO ticket) mutual exclusion ---- */
typedef struct {
	P9OsLock lock;
	P9Wait wait;
	uint32_t next_ticket;
	uint32_t serving;
	int locked;
} P9QLock;

void p9_qlock_init(P9QLock *q);
void p9_qlock(P9QLock *q);
int p9_canqlock(P9QLock *q);
void p9_qunlock(P9QLock *q);

/* ---- Rendez: sleep/wakeup on a condition ----
 * Plan 9 discipline: caller holds an associated lock, checks a
 * condition in a loop, and sleeps atomically releasing it. */
typedef struct {
	P9Wait wait;
} P9Rendez;

void p9_rendez_init(P9Rendez *r);
void p9_sleep(P9Rendez *r, P9OsLock *lock);
void p9_wakeup(P9Rendez *r);

/* ---- Proc ---- */
typedef struct P9Proc P9Proc;
struct P9Proc {
	char name[16];
	void *os; /* substrate handle */
	int exited;
	P9Proc *next;
};

P9Proc *p9_proc_create(const char *name, void (*fn)(void *), void *arg);
void p9_proc_exit(void);
P9Proc *p9_current(void);
void p9_yield(void);
void p9_delay_ms(uint32_t ms);
uint32_t p9_ms_to_ticks(uint32_t ms);
void p9_delay_ticks(uint32_t ticks);

/* Scheduler gate (OSI suspend/resume-all equivalent). While
 * held, queue/timer waits park at checkpoints. */
void p9_sched_suspend_all(void);
void p9_sched_resume_all(void);
void p9_checkpoint(void);

/* ---- Bounded message queue (fixed-size items, copied) ---- */
typedef struct {
	P9OsLock lock;
	P9Wait not_empty;
	P9Wait not_full;
	uint8_t *items;
	uint32_t item_size;
	uint32_t cap;
	uint32_t head; /* index of oldest */
	uint32_t count;
} P9MsgQ;

void p9_msgq_init(P9MsgQ *q, void *storage, uint32_t cap, uint32_t item_size);
/* pos: 0=front, 1=back, 2=overwrite. timeout in ticks. */
int p9_msgq_send(P9MsgQ *q, const void *item, uint32_t timeout_ticks, int pos);
/* ISR-safe send: no checkpoint, no waiting, no yield. Returns 0
 * if the queue lock is contended or the queue is full. */
int p9_msgq_send_isr(P9MsgQ *q, const void *item, int pos);
int p9_msgq_recv_isr(P9MsgQ *q, void *item); /* ISR-safe try-receive */
int p9_msgq_recv(P9MsgQ *q, void *item, uint32_t timeout_ticks);
uint32_t p9_msgq_num(const P9MsgQ *q);

#endif
