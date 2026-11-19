/*
 * This file is part of the Harvey operating system. It is subject to the
 * GNU GPL v2 in the top-level LICENSE.gpl file.
 *
 * Kernel start-up, in the order the shared port layer expects:
 * allocator, process table, console, clock, devices, page cache, and
 * then the first kernel process handed to the scheduler.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "arm.h"
#include "ureg.h"

enum {
	Ramsize = 512 * MiB,
};

static int portclock;

static void
ktest(void *arg)
{
	USED(arg);
	for(;;){
		Proc *up = externup();

		print("arm32: scheduler alive pid %d ticks %lud\n",
			up->pid, sys->ticks);
		delay(1000);
	}
}

void
arm32kernelinit(void)
{
	Proc *p;

	arm32physinit(PHYSDRAM + Ramsize);
	conf.npage = Ramsize / PGSZ;
	conf.upages = arm32physfreepages();
	conf.nproc = 64;
	conf.nimage = 32;
	eve = "root";

	print("arm32: physinit done npage %lud upages %lud\n",
		conf.npage, conf.upages);

	procinit0();
	printinit();
	print("arm32: printinit done\n");
	timersinit();
	psinit(conf.nproc);
	print("arm32: psinit done\n");
	links();

	devtabreset();
	print("arm32: devtabreset done\n");
	pageinit();
	print("arm32: pageinit done\n");

	/*
	 * From here the timer interrupt belongs to the port clock: it
	 * updates sys->ticks and runs expired timers.
	 */
	portclock = 1;
	arm32setportclock(portclock);

	setmachsched(machp());

	p = newproc();
	p->kp = 1;
	p->noswap = 1;
	kprocchild(p, ktest, nil);
	ready(p);

	print("arm32: starting scheduler\n");
	schedinit();
}
