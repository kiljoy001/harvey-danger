/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in the top-level LICENSE.gpl file.
 *
 * GICv2 and the ARM virtual timer for QEMU's 32-bit "virt" board.  The
 * timer is the port clock's hardware clock: it fires timerintr(), which
 * runs hzclock() and any expired timers, and timerset() reprograms the
 * compare value from the fastticks deadline.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "arm.h"
#include "ureg.h"

enum {
	GicdCtlr	= 0x000,
	GicdIsenabler	= 0x100,
	GicdIpriority	= 0x400,
	GicdItargets	= 0x800,
	GiccCtlr	= 0x000,
	GiccPmr		= 0x004,
	GiccIar		= 0x00c,
	GiccEoir	= 0x010,

	Timerirq	= 27,		/* virtual timer, PPI */
};

#define gicd(r)	(*(volatile u32*)((u32)GICD + (r)))
#define gicc(r)	(*(volatile u32*)((u32)GICC + (r)))

static u32 timerfreq;
static u32 probeinterval;
static int portclock;

/*
 * The generic counter is not readable in this guest, so the clock is
 * software: every timer interrupt adds the interval that was programmed
 * to fasttime, giving fastticks() a monotonic microsecond value.
 */
static u64 fasttime;
static u32 lastus;

u64
arm32fastnow(void)
{
	return fasttime;
}

void
arm32clocktick(void)
{
	fasttime += lastus;
}

void
arm32setportclock(int on)
{
	portclock = on;
}

int
arm32portclock(void)
{
	return portclock;
}

void
arm32gicinit(void)
{
	gicd(GicdCtlr) = 0;
	gicc(GiccCtlr) = 0;

	gicd(GicdIpriority + (Timerirq & ~3)) = 0xa0a0a0a0;
	gicd(GicdItargets + (Timerirq & ~3)) = 0x01010101;

	gicc(GiccPmr) = 0xf0;
	gicc(GiccCtlr) = 1;
	gicd(GicdCtlr) = 1;

	gicd(GicdIsenabler) = 1 << Timerirq;
}

void
arm32timerinit(void)
{
	timerfreq = arm32cntfrq();
	probeinterval = timerfreq / HZ;
	lastus = 1000000 / HZ;
	arm32setcntvtval(probeinterval);
	arm32setcntvctl(1);
}

/*
 * timerset is called with a fastticks() deadline (microseconds).  Program
 * the virtual timer for the delta, and let the interrupt handler tick.
 */
void
timerset(u64 when)
{
	u64 hz, now, delta, ticks;

	now = fastticks(&hz);
	if(hz == 0 || timerfreq == 0)
		return;
	if(when <= now)
		delta = 1;
	else
		delta = when - now;
	/* microseconds -> counter ticks, rounded up */
	ticks = (delta * timerfreq + 999999) / 1000000;
	if(ticks == 0)
		ticks = 1;
	/*
	 * Remember the interval just programmed: the next interrupt adds
	 * exactly this much to the software clock.
	 */
	lastus = (u32)delta;
	arm32setcntvtval((u32)ticks);
	arm32setcntvctl(1);
}

void
arm32timerirq(void)
{
	timerintr(nil, 0);
}

/* Reprogram the fixed probe interval (before the port clock takes over). */
void
arm32timerprobe(void)
{
	lastus = 1000000 / HZ;
	arm32setcntvtval(probeinterval);
}
