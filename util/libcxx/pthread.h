#ifndef HARVEY_PTHREAD_H
#define HARVEY_PTHREAD_H

#include <sys/types.h>
#include <time.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long pthread_t;

typedef struct {
    int __count;
    pthread_t __owner;
} pthread_mutex_t;

typedef struct {
    int __type;
} pthread_mutexattr_t;

typedef struct {
    int __waiters;
} pthread_cond_t;

typedef int pthread_once_t;
typedef unsigned int pthread_key_t;

typedef struct {
    int __dummy;
} pthread_attr_t;

#define PTHREAD_MUTEX_INITIALIZER {0, 0}
#define PTHREAD_COND_INITIALIZER {0}
#define PTHREAD_ONCE_INIT 0
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1

int pthread_mutexattr_init(pthread_mutexattr_t *);
int pthread_mutexattr_destroy(pthread_mutexattr_t *);
int pthread_mutexattr_settype(pthread_mutexattr_t *, int);
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

int pthread_once(pthread_once_t *, void (*)(void));

int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
int pthread_detach(pthread_t);
pthread_t pthread_self(void);
int pthread_equal(pthread_t, pthread_t);

int pthread_key_create(pthread_key_t *, void (*)(void *));
int pthread_key_delete(pthread_key_t);
void *pthread_getspecific(pthread_key_t);
int pthread_setspecific(pthread_key_t, const void *);

int sched_yield(void);

#ifdef __cplusplus
}
#endif

#endif
