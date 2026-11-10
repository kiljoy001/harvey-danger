#ifndef HARVEY_PTHREAD_H
#define HARVEY_PTHREAD_H

/*
 * POSIX threads for Harvey.
 *
 * A thread is an rfork(RFPROC|RFMEM) process: it shares the address space,
 * namespace and file descriptor group with its creator, and the kernel
 * schedules it independently.  Synchronisation uses semacquire()/semrelease()
 * on words in shared memory, which is Plan 9's futex.
 */

#include <sys/types.h>
#include <time.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

struct __harvey_thread;
typedef struct __harvey_thread *pthread_t;

typedef struct {
    int32_t __word;     /* 1 = free, 0 = held */
    int32_t __type;     /* PTHREAD_MUTEX_NORMAL / RECURSIVE */
    uintptr_t __owner;  /* thread that holds it */
    int32_t __count;    /* recursion depth */
} pthread_mutex_t;

typedef struct {
    int32_t __type;
} pthread_mutexattr_t;

typedef struct {
    intptr_t __head;    /* waiter list */
    int32_t __lock;     /* protects the list: 1 = free, 0 = held */
    int32_t __pad;
} pthread_cond_t;

typedef int pthread_once_t;
typedef unsigned int pthread_key_t;

typedef struct {
    int __stacksize;
} pthread_attr_t;

typedef struct {
    pthread_mutex_t __mutex;
    pthread_cond_t __readers;
    pthread_cond_t __writers;
    int __active_readers;
    int __active_writers;
    int __waiting_writers;
} pthread_rwlock_t;

#define PTHREAD_MUTEX_INITIALIZER {1, 0, 0, 0}
#define PTHREAD_COND_INITIALIZER  {0, 1, 0}
#define PTHREAD_ONCE_INIT 0
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_RWLOCK_INITIALIZER \
    { {1, 0, 0, 0}, {0}, {0}, 0, 0, 0 }

#define PTHREAD_STACK_MIN 16384
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PTHREAD_KEYS_MAX 64

/* keys are per thread */
#define PTHREAD_DESTRUCTOR_ITERATIONS_ 4

int pthread_mutexattr_init(pthread_mutexattr_t *);
int pthread_mutexattr_destroy(pthread_mutexattr_t *);
int pthread_mutexattr_settype(pthread_mutexattr_t *, int);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *, int *);
int pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int pthread_mutex_destroy(pthread_mutex_t *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_trylock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);

int pthread_cond_init(pthread_cond_t *, const void *);
int pthread_cond_destroy(pthread_cond_t *);
int pthread_cond_signal(pthread_cond_t *);
int pthread_cond_broadcast(pthread_cond_t *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);

int pthread_rwlock_init(pthread_rwlock_t *, const void *);
int pthread_rwlock_destroy(pthread_rwlock_t *);
int pthread_rwlock_rdlock(pthread_rwlock_t *);
int pthread_rwlock_wrlock(pthread_rwlock_t *);
int pthread_rwlock_unlock(pthread_rwlock_t *);

int pthread_once(pthread_once_t *, void (*)(void));

int pthread_attr_init(pthread_attr_t *);
int pthread_attr_destroy(pthread_attr_t *);
int pthread_attr_setstacksize(pthread_attr_t *, size_t);
int pthread_attr_getstacksize(const pthread_attr_t *, size_t *);

int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
int pthread_detach(pthread_t);
pthread_t pthread_self(void);
int pthread_equal(pthread_t, pthread_t);
void pthread_exit(void *);

int pthread_key_create(pthread_key_t *, void (*)(void *));
int pthread_key_delete(pthread_key_t);
void *pthread_getspecific(pthread_key_t);
int pthread_setspecific(pthread_key_t, const void *);

int pthread_sigmask(int, const sigset_t *, sigset_t *);
int pthread_getcpuclockid(pthread_t, clockid_t *);
int pthread_getaffinity_np(pthread_t, size_t, cpu_set_t *);
int pthread_setaffinity_np(pthread_t, size_t, const cpu_set_t *);

#ifdef __cplusplus
}
#endif

#endif
