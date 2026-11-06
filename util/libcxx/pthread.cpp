/*
 * POSIX threads for Harvey.
 *
 * Harvey is Plan 9 at heart, so a thread is a process that shares memory:
 * rfork(RFPROC|RFMEM) gives us a kernel-scheduled execution context in the
 * same address space, namespace and file descriptor group.  Synchronisation
 * uses semacquire()/semrelease() on a word of shared memory -- the same
 * primitive a futex would provide -- so there is no user-space spinning.
 *
 * Each thread needs its own TLS block and stack: __harvey_tls_alloc() builds
 * the former (the kernel keys the FS base off the process), and the stack is
 * an anonymous mapping that the child switches to before it runs any C code.
 */

#include <pthread.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int semacquire(int32_t *, int);
int32_t semrelease(int32_t *, int32_t);
int tsemacquire(int32_t *, uint32_t);
void _exits(char *);
void *__harvey_tls_alloc(void);
int rfork(int);
unsigned int sleep(unsigned int);
}

enum {
    RFPROC = (1 << 4),
    RFMEM = (1 << 5),
    RFNOWAIT = (1 << 6),
};

#define DEFAULT_STACK (4u * 1024u * 1024u)

struct __harvey_thread {
    void *(*fn)(void *);
    void *arg;
    void *ret;
    char *stack;
    size_t stack_size;
    int32_t done;      /* semaphore: 1 when the thread has finished */
    int detached;
};

struct __cond_waiter {
    int32_t sem;
    struct __cond_waiter *next;
};

/* Set in each thread's TLS by thread_body(); the main thread gets a static. */
__thread __harvey_thread *__harvey_self;

namespace {

inline uintptr_t current_thread() {
    return (uintptr_t)pthread_self();
}

inline void lock_word(int32_t *w) {
    semacquire(w, 1);
}

inline void unlock_word(int32_t *w) {
    semrelease(w, 1);
}

/*
 * The waiter list is protected by the condition variable's own lock rather
 * than by CAS.  A timed waiter that times out has to unlink its stack node
 * before returning, and a lock makes that safe: a signaler either removes
 * the node before the waiter does (and releases its semaphore under the
 * lock) or finds it already gone.  Without that, a signal could touch a
 * dead stack frame.
 */
inline void cond_lock(pthread_cond_t *c) {
    semacquire(&c->__lock, 1);
}

inline void cond_unlock(pthread_cond_t *c) {
    semrelease(&c->__lock, 1);
}

void cond_push(pthread_cond_t *c, __cond_waiter *w) {
    w->next = (__cond_waiter *)c->__head;
    c->__head = (intptr_t)w;
}

__cond_waiter *cond_pop(pthread_cond_t *c) {
    __cond_waiter *w = (__cond_waiter *)c->__head;
    if (w)
        c->__head = (intptr_t)w->next;
    return w;
}

void cond_remove(pthread_cond_t *c, __cond_waiter *w) {
    __cond_waiter **p = (__cond_waiter **)&c->__head;
    while (*p) {
        if (*p == w) {
            *p = w->next;
            return;
        }
        p = &(*p)->next;
    }
}

/* Runs on the thread's own stack: install its TLS, call the body, finish. */
void thread_body(__harvey_thread *t) {
    __harvey_tls_alloc();
    __harvey_self = t;
    t->ret = t->fn(t->arg);
    __sync_synchronize();
    semrelease(&t->done, 1);
    /* Daemon threads are reaped by the creator's join; a detached thread
     * simply exits and leaks its bookkeeping, as POSIX allows. */
    _exits(nullptr);
}

/*
 * rfork, and in the child switch to the new stack before executing any C
 * code.  The main thread's stack lives in the shared heap, so the parent and
 * the new child would otherwise run on the same stack: the child's very first
 * C statement could overwrite the parent's frame.  Doing the switch here
 * means the only shared-stack access is the identical return-address push of
 * the rfork call itself.
 */
#if defined(__aarch64__)
extern "C" __attribute__((naked)) int
thread_spawn(int flags, void *stacktop, void (*fn)(void *), void *arg) {
    __asm__ volatile(
        "stp x19, x20, [sp, #-16]!\n\t"
        "stp x21, x30, [sp, #-16]!\n\t"
        "mov x19, x1\n\t"
        "mov x20, x2\n\t"
        "mov x21, x3\n\t"
        "bl rfork\n\t"
        "cbnz w0, 1f\n\t"
        "mov sp, x19\n\t"
        "mov x0, x21\n\t"
        "blr x20\n\t"
        "brk #0\n\t"
        "1:\n\t"
        "ldp x21, x30, [sp], #16\n\t"
        "ldp x19, x20, [sp], #16\n\t"
        "ret\n\t");
}
#else
extern "C" __attribute__((naked)) int
thread_spawn(int flags, void *stacktop, void (*fn)(void *), void *arg) {
    __asm__ volatile(
        "pushq %rbx\n\t"
        "pushq %r12\n\t"
        "pushq %r13\n\t"
        "movq %rsi, %rbx\n\t"
        "movq %rdx, %r12\n\t"
        "movq %rcx, %r13\n\t"
        "callq rfork\n\t"
        "testl %eax, %eax\n\t"
        "jne 1f\n\t"
        "movq %rbx, %rsp\n\t"
        "xorl %ebp, %ebp\n\t"
        "movq %r13, %rdi\n\t"
        "callq *%r12\n\t"
        "hlt\n\t"
        "1:\n\t"
        "popq %r13\n\t"
        "popq %r12\n\t"
        "popq %rbx\n\t"
        "retq\n\t");
}
#endif

} // namespace

