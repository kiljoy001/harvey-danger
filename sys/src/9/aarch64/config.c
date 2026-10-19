/* ARM64 microkernel: only core namespace devices and 9P transports. */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/sd.h"
#include "../ip/ip.h"

extern Dev rootdevtab, consdevtab, envdevtab, pipedevtab, procdevtab;
extern Dev mntdevtab, srvdevtab, dupdevtab, capdevtab, segmentdevtab;
extern Dev ramdevtab, ringdevtab, virtio9pdevtab, pnpdevtab;

Dev *devtab[] = {
	&rootdevtab, &consdevtab, &envdevtab, &pipedevtab,
	&procdevtab, &mntdevtab, &srvdevtab, &dupdevtab,
	&capdevtab, &segmentdevtab, &ramdevtab, &ringdevtab,
	&virtio9pdevtab, &pnpdevtab,
	nil,
};

/* The shared port objects still refer to these registration tables. */
void (*ipprotoinit[])(Fs *) = { nil };
SDifc *sdifc[] = { nil };
PhysUart *physuart[] = { nil };

/* Profiling is a user-space service, so the shared clock has no profiler. */
void
oprof_alarm_handler(Ureg *u)
{
	USED(u);
}
Physseg physseg[128] = {
	{ .attr = SG_SHARED|SG_READ|SG_WRITE, .name = "shared", .size = SEGMAXPG },
	{ .attr = SG_BSS|SG_READ|SG_WRITE, .name = "memory", .size = SEGMAXPG },
};
int nphysseg = 128;
int cpuserver = 1;
/* The host ring is supplied by the VM transport when one is available. */
u64 ringhostpa;
int printallsyscalls;
u32 kerndate = KERNDATE;
Conf conf;
char *cputype = "aarch64";
char *rolename[] = { "TC", "KC", "AC", "XC" };

void
links(void)
{
}
