/*
 * This file is part of the Harvey operating system. It is subject to the
 * GNU GPL v2 in the top-level LICENSE.gpl file.
 *
 * The kernel's device table and physical segment list.  The bring-up has
 * only the console; more devices arrive with their drivers.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "ureg.h"
#include "../port/error.h"
#include "../port/sd.h"
#include "../ip/ip.h"

extern Dev consdevtab, dupdevtab, envdevtab, pipedevtab, procdevtab;
extern Dev mntdevtab, srvdevtab, rootdevtab, segmentdevtab, ramdevtab;

Dev *devtab[] = {
	&consdevtab,
	&dupdevtab,
	nil,
};

SDifc *sdifc[] = { nil };
PhysUart *physuart[] = { nil };
void (*ipprotoinit[])(Fs*) = { nil };

Physseg physseg[8] = {
	{ .attr = SG_SHARED|SG_READ|SG_WRITE, .name = "shared", .size = SEGMAXPG },
	{ .attr = SG_BSS|SG_READ|SG_WRITE, .name = "memory", .size = SEGMAXPG },
};
int nphysseg = 8;
int printallsyscalls;

void
links(void)
{
}

void
oprof_alarm_handler(Ureg *u)
{
	USED(u);
}
