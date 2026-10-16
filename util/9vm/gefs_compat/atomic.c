#include <u.h>
#include <libc.h>

#include "atomic.h"

#define Get(type, name) \
type name(type *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

#define Set(type, name) \
type name(type *p, type v) { return __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL); }

#define Inc(type, name) \
type name(type *p, type v) { return __atomic_add_fetch(p, v, __ATOMIC_ACQ_REL); }

#define Cas(type, name) \
int name(type *p, type old, type newv) { \
	return __atomic_compare_exchange_n(p, &old, newv, 0, \
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); \
}

Get(long, agetl)
Get(vlong, agetv)
Get(void*, agetp)
Set(long, asetl)
Set(vlong, asetv)
Set(void*, asetp)
Inc(long, aincl)
Inc(vlong, aincv)
Cas(long, acasl)
Cas(vlong, acasv)
Cas(void*, acasp)

void
coherence(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}
