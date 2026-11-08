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
extern Dev etherdevtab, ipdevtab;
extern Dev vopdevtab, xhcidevtab;

Dev *devtab[] = {
	&rootdevtab, &consdevtab, &envdevtab, &pipedevtab,
	&procdevtab, &mntdevtab, &srvdevtab, &dupdevtab,
	&capdevtab, &segmentdevtab, &ramdevtab, &ringdevtab,
	&virtio9pdevtab, &pnpdevtab, &vopdevtab, &xhcidevtab,
	&etherdevtab, &ipdevtab,
	nil,
};

/* Protocol initialisation order for the kernel IP stack. */
extern void tcpinit(Fs*);
extern void udpinit(Fs*);
extern void rudpinit(Fs*);
extern void ipifcinit(Fs*);
extern void icmpinit(Fs*);
extern void icmp6init(Fs*);
extern void greinit(Fs*);
extern void ipmuxinit(Fs*);
extern void ilinit(Fs*);

void (*ipprotoinit[])(Fs *) = {
	tcpinit,
	udpinit,
	rudpinit,
	ipifcinit,
	icmpinit,
	icmp6init,
	greinit,
	ipmuxinit,
	ilinit,
	nil,
};
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
/* Reserved physically contiguous DMA arena, published through #P/dma. */
u64 arm64dmabase;
u64 arm64dmasize = 512*1024*1024;
/* Vendor RK3566 OP-TEE static NSEC SHM, published through #P/tee. */
u64 arm64teeshmbase;
u64 arm64teeshmsize;
int printallsyscalls;
u32 kerndate = KERNDATE;
Conf conf;
char *cputype = "aarch64";
char *rolename[] = { "TC", "KC", "AC", "XC" };

void
links(void)
{
}
