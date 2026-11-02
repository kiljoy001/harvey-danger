/* Single-threaded pthread shim for Harvey/APEX.
 *
 * libc++ needs the pthread API even when LLVM is built with
 * LLVM_ENABLE_THREADS=OFF.  Mutexes and condition variables are no-ops,
 * thread creation runs synchronously, and TLS is a single static table.
 * This makes the C++ headers and LLVM's Mutex.h work; it is not a real
 * threading implementation.
 */

#include <pthread.h>
#include <stddef.h>
#include <string.h>

extern "C" {

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

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;
    if (m) { m->__count = 0; m->__owner = 0; }
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    if (m) { m->__count++; m->__owner = pthread_self(); }
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    return pthread_mutex_lock(m);
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (m && m->__count > 0) m->__count--;
    return 0;
}

int pthread_cond_init(pthread_cond_t *c, const void *a) {
    (void)a;
    if (c) c->__waiters = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) {
    (void)c;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c) {
    (void)c;
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    (void)c;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    (void)c; (void)m;
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *ts) {
    (void)c; (void)m; (void)ts;
    return 0;
}

int pthread_once(pthread_once_t *o, void (*fn)(void)) {
    if (o && *o == 0) {
        *o = 1;
        fn();
    }
    return 0;
}

int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
    (void)a;
    if (t) *t = 1;
    if (fn) fn(arg);
    return 0;
}

int pthread_join(pthread_t t, void **ret) {
    (void)t;
    if (ret) *ret = 0;
    return 0;
}

int pthread_detach(pthread_t t) {
    (void)t;
    return 0;
}

pthread_t pthread_self(void) {
    return 1;
}

int pthread_equal(pthread_t a, pthread_t b) {
    return a == b;
}

#define NKEYS 128
static pthread_key_t next_key = 1;
static void *key_values[NKEYS];

int pthread_key_create(pthread_key_t *key, void (*at_exit)(void *)) {
    (void)at_exit;
    if (!key || next_key >= NKEYS) return 1;
    *key = next_key++;
    return 0;
}

int pthread_key_delete(pthread_key_t key) {
    if (key < NKEYS) key_values[key] = 0;
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    return key < NKEYS ? key_values[key] : 0;
}

int pthread_setspecific(pthread_key_t key, const void *p) {
    if (key >= NKEYS) return 1;
    key_values[key] = (void *)p;
    return 0;
}

int sched_yield(void) {
    return 0;
}

}
