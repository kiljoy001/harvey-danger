/*
 * Plan 9 atomic accessors for the ARM64 userland build.
 *
 * gefs uses the Along/Avlong/Aptr API from 9front, which that libc
 * implements in assembly.  Here the same semantics are expressed with
 * the compiler's atomic builtins, which AArch64 lowers to LDXR/STXR or
 * the LSE instructions.
 */
#include <u.h>
#include <libc.h>

long
agetl(Along *p)
{
	return __atomic_load_n(&p->v, __ATOMIC_ACQUIRE);
}

vlong
agetv(Avlong *p)
{
	return __atomic_load_n(&p->v, __ATOMIC_ACQUIRE);
}

void *
agetp(Aptr *p)
{
	return __atomic_load_n(&p->v, __ATOMIC_ACQUIRE);
}

long
aswapl(Along *p, long v)
{
	return __atomic_exchange_n(&p->v, v, __ATOMIC_SEQ_CST);
}

vlong
aswapv(Avlong *p, vlong v)
{
	return __atomic_exchange_n(&p->v, v, __ATOMIC_SEQ_CST);
}

void *
aswapp(Aptr *p, void *v)
{
	return __atomic_exchange_n(&p->v, v, __ATOMIC_SEQ_CST);
}

/* Return the value after the increment, matching 9front's aincl. */
long
aincl(Along *p, long n)
{
	return __atomic_add_fetch(&p->v, n, __ATOMIC_SEQ_CST);
}

vlong
aincv(Avlong *p, vlong n)
{
	return __atomic_add_fetch(&p->v, n, __ATOMIC_SEQ_CST);
}

/* Return 1 on success and 0 when the value did not match. */
int
acasl(Along *p, long old, long new)
{
	return __atomic_compare_exchange_n(&p->v, &old, new, 0,
					   __ATOMIC_SEQ_CST,
					   __ATOMIC_SEQ_CST);
}

int
acasv(Avlong *p, vlong old, vlong new)
{
	return __atomic_compare_exchange_n(&p->v, &old, new, 0,
					   __ATOMIC_SEQ_CST,
					   __ATOMIC_SEQ_CST);
}

int
acasp(Aptr *p, void *old, void *new)
{
	return __atomic_compare_exchange_n(&p->v, &old, new, 0,
					   __ATOMIC_SEQ_CST,
					   __ATOMIC_SEQ_CST);
}

void
coherence(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}
