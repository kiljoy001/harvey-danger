/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in the top-level LICENSE.gpl file.
 *
 * Process-side architecture: the primitive that enters a kernel process,
 * the MMU hooks (still identity-mapped: user segments do not exist yet)
 * and the idle instruction.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "arm.h"
#include "ureg.h"

void
procsave(Proc *p)
{
	USED(p);
	/* The kernel is soft-float today; VFP state arrives with user mode. */
}

void
procrestore(Proc *p)
{
	USED(p);
}

static void
linkproc(void)
{
	Proc *up;

	up = externup();
	spllo();
	up->kpfun(up->kparg);
	pexit("kproc dying", 0);
	for(;;)
		__asm__ volatile("wfi");
}

void
kprocchild(Proc *p, void (*func)(void *), void *arg)
{
	p->kpfun = func;
	p->kparg = arg;
	labelstart(&p->sched, linkproc, (void*)STACKALIGN((usize)(p->kstack + KSTACK)));
}

void
idlehands(void)
{
	__asm__ volatile("wfi");
}

void
mmuflushtlb(void)
{
	arm32tlbflush();
}

void
mmuprocinit(Proc *p)
{
	p->MMU.mmul1pa = 0;
	p->MMU.ptpages = nil;
	memset(&p->FPU, 0, sizeof p->FPU);
}

void
mmuswitch(Proc *p)
{
	arm32mmuswitch(p->MMU.mmul1pa);
}

void
mmurelease(Proc *p)
{
	arm32mmurelease(p);
}

void
mmuidle(void)
{
	arm32mmuswitch(0);
}

usize
userpc(Ureg *u)
{
	return u == nil ? 0 : u->pc;
}

usize
dbgpc(Proc *p)
{
	return p->dbgreg == nil ? 0 : ((Ureg*)p->dbgreg)->pc;
}

void
setkernur(Ureg *r, Proc *p)
{
	r->pc = 0;
	r->sp = p->sched.sp;
	r->psr = PsrMsvc | PsrDirq | PsrDfiq;
}

void
setregisters(Ureg *r, char *target, char *source, int n)
{
	u32 psr;

	psr = r->psr;
	memmove(target, source, n);
	/* /proc/regs must not be able to change the mode. */
	r->psr = (r->psr & ~0x1f) | (psr & 0x1f);
}

void
ureg2gdb(Ureg *r, usize *g)
{
	int i;

	for(i = 0; i < 16; i++)
		g[i] = ((u32*)r)[i];
	g[16] = r->psr;
}
