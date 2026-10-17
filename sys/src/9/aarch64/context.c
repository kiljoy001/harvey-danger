/*
 * This file is part of the Harvey operating system. It is subject to the
 * GNU GPL v2 in the top-level LICENSE.gpl file.
 * Exercise the same Label switch primitive that port/proc.c calls.
 */
#include "context.h"

enum {
	Stacksize = 16 * 1024,
};

static Label bootlabel, alabel, blabel;
static u8 astack[Stacksize] __attribute__((aligned(16)));
static u8 bstack[Stacksize] __attribute__((aligned(16)));
static volatile u32 phase;
static volatile usize asp, bsp;

static __attribute__((noreturn)) void
taska(void)
{
	register usize guard19 __asm__("x19") = 0x195aa55a195aa55aULL;
	register usize guard28 __asm__("x28") = 0x28a55aa528a55aa5ULL;

	if(phase != 0)
		for(;;)
			__asm__ volatile("wfi");
	__asm__ volatile("" : "+r"(guard19), "+r"(guard28));
	__asm__ volatile("mov %0, sp" : "=r"(asp));
	phase = 1;
	if(slim_setlabel(&alabel) == 0)
		gotolabel(&blabel);
	__asm__ volatile("" : "+r"(guard19), "+r"(guard28));
	if(phase != 2 || guard19 != 0x195aa55a195aa55aULL ||
	   guard28 != 0x28a55aa528a55aa5ULL)
		for(;;)
			__asm__ volatile("wfi");
	phase = 3;
	gotolabel(&bootlabel);
}

static __attribute__((noreturn)) void
taskb(void)
{
	if(phase != 1)
		for(;;)
			__asm__ volatile("wfi");
	__asm__ volatile("mov %0, sp" : "=r"(bsp));
	phase = 2;
	gotolabel(&alabel);
}

int
arm64contextprobe(void)
{
	phase = 0;
	asp = bsp = 0;
	alabel.sp = (usize)(astack+Stacksize);
	alabel.pc = (usize)taska;
	blabel.sp = (usize)(bstack+Stacksize);
	blabel.pc = (usize)taskb;
	if(slim_setlabel(&bootlabel) == 0)
		gotolabel(&alabel);
	return phase == 3 &&
	       asp >= (usize)astack && asp <= (usize)(astack+Stacksize) &&
	       bsp >= (usize)bstack && bsp <= (usize)(bstack+Stacksize) &&
	       asp != bsp ? 0 : -1;
}
