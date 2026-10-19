/* Bring Harvey's shared process scheduler online after the ARM64 probes. */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "fdt.h"
#include "phys.h"
#include "mmu.h"
#include "user.h"
#include "userprobe.h"
#include "userland.h"
#include <tos.h>

extern void arm64uartputs(char *);
extern void (*consuartputs)(char *, int);
extern void arm64clockready(void);
extern void arm64virtio9pconfigure(Fdtinfo *);
extern void arm64pnpinit(Fdtinfo *);
extern Dev rootdevtab;
extern Dev virtio9pdevtab;
extern Dev ringdevtab;
extern Dev pnpdevtab;

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
static volatile int userprocessphase;
static u64 useruartbase;
static u32 useruarttype;
static Fdtinfo *bootinfo;

void
arm64processresume(void)
{
	userprocessphase = 2;
	arm64uartputs("HARVEY-ARM64 USER-PROCESS PASS\n");
	for(;;)
		sched();
}

static void
userprocess(void *arg)
{
	Proc *p;
	u64 code, data, entry, stack, root, typeoff;

	USED(arg);
	p = externup();
	code = arm64physpage();
	data = arm64physpage();
	stack = arm64physpage();
	root = arm64mmuuserprocess(arm64mmukernelroot(), code, data, stack);
	if(code == 0 || data == 0 || stack == 0 || root == 0)
		panic("ARM64 user process address space failed");
	if(arm64mmuuserdevice(root, ARM64USERBASE+3*PGSZ,
	   useruartbase & ~(u64)(PGSZ-1)) < 0)
		panic("ARM64 user process UART map failed");
	if(arm64userloadelf(code, data, arm64userprobeelf,
	   sizeof(arm64userprobeelf), &entry) < 0)
		panic("ARM64 user process ELF load failed");
	typeoff = ARM64USERPROBEUARTTYPE - (ARM64USERBASE+PGSZ);
	if(typeoff > PGSZ-sizeof(u32))
		panic("ARM64 user process data offset failed");
	*(u32 *)(data+typeoff) = useruarttype;
	p->MMU.rootpa = root;
	arm64mmuswitch(root);
	userprocessphase = 1;
	arm64runprocessargs(entry, ARM64USERBASE+3*PGSZ,
	   1, ARM64USERBASE+2*PGSZ, ARM64USERBASE+3*PGSZ-512);
}

static void
startuserprocess(void)
{
	Proc *p;

	p = newproc();
	kstrdup(&p->user, eve);
	kstrdup(&p->text, "*arm64-user-probe*");
	p->noswap = 1;
	procpriority(p, PriNormal, 0);
	kprocchild(p, userprocess, nil);
	ready(p);
}

static void
initprocess(void *arg)
{
	Proc *p;

	USED(arg);
	p = externup();
	p->nerrlab = 0;
	spllo();
	p->slash = namec("#/", Atodir, 0, 0);
	pathclose(p->slash->path);
	p->slash->path = newpath("/");
	p->dot = cclone(p->slash);
	devtabinit();
	kproc("alarm", alarmkproc, nil);
	arm64uartputs("HARVEY-ARM64 FILE-EXEC START\n");
	arm64runprocessargs(ARM64EXECSTUBENTRY, USTKTOP-512,
		1, USTKTOP-BIGPGSZ+288, USTKTOP-sizeof(Tos));
}

