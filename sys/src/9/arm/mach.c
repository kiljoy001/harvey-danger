/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in the top-level LICENSE.gpl file.
 *
 * Per-machine state for the shared scheduler.  This port is uniprocessor
 * for now, so the current Mach is a global rather than a register.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

Mach arm32machs[MACHMAX];
static Sys bootsys = {
	.nmach = 1,
	.machptr = { &arm32machs[0] },
	.npgsz = NPGSZ,
	.pgsz = { PGSZ, BIGPGSZ },
};
Sys *sys = &bootsys;
Active active;
Conf conf;

char *cputype = "arm32";
char *rolename[] = { "TC", "KC", "AC", "XC" };
int cpuserver = 1;
u32 kerndate = 1;

static Mach *mach0;

Mach *
machp(void)
{
	return mach0;
}

void
arm32setmach(Mach *m)
{
	mach0 = m;
}

int
corecolor(int machno)
{
	USED(machno);
	return 0;
}

int
ainc(int *p)
{
	return __atomic_add_fetch(p, 1, __ATOMIC_ACQ_REL);
}

int
adec(int *p)
{
	return __atomic_sub_fetch(p, 1, __ATOMIC_ACQ_REL);
}

int
incref(Ref *r)
{
	return __atomic_add_fetch(&r->ref, 1, __ATOMIC_ACQ_REL);
}

int
decref(Ref *r)
{
	return __atomic_sub_fetch(&r->ref, 1, __ATOMIC_ACQ_REL);
}
