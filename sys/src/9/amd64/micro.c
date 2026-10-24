#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "io.h"

extern Dev rootdevtab;
extern Dev consdevtab;
extern Dev archdevtab;
extern Dev envdevtab;
extern Dev pipedevtab;
extern Dev procdevtab;
extern Dev mntdevtab;
extern Dev srvdevtab;
extern Dev dupdevtab;
extern Dev capdevtab;
extern Dev segmentdevtab;
extern Dev virtio9pdevtab;
extern Dev ringdevtab;
extern Dev ramdevtab;
extern Dev xhcidevtab;
extern Dev sddevtab;

Dev *devtab[] = {
	&rootdevtab,
	&consdevtab,
	&archdevtab,
	&envdevtab,
	&pipedevtab,
	&procdevtab,
	&mntdevtab,
	&srvdevtab,
	&dupdevtab,
	&capdevtab,
	&segmentdevtab,
	&virtio9pdevtab,
	&ringdevtab,
	&ramdevtab,
	&xhcidevtab,
	&sddevtab,
	nil,
};

void
links(void)
{
}

#include "../port/sd.h"
extern SDifc sdvirtioifc;

SDifc* sdifc[] = {
	&sdvirtioifc,
	nil,
};

PhysUart* physuart[] = {
	nil,
};

Physseg physseg[8] = {
	{
		.attr = SG_SHARED|SG_READ|SG_WRITE,
		.name = "shared",
		.size = SEGMAXPG,
	},
	{
		.attr = SG_BSS|SG_READ|SG_WRITE,
		.name = "memory",
		.size = SEGMAXPG,
	},
};
int nphysseg = 8;

int cpuserver = 1;
u32 kerndate = 1;

char* conffile = "micro";

/* Stubs for devices stripped from headless microkernel */
void keybinit(void) {}
void keybenable(void) {}
void mouseenable(void) {}
void virtiosetup(void) {}

i32
mtagsread(Chan *c, void *buf, i32 n, i64 off)
{
	USED(c);
	return readstr(off, buf, n, "virtio9p:0:32768:0:0:0\n");
}
