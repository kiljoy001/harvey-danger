/* Bring Harvey's shared process scheduler online after the ARM64 probes. */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "fdt.h"
#include "phys.h"

extern void arm64uartputs(char *);
extern void (*consuartputs)(char *, int);
extern void arm64clockready(void);
extern void arm64virtio9pconfigure(Fdtinfo *);
extern Dev rootdevtab;
extern Dev virtio9pdevtab;
extern Dev ringdevtab;

static Chan *
ringfile(Chan *root, char *name, int mode)
{
	Walkqid *walk;
	Chan *file;

	walk = ringdevtab.walk(root, nil, &name, 1);
	if(walk == nil || walk->nqid != 1 || walk->clone == nil)
		panic("ARM64 9ring walk failed");
	file = walk->clone;
	free(walk);
	ringdevtab.open(file, mode);
	return file;
}

static void
ringprobe(void)
{
	static u8 request[] = {
		19, 0, 0, 0, 100, 42, 0,
		0x18, 0, 1, 0, 6, 0, '9', 'P', '2', '0', '0', '0',
	};
	static u8 response[] = {
		19, 0, 0, 0, 101, 42, 0,
		0x18, 0, 1, 0, 6, 0, '9', 'P', '2', '0', '0', '0',
	};
	u8 received[32];
	Chan *server, *client, *announce, *connect, *listen;
	int n;

	server = ringdevtab.attach("!arm64probe");
	announce = ringfile(server, "announce", OREAD);
	client = ringdevtab.attach("arm64probe");
	connect = ringfile(client, "connect", ORDWR);
	listen = ringfile(server, "listen", ORDWR);
	n = ringdevtab.write(connect, request, sizeof(request), 0);
	if(n != sizeof(request))
		panic("ARM64 9ring request write failed");
	n = ringdevtab.read(listen, received, sizeof(received), 0);
	if(n != sizeof(request) || memcmp(received, request, n) != 0)
		panic("ARM64 9ring request invalid");
	n = ringdevtab.write(listen, response, sizeof(response), 0);
	if(n != sizeof(response))
		panic("ARM64 9ring response write failed");
	n = ringdevtab.read(connect, received, sizeof(received), 0);
	if(n != sizeof(response) || memcmp(received, response, n) != 0)
		panic("ARM64 9ring response invalid");
	cclose(listen);
	cclose(connect);
	cclose(client);
	cclose(announce);
	cclose(server);
	arm64uartputs("HARVEY-ARM64 9RING PASS\n");
}

static void
virtio9pprobe(void)
{
	static u8 badrequest[] = {8, 0, 0, 0, 100, 1, 0};
	static u8 request[] = {
		19, 0, 0, 0, 100, 255, 255,
		0x18, 0, 1, 0, 6, 0, '9', 'P', '2', '0', '0', '0',
	};
	u8 reply[64];
	Chan *root, *stream;
	Walkqid *walk;
	char *name;
	int n;
	Proc *up;

	root = virtio9pdevtab.attach("");
	name = "0";
	walk = virtio9pdevtab.walk(root, nil, &name, 1);
	if(walk == nil || walk->nqid != 1 || walk->clone == nil){
		if(walk != nil)
			free(walk);
		cclose(root);
		return;
	}
	stream = walk->clone;
	free(walk);
	cclose(root);
	virtio9pdevtab.open(stream, ORDWR);
	up = externup();
	if(waserror()){
		arm64uartputs("HARVEY-ARM64 VIRTIO-9P FRAMING PASS\n");
	}else{
		virtio9pdevtab.write(stream, badrequest, sizeof(badrequest), 0);
		poperror();
		panic("ARM64 virtio9p accepted invalid frame");
	}
	n = virtio9pdevtab.write(stream, request, sizeof(request), 0);
	if(n != sizeof(request))
		panic("ARM64 virtio9p Tversion write failed");
	n = virtio9pdevtab.read(stream, reply, sizeof(reply), 0);
	if(n < 13 || reply[4] != 101 || reply[5] != 255 || reply[6] != 255 ||
	   reply[0] != n)
		panic("ARM64 virtio9p Rversion invalid");
	cclose(stream);
	arm64uartputs("HARVEY-ARM64 VIRTIO-9P PASS\n");
}

static void
uartwrite(char *buf, int n)
{
	char c[2];

	c[1] = 0;
	while(n-- > 0){
		c[0] = *buf++;
		arm64uartputs(c);
	}
}

static volatile int workerphase;

static void
worker(void *arg)
{
	int id;

	id = (usize)arg;
	if(id == 1){
		workerphase |= 1;
		while(workerphase != 3)
			sched();
		arm64uartputs("HARVEY-ARM64 SCHEDULER PASS\n");
		while(sys->ticks < 3)
			sched();
		arm64uartputs("HARVEY-ARM64 CLOCK PASS\n");
		{
			Chan *root;
			Walkqid *walk;
			char *name;

			root = rootdevtab.attach("");
			name = "dev";
			walk = rootdevtab.walk(root, nil, &name, 1);
			if(walk == nil || walk->nqid != 1 || walk->clone == nil ||
			   (walk->qid[0].type & QTDIR) == 0)
				panic("ARM64 root namespace walk failed");
			cclose(walk->clone);
			free(walk);
			cclose(root);
			arm64uartputs("HARVEY-ARM64 ROOT-WALK PASS\n");
		}
		ringprobe();
		virtio9pprobe();
		arm64uartputs("HARVEY-ARM64 BOOT-PROBES PASS\n");
	}else{
		workerphase |= 2;
	}
}

static void
startworker(int id)
{
	Proc *p;

	p = newproc();
	kstrdup(&p->user, eve);
	p->kp = 1;
	p->noswap = 1;
	procpriority(p, PriKproc, 0);
	kprocchild(p, worker, (void *)(usize)id);
	ready(p);
}

void
arm64kernelstart(Fdtinfo *info)
{
	u64 freq;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	sys->cyclefreq = freq;
	machp()->online = 1;
	consuartputs = uartwrite;
	mallocinit();
	conf.npage = info->ramsize / PGSZ;
	conf.upages = arm64physfreepages();
	conf.nproc = 64;
	conf.nimage = 64;
	eve = "root";
	procinit0();
	psinit(conf.nproc);
	initimage();
	printinit();
	timersinit();
	arm64virtio9pconfigure(info);
	devtabreset();
	arm64uartputs("HARVEY-ARM64 DEVICE-RESET PASS\n");
	pageinit();
	{
		Page *p;

		p = pgalloc(PGSZ, 0);
		if(p == nil)
			panic("ARM64 page allocator failed");
		pgfree(p);
	}
	arm64uartputs("HARVEY-ARM64 PAGE-ALLOC PASS\n");
	arm64clockready();
	startworker(1);
	startworker(2);
	schedinit();
	panic("ARM64 scheduler returned");
}
