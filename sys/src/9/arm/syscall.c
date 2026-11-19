/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in the top-level LICENSE.gpl file.
 *
 * Sycall entry and the user-mode trampoline for ARMv7.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "arm.h"
#include "ureg.h"
#include "sys.h"

void
arm32syscall(Ureg *u)
{
	Proc *up;
	Ar0 ar0;
	char *e;
	u32 number;

	up = externup();
	if(up == nil)
		panic("syscall without a process");
	number = u->r7;
	if(number == 8)		/* exits */
		diag("arm32: user process called exits(%d)\n", (int)u->r0);
	up->dbgreg = u;
	up->pc = u->pc;
	up->scallnr = number;
	up->insyscall = 1;
	up->nsyscall++;
	up->nqsyscall++;
	machp()->syscall++;
	up->nerrlab = 0;
	memset(&ar0, 0, sizeof ar0);
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
		systab[number].f(&ar0, u->r0, u->r1, u->r2, u->r3, u->r4, u->r5);
		poperror();
	}
	splhi();
	u->r0 = ar0.p;
	up->insyscall = 0;
	up->psstate = nil;
	up->dbgreg = nil;
	if(up->delaysched && up->state == Running)
		sched();
}
