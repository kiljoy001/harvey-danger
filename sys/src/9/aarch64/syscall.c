/* AArch64 register syscall entry for Harvey user processes. */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "sys.h"
#include "user.h"
#include <ureg.h>

static void
unpackregs(Ureg *u, Arm64ureg *r)
{
	memmove(&u->x0, r->x, 31*sizeof(u64));
	u->sp = r->sp0;
	u->type = r->esr;
	u->psr = r->spsr;
	u->pc = r->elr;
}

static void
packregs(Arm64ureg *r, Ureg *u)
{
	memmove(r->x, &u->x0, 31*sizeof(u64));
	r->sp0 = u->sp;
	r->spsr = u->psr;
	r->elr = u->pc;
}

void
arm64syscall(Arm64ureg *raw)
{
	Proc *up;
	Ureg u;
	Ar0 ar0;
	char *e;
	u32 number;
	usize usp;

	up = externup();
	if(up == nil)
		panic("ARM64 syscall without process");
	unpackregs(&u, raw);
	number = u.x8;
	usp = u.sp;
	up->dbgreg = &u;
	up->pc = u.pc;
	up->scallnr = number;
	up->insyscall = 1;
	up->nsyscall++;
	up->nqsyscall++;
	machp()->syscall++;
	up->nerrlab = 0;
	memset(&ar0, 0, sizeof(ar0));
	if(waserror()){
		e = up->syserrstr;
		up->syserrstr = up->errstr;
		up->errstr = e;
		if(number < nsyscall)
			ar0 = systab[number].r;
		else
			ar0.vl = -1;
	}else{
		if(number >= nsyscall || systab[number].f == nil)
			error(Ebadarg);
		up->psstate = systab[number].n;
		spllo();
		systab[number].f(&ar0, u.x0, u.x1, u.x2, u.x3, u.x4, u.x5);
		poperror();
	}
	splhi();
	u.x0 = ar0.p;
	up->insyscall = 0;
	up->psstate = nil;
	up->dbgreg = nil;
	/*
	 * Plan 9 preempts on the syscall return path.  Without this the
	 * port never reschedules a user-mode process that keeps entering
	 * the kernel (any tight host-call loop, e.g. a guest busy-wait),
	 * so it starves every other process and every tsleep wakeup.
	 */
	if(up->delaysched && up->state == Running)
		sched();
	packregs(raw, &u);
}
