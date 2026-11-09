/* ARM64 register layout used by /proc, exec, and rfork. */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "ureg.h"
#include "tos.h"
#include "user.h"

extern void arm64enterureg(Ureg *) __attribute__((noreturn));

/* argc saved by sysexecstack: after the exec stack is relocated and the
 * mmu is flushed, the argument count is no longer readable from the stack,
 * so it is kept on the Proc for sysexecregs.  A static does not work under
 * SMP: two CPUs (or a preempted syscall) executing a program at the same
 * time overwrite each other's count, and the victim then loses its last
 * argv entry. */

_Static_assert(__builtin_offsetof(Ureg, sp) == 248, "ARM64 SP frame offset");
_Static_assert(__builtin_offsetof(Ureg, psr) == 264, "ARM64 PSR frame offset");
_Static_assert(__builtin_offsetof(Ureg, pc) == 272, "ARM64 PC frame offset");

int
userureg(Ureg *r)
{
	return (r->psr & 15) == 0;
}

usize
userpc(Ureg *r)
{
	Proc *p;

	if(r == nil){
		p = externup();
		if(p == nil)
			return 0;
		r = p->dbgreg;
		if(r == nil)
			return 0;
	}
	return r->pc;
}

usize
dbgpc(Proc *p)
{
	return p->dbgreg == nil ? 0 : ((Ureg *)p->dbgreg)->pc;
}

void
setkernur(Ureg *r, Proc *p)
{
	r->pc = p->sched.pc;
	r->sp = p->sched.sp;
	r->psr = 0x3c5; /* EL1h, interrupts masked. */
}

void
setregisters(Ureg *r, char *target, char *source, int n)
{
	u64 psr, type;

	psr = r->psr;
	type = r->type;
	memmove(target, source, n);
	/* /proc/regs must not be able to raise a task to EL1. */
	r->psr = (r->psr & ~15ULL) | (psr & 15);
	r->type = type;
}

void
ureg2gdb(Ureg *r, usize *g)
{
	usize *x;
	int i;

	x = &r->x0;
	for(i = 0; i < 31; i++)
		g[i] = x[i];
	g[31] = r->sp;
	g[32] = r->pc;
	g[33] = r->psr;
}

void
dumpgpr(Ureg *r)
{
	usize *x;
	int i;

	x = &r->x0;
	for(i = 0; i < 31; i += 2)
		print("x%d %#p  x%d %#p\n", i, (void *)x[i],
			i+1, (void *)(i+1 < 31 ? x[i+1] : 0));
	print("pc %#p sp %#p psr %#llx\n", (void *)r->pc,
		(void *)r->sp, r->psr);
}

usize
sysexecstack(usize stack, int argc)
{
	Proc *p;
	usize sp;

	p = externup();
	p->execargc = argc;
	/* sysproc pushes argc, argv pointers, and a nil after this point. */
	sp = stack & ~(usize)15;
	if(argc & 1)
		sp -= 8;
	return sp;
}

void *
sysexecregs(usize entry, u32 ssize, void *tos)
{
	Proc *p;
	Ureg *r;
	usize sp;

	p = externup();
	r = p->dbgreg;
	sp = USTKTOP - ssize;
	if((sp & 15) != 0 || r == nil)
		panic("ARM64 exec: invalid user stack or registers");
	memset(r, 0, sizeof(*r));
	r->sp = sp;
	r->pc = entry;
	r->psr = 0x340; /* EL0t, IRQ unmasked, FP disabled. */
	r->x0 = p->execargc;
	r->x1 = sp + sizeof(usize);
	r->x2 = (usize)tos;
	/* Return argc so the generic syscall return (u.x0 = ar0.p) keeps the
	 * argc value in x0 instead of clobbering it with the tos address. */
	return (void *)(usize)p->execargc;
}

void
sysprocsetup(Proc *p)
{
	/* exec starts with the default FP/SIMD state. */
	memset(&p->FPU, 0, sizeof p->FPU);
}

void
arm64settls(usize tls)
{
	__asm__ volatile("msr tpidr_el0, %0" :: "r"((u64)tls) : "memory");
}

static void
forkreturn(void)
{
	Proc *p;

	p = externup();
	if(p == nil || p->dbgreg == nil)
		panic("ARM64 fork: missing child register frame");
	arm64settls(p->tls);
	arm64enterureg(p->dbgreg);
}

void
sysrforkchild(Proc *child, Proc *parent)
{
	Ureg *r;

	if(parent->dbgreg == nil)
		panic("ARM64 fork: missing parent register frame");
	r = (Ureg *)STACKALIGN((usize)(child->kstack + KSTACK - sizeof(Ureg)));
	memmove(r, parent->dbgreg, sizeof(*r));
	r->x0 = 0;
	child->dbgreg = r;
	child->sched.sp = (usize)r;
	child->sched.pc = (usize)forkreturn;
	/* The child starts with the parent's live FP/SIMD registers. */
	arm64fpsave(parent);
	child->FPU = parent->FPU;
}

void
getcallstack(usize *pcs, usize n)
{
	usize fp, pc;
	int got, i;

	__asm__ volatile("mov %0, x29" : "=r"(fp));
	pc = (usize)__builtin_return_address(0);
	got = backtrace_list(pc, fp, pcs, n);
	for(i = got; i < n; i++)
		pcs[i] = 0;
}
