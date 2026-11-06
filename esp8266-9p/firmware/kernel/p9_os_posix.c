/* p9_os_posix - POSIX substrate for host testing of p9.c.
 *
 * This file exists so the portable primitives can be exercised
 * on a workstation. Host threads are only the test vehicle: the
 * API semantics under test (ticket order, queue copy semantics,
 * rendez discipline) are substrate-independent. The Xtensa
 * substrate is Phase C (PLAN9-OS section 4) and does not use
 * this file.
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "p9.h"

void
p9_os_lock_init(P9OsLock *l)
{
	pthread_mutex_t *m = malloc(sizeof(*m));
	pthread_mutex_init(m, NULL);
	l->impl = m;
}

void
p9_os_lock(P9OsLock *l)
{
	pthread_mutex_lock((pthread_mutex_t *)l->impl);
}

int
p9_os_trylock(P9OsLock *l)
{
	return pthread_mutex_trylock((pthread_mutex_t *)l->impl) == 0;
}

void
p9_os_unlock(P9OsLock *l)
{
	pthread_mutex_unlock((pthread_mutex_t *)l->impl);
}

void
p9_os_wait_init(P9Wait *w)
{
	pthread_cond_t *c = malloc(sizeof(*c));
	pthread_cond_init(c, NULL);
	w->impl = c;
}

void
p9_os_wait(P9Wait *w, P9OsLock *l)
{
	pthread_cond_wait((pthread_cond_t *)w->impl,
			  (pthread_mutex_t *)l->impl);
}

void
p9_os_wake(P9Wait *w, int all)
{
	if (all)
		pthread_cond_broadcast((pthread_cond_t *)w->impl);
	else
		pthread_cond_signal((pthread_cond_t *)w->impl);
}

typedef struct {
	void (*fn)(void *);
	void *arg;
	volatile int started;
} ThreadBoot;

static void *
thread_main(void *arg)
{
	ThreadBoot *b = (ThreadBoot *)arg;
	void (*fn)(void *) = b->fn;
	void *a = b->arg;
	b->started = 1;
	fn(a);
	return NULL;
}

void *
p9_os_thread_create(void (*fn)(void *), void *arg, const char *name)
{
	pthread_t *th = malloc(sizeof(*th));
	ThreadBoot *b = malloc(sizeof(*b));
	(void)name;
	b->fn = fn;
	b->arg = arg;
	b->started = 0;
	if (pthread_create(th, NULL, thread_main, b) != 0)
		return NULL;
	/* p9.c already barrier-waits on its own boot copy; this
	 * substrate copies fn/arg synchronously enough for its
	 * contract but detached operation is fine. */
	pthread_detach(*th);
	return (void *)(uintptr_t)(*th);
}

void
p9_os_thread_exit(void)
{
	pthread_exit(NULL);
}

void *
p9_os_current(void)
{
	/* glibc pthread_t is an integer handle; used only as a
	 * comparison token against the create-time value. */
	return (void *)(uintptr_t)pthread_self();
}

void
p9_os_yield(void)
{
	struct timespec ts = { 0, 1000000 };
	nanosleep(&ts, NULL);
}

void
p9_os_delay_ms(uint32_t ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

uint64_t
p9_os_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
