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

Mach arm64machs[MACHMAX];
static Sys bootsys = {
	.nmach = 1,
	.machptr = {&arm64machs[0]},
	.npgsz = NPGSZ,
	.pgsz = { PGSZ, BIGPGSZ },
};
Sys *sys = &bootsys;
Active active;

Mach *
machp(void)
{
	u64 m;

	__asm__ volatile("mrs %0, tpidr_el1" : "=r"(m));
	return (Mach *)m;
}

void
arm64setmach(Mach *m)
{
	__asm__ volatile("msr tpidr_el1, %0" :: "r"((u64)m) : "memory");
}

/* Used by the early boot code, which does not include the port headers. */
void
arm64machinit(void)
{
	arm64setmach(&arm64machs[0]);
}

void
cycles(u64 *t)
{
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(*t));
}

int
corecolor(int machno)
{
	USED(machno);
	return 0;
}

/*
 * User FP/SIMD state.  The kernel is compiled with -mgeneral-regs-only,
 * so q0-q31, FPCR and FPSR belong to the current user process and must be
 * saved whenever that process leaves a CPU.  Without this a process that
 * migrates to another CPU inherits the previous occupant's registers.
 */
static u64 fpusave[MACHMAX][66] __attribute__((aligned(16)));

void
arm64fpsave(Proc *p)
{
	u64 *s;
	int m;

	m = machp()->machno;
	s = fpusave[m];
	arm64fpstore(s);
	memmove(&p->FPU, s, sizeof(PFPU));
}

void
arm64fprestore(Proc *p)
{
	u64 *s;
	int m;

	m = machp()->machno;
	s = fpusave[m];
	memmove(s, &p->FPU, sizeof(PFPU));
	arm64fpload(s);
}

void
procsave(Proc *p)
{
	u64 t;

	cycles(&t);
	p->pcycles += t;
	if(p->kp)
		return;
	arm64fpsave(p);
}

void
procrestore(Proc *p)
{
	u64 t;

	if(p->kp)
		return;
	arm64fprestore(p);
	/*
	 * TPIDR_EL0 is per-CPU, not part of the trap frame.  A process that
	 * was preempted by the timer and resumes through the IRQ path never
	 * goes through arm64usertrap(), so restore its TLS pointer here.
	 */
	arm64settls(p->tls);
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
	memset(&p->FPU, 0, sizeof p->FPU);
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