static void
startinitprocess(void)
{
	Proc *p;
	Segment *s;
	Page *code, *data, *stack;
	KMap *codeva, *datava, *stackva;
	u64 entry;
	char *path;
	char **argv;
	Tos *tos;

	p = newproc();
	p->pgrp = newpgrp();
	p->egrp = smalloc(sizeof(Egrp));
	p->egrp->r.ref = 1;
	p->fgrp = dupfgrp(nil);
	p->rgrp = newrgrp();
	p->procmode = 0640;
	kstrdup(&p->user, eve);
	kstrdup(&p->text, "*arm64-init*");
	p->noswap = 1;

	code = newpage(1, nil, ARM64EXECSTUBBASE, BIGPGSZ, -1);
	data = newpage(1, nil, ARM64EXECSTUBBASE+BIGPGSZ, BIGPGSZ, -1);
	stack = newpage(1, nil, USTKTOP-BIGPGSZ, BIGPGSZ, -1);
	if(code == nil || data == nil || stack == nil)
		panic("ARM64 init process page allocation failed");
	codeva = kmap(code);
	datava = kmap(data);
	if(arm64userloadelfpages(VA(codeva), VA(datava), BIGPGSZ,
	   ARM64EXECSTUBBASE, arm64execstubelf, sizeof(arm64execstubelf),
	   &entry) < 0 || entry != ARM64EXECSTUBENTRY)
		panic("ARM64 exec bootstrap ELF load failed");
	kunmap(codeva);
	kunmap(datava);

	s = newseg(SG_TEXT|SG_READ|SG_EXEC, ARM64EXECSTUBBASE, 1);
	s->flushme++;
	p->seg[0] = s;
	segpage(s, code);
	s = newseg(SG_DATA|SG_READ|SG_WRITE, ARM64EXECSTUBBASE+BIGPGSZ, 1);
	p->seg[1] = s;
	segpage(s, data);
	s = newseg(SG_STACK|SG_READ|SG_WRITE, USTKTOP-USTKSIZE,
		USTKSIZE/BIGPGSZ);
	p->seg[2] = s;
	segpage(s, stack);

	stackva = kmap(stack);
	path = (char *)(VA(stackva)+256);
	strcpy(path, "/boot/execstub");
	argv = (char **)(VA(stackva)+288);
	argv[0] = (char *)(USTKTOP-BIGPGSZ+256);
	argv[1] = nil;
	tos = (Tos *)(VA(stackva)+BIGPGSZ-sizeof(Tos));
	memset(tos, 0, sizeof(*tos));
	tos->cyclefreq = sys->cyclefreq;
	kunmap(stackva);
	procpriority(p, PriNormal, 0);
	kprocchild(p, initprocess, nil);
	ready(p);
}

static int
physseginit(Fdtinfo *info)
{
	Physseg seg;
	u64 base, end;
	u32 i;
	int n;

	n = 0;
	memset(&seg, 0, sizeof seg);
	seg.attr = SG_PHYSICAL | SG_READ | SG_WRITE;
	seg.pgszi = -1;	/* default 2 MiB pages */
	for(i = 0; i < info->ndev; i++){
		base = info->dev[i].base & ~(u64)(BIGPGSZ - 1);
		end = (info->dev[i].base + info->dev[i].size + BIGPGSZ - 1) &
		      ~(u64)(BIGPGSZ - 1);
		if(end <= base)
			end = base + BIGPGSZ;
		seg.name = info->dev[i].name;
		seg.pa = base;
		seg.size = (end - base) / BIGPGSZ;
		if(addphysseg(&seg) == 0)
			n++;
	}
	return n;
}

static void
pnpprobe(Fdtinfo *info)
{
	char *names[2];
	char buf[64];
	Chan *root, *reg;
	Walkqid *walk;
	int n;

	if(info->ndev == 0)
		return;
	root = pnpdevtab.attach("");
	names[0] = info->dev[0].name;
	names[1] = "reg";
	walk = pnpdevtab.walk(root, nil, names, 2);
	if(walk == nil || walk->nqid != 2 || walk->clone == nil){
		if(walk != nil)
			free(walk);
		cclose(root);
		panic("ARM64 #P walk failed");
	}
	reg = walk->clone;
	free(walk);
	cclose(root);
	pnpdevtab.open(reg, OREAD);
	n = pnpdevtab.read(reg, buf, sizeof(buf), 0);
	cclose(reg);
	if(n < 2 || memcmp(buf, "0x", 2) != 0)
		panic("ARM64 #P reg read failed");
	arm64uartputs("HARVEY-ARM64 PNP PASS\n");
}

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
		pnpprobe(bootinfo);
		ringprobe();
		virtio9pprobe();
		arm64uartputs("HARVEY-ARM64 BOOT-PROBES PASS\n");
		startuserprocess();
		while(userprocessphase != 2)
			sched();
		startinitprocess();
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
	useruartbase = info->uartbase;
	useruarttype = info->uarttype;
	bootinfo = info;
	devtabreset();
	physseginit(info);
	arm64pnpinit(info);
	addbootfile("init", (u8 *)arm64initelf, sizeof(arm64initelf));
	addbootfile("o9drvman", (u8 *)arm64drvmanelf, sizeof(arm64drvmanelf));
	addbootfile("o9driverfs", (u8 *)arm64driverfself, sizeof(arm64driverfself));
	addbootfile("uartfs", (u8 *)arm64uartfself, sizeof(arm64uartfself));
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
