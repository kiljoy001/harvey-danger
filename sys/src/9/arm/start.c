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

/*
 * User address space self-test: map one page at a user virtual address
 * and read it back through the process page table.
 */
static void
mmuprobe(void)
{
	Proc *up;
	Page *pg;
	u8 *k, *u;
	int ok;

	up = externup();
	pg = newpage(1, nil, 0, PGSZ, -1);
	if(pg == nil){
		printk("mmu user map FAILED (no page)\n");
		return;
	}
	k = KADDR(pg->pa);
	memset(k, 0x5a, PGSZ);
	mmuput(0x10000000, pg, PTEVALID|PTEWRITE);

	arm32mmuswitch(up->MMU.mmul1pa);
	u = (u8*)0x10000000;
	ok = u[0] == 0x5a && u[PGSZ/2] == 0x5a && u[PGSZ-1] == 0x5a;
	arm32mmuswitch(0);

	printk("mmu user map %s\n", ok? "ok": "FAILED");
}

extern char arm32userprog_start[], arm32userprog_end[];

/*
 * Copy the user program into a page, give it a stack and enter user mode.
 * The process should come back through exits(0).
 */
static void
userprobe(void *arg)
{
	Proc *up;
	Page *code, *stack;
	u32 n;

	USED(arg);

	up = externup();
	code = newpage(1, nil, 0, PGSZ, -1);
	stack = newpage(1, nil, 0, PGSZ, -1);
	if(code == nil || stack == nil){
		printk("user probe FAILED (no pages)\n");
		return;
	}
	n = arm32userprog_end - arm32userprog_start;
	memmove(KADDR(code->pa), arm32userprog_start, n);
	mmuput(0x10000000, code, PTEVALID|PTEWRITE);
	mmuput(0x3ff00000, stack, PTEVALID|PTEWRITE);

	print("user probe: entering user mode\n");
	arm32mmuswitch(up->MMU.mmul1pa);
	arm32enteruser(0x10000000, 0x3ff00000 + PGSZ - 16);
	print("user probe: returned from user mode\n");
}

static void
ktest(void *arg)
{
	Proc *up = externup();
	int i;

	USED(arg);
	mmuprobe();
	for(i = 0; i < 3; i++){
		print("arm32: scheduler alive pid %d ticks %lud\n",
			up->pid, sys->ticks);
		delay(100);
	}
	/* Ends with exits(0) from user mode. */
	userprobe(nil);
	print("arm32: user probe returned\n");
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
