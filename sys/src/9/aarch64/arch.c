/*
 * This file is part of the Harvey operating system. It is subject to the
 * GNU GPL v2 in the top-level LICENSE.gpl file.
 * Initial one-core hooks for Harvey's shared process scheduler.
 */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "mmu.h"

static Mach bootmach;
static Sys bootsys = {
	.nmach = 1,
	.machptr = {&bootmach},
	.npgsz = NPGSZ,
	.pgsz = { PGSZ, BIGPGSZ },
};
Sys *sys = &bootsys;
Active active;

Mach *
machp(void)
{
	return &bootmach;
}

void
cycles(u64 *t)
{
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(*t));
}

int
corecolor(int machno)
{
	return machno == 0 ? 0 : -1;
}

/* Kernel SIMD use is disabled until FP state save/restore is implemented. */
void
procsave(Proc *p)
{
	u64 t;

	cycles(&t);
	p->pcycles += t;
}

void
procrestore(Proc *p)
{
	u64 t;

	if(p->kp)
		return;
	cycles(&t);
	p->pcycles -= t;
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
	p->sched.pc = (usize)linkproc;
	p->sched.sp = STACKALIGN((usize)(p->kstack + KSTACK));
	p->kpfun = func;
	p->kparg = arg;
}

void
idlehands(void)
{
	__asm__ volatile("wfi");
}

void
mmuflushtlb(void)
{
	__asm__ volatile("dsb ishst; tlbi vmalle1; dsb ish; isb" ::: "memory");
}

void
mmuprocinit(Proc *p)
{
	p->MMU.rootpa = 0;
	p->MMU.ptpages = nil;
}

void
mmuswitch(Proc *p)
{
	u64 root;

	root = p->MMU.rootpa;
	if(root == 0)
		root = arm64mmukernelroot();
	if(root == 0)
		panic("mmuswitch: no kernel page table");
	arm64mmuswitch(root);
}

void
mmurelease(Proc *p)
{
	arm64mmurelease(p);
}

int
infected_with_std(void)
{
	return 0;
}

void
disinfect_std(void)
{
}

int
incref(Ref *r)
{
	int n;

	lock(&r->l);
	n = ++r->ref;
	unlock(&r->l);
	return n;
}

int
decref(Ref *r)
{
	int n;

	lock(&r->l);
	n = --r->ref;
	unlock(&r->l);
	if(n < 0)
		panic("decref: negative reference count");
	return n;
}
