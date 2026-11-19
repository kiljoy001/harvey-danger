/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * The kernel libc's formatting code takes a libc Lock (two words).  Until
 * the real kernel lock (port/taslock.c, which sleeps) is linked, use a
 * spinning lock with interrupts masked.  The layout matches sys/include/libc.h.
 */

typedef int i32;

struct LibcLock {
	i32 key;
	i32 sem;
};

void
lock(struct LibcLock *l)
{
	int i;

	while(__atomic_exchange_n(&l->key, 1, __ATOMIC_ACQUIRE)){
		for(i = 0; i < 100; i++)
			__asm__ volatile("" ::: "memory");
	}
}

void
unlock(struct LibcLock *l)
{
	__atomic_store_n(&l->key, 0, __ATOMIC_RELEASE);
}

int
canlock(struct LibcLock *l)
{
	return __atomic_exchange_n(&l->key, 1, __ATOMIC_ACQUIRE) == 0;
}

int
_tas(int *p)
{
	return __atomic_exchange_n(p, 1, __ATOMIC_ACQUIRE);
}

int
_tas32(int *p)
{
	return __atomic_exchange_n(p, 1, __ATOMIC_ACQUIRE);
}