extern "C" {

/* ---- mutexes ---------------------------------------------------------- */

int pthread_mutexattr_init(pthread_mutexattr_t *a) {
    if (a) a->__type = PTHREAD_MUTEX_NORMAL;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *a) {
    (void)a;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type) {
    if (a) a->__type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type) {
    if (!a || !type) return EINVAL;
    *type = a->__type;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    if (!m) return EINVAL;
    m->__word = 1;
    m->__type = a ? a->__type : PTHREAD_MUTEX_NORMAL;
    m->__owner = 0;
    m->__count = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    uintptr_t self = current_thread();
    if (m->__type == PTHREAD_MUTEX_RECURSIVE && m->__owner == self) {
        m->__count++;
        return 0;
    }
    lock_word(&m->__word);
    m->__owner = self;
    m->__count = 1;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    uintptr_t self = current_thread();
    if (m->__type == PTHREAD_MUTEX_RECURSIVE && m->__owner == self) {
        m->__count++;
        return 0;
    }
    if (semacquire(&m->__word, 0) != 1)
        return EBUSY;
    m->__owner = self;
    m->__count = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (m->__type == PTHREAD_MUTEX_RECURSIVE && --m->__count > 0)
        return 0;
    m->__owner = 0;
    m->__count = 0;
    unlock_word(&m->__word);
    return 0;
}

/* ---- condition variables ---------------------------------------------- */

int pthread_cond_init(pthread_cond_t *c, const void *a) {
    (void)a;
    if (!c) return EINVAL;
    c->__head = 0;
    c->__lock = 1;
    c->__pad = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) {
    (void)c;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    __cond_waiter w;
    w.sem = 0;
    w.next = nullptr;
    cond_lock(c);
    cond_push(c, &w);
    cond_unlock(c);
    pthread_mutex_unlock(m);
    semacquire(&w.sem, 1);
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *ts) {
    __cond_waiter w;
    long long deadline, cur, ms;

    if (!ts)
        return pthread_cond_wait(c, m);

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    cur = (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
    deadline = (long long)ts->tv_sec * 1000000000LL + ts->tv_nsec;
    ms = (deadline - cur + 999999) / 1000000;   /* round up */
    if (ms <= 0) {
        /* Already expired: behave like a timed wait that timed out. */
        pthread_mutex_unlock(m);
        pthread_mutex_lock(m);
        return ETIMEDOUT;
    }
    if (ms > 0x7fffffffLL)
        ms = 0x7fffffffLL;

    w.sem = 0;
    w.next = nullptr;
    cond_lock(c);
    cond_push(c, &w);
    cond_unlock(c);
    pthread_mutex_unlock(m);

    if (tsemacquire(&w.sem, (uint32_t)ms) == 1) {
        pthread_mutex_lock(m);
        return 0;
    }

    /*
     * Timed out.  Unlink the node under the condition lock so no signal can
     * reference this stack frame afterwards; then check once more, because a
     * signal may have popped and released us just before we got the lock.
     */
    cond_lock(c);
    cond_remove(c, &w);
    cond_unlock(c);

    int rc = ETIMEDOUT;
    if (semacquire(&w.sem, 0) == 1)
        rc = 0;

    pthread_mutex_lock(m);
    return rc;
}

int pthread_cond_signal(pthread_cond_t *c) {
    cond_lock(c);
    __cond_waiter *w = cond_pop(c);
    if (w)
        semrelease(&w->sem, 1);
    cond_unlock(c);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    cond_lock(c);
    __cond_waiter *w;
    while ((w = cond_pop(c)) != nullptr)
        semrelease(&w->sem, 1);
    cond_unlock(c);
    return 0;
}

/* ---- read/write locks -------------------------------------------------- */

int pthread_rwlock_init(pthread_rwlock_t *r, const void *a) {
    (void)a;
    pthread_mutex_init(&r->__mutex, nullptr);
    pthread_cond_init(&r->__readers, nullptr);
    pthread_cond_init(&r->__writers, nullptr);
    r->__active_readers = 0;
    r->__active_writers = 0;
    r->__waiting_writers = 0;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *r) {
    (void)r;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *r) {
    pthread_mutex_lock(&r->__mutex);
    while (r->__active_writers || r->__waiting_writers)
        pthread_cond_wait(&r->__readers, &r->__mutex);
    r->__active_readers++;
    pthread_mutex_unlock(&r->__mutex);
    return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *r) {
    pthread_mutex_lock(&r->__mutex);
    r->__waiting_writers++;
    while (r->__active_writers || r->__active_readers)
        pthread_cond_wait(&r->__writers, &r->__mutex);
    r->__waiting_writers--;
    r->__active_writers = 1;
    pthread_mutex_unlock(&r->__mutex);
    return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t *r) {
    pthread_mutex_lock(&r->__mutex);
    if (r->__active_writers) {
        r->__active_writers = 0;
        pthread_cond_signal(&r->__readers);
        pthread_cond_signal(&r->__writers);
    } else if (r->__active_readers) {
        if (--r->__active_readers == 0)
            pthread_cond_signal(&r->__writers);
    }
    pthread_mutex_unlock(&r->__mutex);
    return 0;
}

/* ---- once -------------------------------------------------------------- */

int pthread_once(pthread_once_t *o, void (*fn)(void)) {
    if (__sync_bool_compare_and_swap(o, 0, 1)) {
        fn();
        __sync_synchronize();
        *(volatile int *)o = 2;
        return 0;
    }
    while (*(volatile int *)o != 2)
        sched_yield();
    return 0;
}

/* ---- thread attributes ------------------------------------------------- */

int pthread_attr_init(pthread_attr_t *a) {
    if (a) a->__stacksize = (int)DEFAULT_STACK;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *a) {
    (void)a;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *a, size_t n) {
    if (!a || n < PTHREAD_STACK_MIN) return EINVAL;
    a->__stacksize = (int)n;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *n) {
    if (!a || !n) return EINVAL;
    *n = (size_t)a->__stacksize;
    return 0;
}

/* ---- threads ----------------------------------------------------------- */

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg) {
    if (!out || !fn) return EINVAL;

    size_t stacksize = attr && attr->__stacksize > 0
                           ? (size_t)attr->__stacksize
                           : DEFAULT_STACK;

    __harvey_thread *t = (__harvey_thread *)malloc(sizeof *t);
    if (!t) return EAGAIN;
    memset(t, 0, sizeof *t);

    /*
     * RFMEM shares the heap but not the stack segment, so thread stacks must
     * come from the heap to be visible to every other thread.
     */
    t->stack = (char *)malloc(stacksize);
    if (!t->stack) {
        free(t);
        return EAGAIN;
    }
    t->stack_size = stacksize;
    t->fn = fn;
    t->arg = arg;
    t->done = 0;

    char *top = (char *)((uintptr_t)(t->stack + t->stack_size) & ~(uintptr_t)15);
    int pid = thread_spawn(RFPROC | RFMEM | RFNOWAIT, top, (void (*)(void *))thread_body, t);
    if (pid < 0) {
        free(t->stack);
        free(t);
        return EAGAIN;
    }

    *out = t;
    return 0;
}

int pthread_join(pthread_t t, void **ret) {
    if (!t) return EINVAL;
    semacquire(&t->done, 1);
    if (ret)
        *ret = t->ret;
    if (!t->detached) {
        /*
         * The thread releases t->done from thread_body() and only then
         * calls _exits(), still on t->stack.  Freeing the stack here lets
         * the joiner reuse it under the exiting thread -- a use-after-free
         * that SMP makes immediate.  Threads are pooled and the whole heap
         * goes away with the process, so leave the stack for exit.
         */
        free(t);
    }
    return 0;
}

int pthread_detach(pthread_t t) {
    if (!t) return EINVAL;
    t->detached = 1;
    return 0;
}

pthread_t pthread_self(void) {
    if (!__harvey_self) {
        static __harvey_thread main_thread;
        __harvey_self = &main_thread;
    }
    return __harvey_self;
}

int pthread_equal(pthread_t a, pthread_t b) {
    return a == b;
}

void pthread_exit(void *ret) {
    __harvey_thread *t = pthread_self();
    if (t) {
        t->ret = ret;
        __sync_synchronize();
        semrelease(&t->done, 1);
    }
    _exits(nullptr);
}

/* ---- thread-specific data ---------------------------------------------- */

static __thread void *key_values[PTHREAD_KEYS_MAX];
static int next_key = 1;

int pthread_key_create(pthread_key_t *key, void (*at_exit)(void *)) {
    (void)at_exit;
    if (!key) return EINVAL;
    int k = __sync_add_and_fetch(&next_key, 1) - 1;
    if (k >= PTHREAD_KEYS_MAX) return EAGAIN;
    *key = (pthread_key_t)k;
    return 0;
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= PTHREAD_KEYS_MAX) return EINVAL;
    key_values[key] = nullptr;
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    if (key >= PTHREAD_KEYS_MAX) return nullptr;
    return key_values[key];
}

int pthread_setspecific(pthread_key_t key, const void *p) {
    if (key >= PTHREAD_KEYS_MAX) return EINVAL;
    key_values[key] = (void *)p;
    return 0;
}

int sched_yield(void) {
    /* Plan 9 has no yield syscall; a zero-length sleep is the yield. */
    sleep(0);
    return 0;
}

int sched_setaffinity(int pid, size_t setsize, const cpu_set_t *set) {
    (void)pid;
    (void)setsize;
    (void)set;
    return 0;
}

int sched_getaffinity(int pid, size_t setsize, cpu_set_t *set) {
    (void)pid;
    if (set && setsize >= sizeof(cpu_set_t)) {
        __builtin_memset(set, 0, sizeof(cpu_set_t));
        CPU_SET(0, set);
    }
    return 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old) {
    return sigprocmask(how, set, old);
}

int pthread_getcpuclockid(pthread_t t, clockid_t *id) {
    (void)t;
    if (id) *id = 0;
    return 0;
}

} // extern "C"
