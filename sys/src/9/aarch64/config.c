/* ARM64 kernel configuration for the shared Plan 9 devices and IP stack. */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/sd.h"
#include "../ip/ip.h"

extern Dev rootdevtab, consdevtab, envdevtab, pipedevtab, procdevtab;
extern Dev mntdevtab, srvdevtab, dupdevtab, capdevtab, segmentdevtab;
extern Dev ramdevtab, ipdevtab, sddevtab, uartdevtab, tlsdevtab;
extern Dev ssldevtab, kprofdevtab, wddevtab;
extern Dev ringdevtab;
extern Dev fdmuxdevtab, mntndevtab, tracedevtab, wsdevtab;
extern Dev regressdevtab, kbindevtab;
extern void tcpinit(Fs *), udpinit(Fs *), icmpinit(Fs *), icmp6init(Fs *);
extern void greinit(Fs *), ipifcinit(Fs *);

Dev *devtab[] = {
	&rootdevtab, &consdevtab, &envdevtab, &pipedevtab,
	&procdevtab, &mntdevtab, &srvdevtab, &dupdevtab,
	&capdevtab, &segmentdevtab, &ramdevtab, &ipdevtab,
	&sddevtab, &uartdevtab, &tlsdevtab, &ssldevtab,
	&kprofdevtab, &wddevtab, &ringdevtab, &fdmuxdevtab,
	&mntndevtab, &tracedevtab, &wsdevtab, &regressdevtab,
	&kbindevtab, nil,
};

void (*ipprotoinit[])(Fs *) = {
	tcpinit, udpinit, icmpinit, icmp6init, greinit, ipifcinit, nil,
};
SDifc *sdifc[] = { nil };
PhysUart *physuart[] = { nil };
Physseg physseg[8] = {
	{ .attr = SG_SHARED|SG_READ|SG_WRITE, .name = "shared", .size = SEGMAXPG },
	{ .attr = SG_BSS|SG_READ|SG_WRITE, .name = "memory", .size = SEGMAXPG },
};
int nphysseg = 8;
int cpuserver = 1;
/* The host ring is supplied by the VM transport when one is available. */
u64 ringhostpa;
int printallsyscalls;
u32 kerndate = KERNDATE;
Conf conf;
char *cputype = "arm64";
char *rolename[] = { "TC", "KC", "AC", "XC" };

void
links(void)
{
}
