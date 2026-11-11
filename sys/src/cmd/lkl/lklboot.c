/*
 * lklboot -- M1 probe: start LKL inside a Harvey process, print the Linux
 * banner, then shut the kernel down again.
 *
 * No devices are attached yet.  The point is to prove the host layer, the
 * kernel init path through /init, and a clean shutdown.
 *
 * Diagnostics are written to a file by default: writing to the Harvey
 * console from many host threads at once can wedge the console driver.
 */
#include <u.h>
#include <libc.h>
#include <lkl/asm/host_ops.h>
#include <lkl/asm/unistd.h>
#include "lklfamily.h"

extern struct lkl_host_operations lkl_harvey_ops;

extern int	lkl_init(struct lkl_host_operations*);
extern int	lkl_start_kernel(const char *fmt, ...);
extern void	lkl_set_dtb(void*, unsigned long);
extern long	lkl_sys_halt(void);
extern void	lkl_cleanup(void);
extern void	lklhost_setout(int);
extern void	lklhost_debug(int);
extern void	lklhost_selftest(void);
extern int	lklhost_mapdev(char*, u64*, u64*);
extern int	lklhost_iomem(u64, void*, int, int);
extern int	lklhost_irqthread(char*, int);
extern void	lklhost_raminit(unsigned long);
extern int	lkl_printf(const char*, ...);
extern void	usbservice(void);

#define say(s) lkl_printf("%s", s)

/*
 * The self-test is the payload of LKL's own tests/boot (getpid, nanosleep),
 * run against our host layer instead of liblkl's.  A nanosleep only works
 * if the host arms the clockevent and the idle path can stop and restart
 * the tick, which is exactly the part that was broken.
 */
typedef struct Lkltimespec Lkltimespec;
struct Lkltimespec {
	long long	sec;
	long long	nsec;
};

extern long	lkl_syscall(long, long*);

static long
lklsleep(long long sec, long long nsec)
{
	Lkltimespec ts;
	long p[6];

	ts.sec = sec;
	ts.nsec = nsec;
	/*
	 * aarch64 is 64-bit, so the plain nanosleep takes the 64-bit
	 * timespec; the *_time64 numbering exists only for 32-bit hosts.
	 */
	p[0] = (long)&ts;
	p[1] = 0;
	p[2] = 0;
	p[3] = 0;
	p[4] = 0;
	p[5] = 0;
	return lkl_syscall(__lkl__NR_nanosleep, p);
}

static int
selftest(void)
{
	vlong t0, t1, d;
	long r, pid;
	int i, bad;
	long p[6];

	bad = 0;
	memset(p, 0, sizeof p);
	pid = lkl_syscall(__lkl__NR_getpid, p);
	lkl_printf("selftest: getpid %ld\n", pid);
	if(pid <= 0)
		bad++;

	for(i = 0; i < 3; i++){
		long long sn = i == 0 ? 1*1000*1000 : (i == 1 ? 10*1000*1000 : 87*1000*1000);
		t0 = nsec();
		r = lklsleep(0, sn);
		t1 = nsec();
		d = (t1 - t0) / 1000000;
		lkl_printf("selftest: nanosleep %d (%lldns) ret=%ld %lldms\n", i, sn, r, d);
		if(r < 0 || d < 60 || d > 3000)
			bad++;
	}
	/*
	 * Long enough for NO_HZ_IDLE to stop the tick: the guest programs
	 * KTIME_MAX, the delta a naive host cannot represent.  This is the
	 * case that used to turn into a permanent IRQ storm.
	 */
	t0 = nsec();
	r = lklsleep(1, 200*1000*1000);
	t1 = nsec();
	d = (t1 - t0) / 1000000;
	lkl_printf("selftest: idle sleep ret=%ld %lldms\n", r, d);
	if(r < 0 || d < 1100 || d > 4000)
		bad++;

	/* The tick has to come back afterwards. */
	t0 = nsec();
	r = lklsleep(0, 50*1000*1000);
	t1 = nsec();
	d = (t1 - t0) / 1000000;
	lkl_printf("selftest: resume sleep ret=%ld %lldms\n", r, d);
	if(r < 0 || d < 30 || d > 1500)
		bad++;

	lkl_printf("selftest: %s\n", bad ? "FAIL" : "PASS");
	return bad;
}

static void
probe(char *node)
{
	char path[80], buf[64];
	u64 base, size;
	uchar ver[2];
	int fd;

	snprint(path, sizeof path, "#P/%s/claim", node);
	if((fd = open(path, OWRITE)) < 0)
		lkl_printf("probe: claim %s: %r\n", node);
	else{
		write(fd, "lkld", 4);
		close(fd);
	}
	if(lklhost_mapdev(node, &base, &size) < 0){
		lkl_printf("probe: map %s: %r\n", node);
		return;
	}
	lkl_printf("probe: %s reg %#llux size %#llux\n", node, base, size);
	if(lklhost_iomem(base + 0xfe, ver, 2, 0) == 0)
		lkl_printf("probe: SDHCI version %x.%x\n", ver[1], ver[0]);
	else
		lkl_printf("probe: read version failed\n");
}

/*
 * Claim the eMMC node and map its register window into the shared mmio[]
 * table.  The embedded DTB carries the same reg address, so when
 * sdhci-of-dwcmshc ioremaps its resource through lkl_ops->ioremap() it
 * lands on this window.
 */
static int
nodeirq(char *node)
{
	char path[80], buf[32];
	int fd, n;

	snprint(path, sizeof path, "#P/%s/irq", node);
	if((fd = open(path, OREAD)) < 0)
		return -1;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		return -1;
	buf[n] = 0;
	return atoi(buf);
}

/*
 * The eMMC controller's card clock comes from the CRU, outside the
 * controller's register block.  Boot-critical clocks stay native: release
 * the controller resets and put cclk_emmc on the oscillator-derived
 * identify clock (parent 5), which is what the working vendor sequence
 * uses.  The LKL driver divides it further for 400 kHz.
 */
static void
setemmcclock(void)
{
	char *node = "clock-controller@fdd20000";
	u64 base, size;
	u32 v;

	if(lklhost_mapdev(node, &base, &size) < 0){
		lkl_printf("lklboot: map %s: %r\n", node);
		return;
	}
	/*
	 * The controller has five CRU resets (SRST_A/H/B/C/T_EMMC) in
	 * SOFTRST_CON(7) bits 5-9.  U-Boot leaves them asserted when it
	 * boots over the network; the register block still answers, but
	 * the card interface is held in reset and no command completes.
	 */
	v = (0x3e0u<<16) | 0;
	lklhost_iomem(base + 0x41c, &v, 4, 1);
	/*
	 * Identify clock: parent 5 is clk_osc0_div_375k.  Its divider is
	 * CLKSEL_CON(82) bits 8-13 and only the clock framework normally
	 * programs it, so program 24 MHz / 32 = 750 kHz and enable its
	 * gate here; the controller then divides it down further.
	 */
	v = (0x3fu<<8)<<16 | (0x1fu<<8);
	lklhost_iomem(base + 0x248, &v, 4, 1);
	v = (1u<<15)<<16 | 0;
	lklhost_iomem(base + 0x38c, &v, 4, 1);
	v = (7u<<12)<<16 | (5u<<12);	/* select parent 5 */
	lklhost_iomem(base + 0x170, &v, 4, 1);
	v = (1u<<8)<<16 | 0;		/* enable the clock gate */
	lklhost_iomem(base + 0x324, &v, 4, 1);
	{
		u32 sr, sel, gate;

		sr = sel = gate = 0;
		lklhost_iomem(base + 0x41c, &sr, 4, 0);
		lklhost_iomem(base + 0x170, &sel, 4, 0);
		lklhost_iomem(base + 0x324, &gate, 4, 0);
		lkl_printf("lklboot: cru 375k resets %#ux sel %#ux gate %#ux\n",
			   sr, sel, gate);
	}
}

static int
findfamily(char *name)
{
	int i;

	for(i = 0; i < nlklfamilies; i++)
		if(strcmp(lklfamilies[i].name, name) == 0)
			return i;
	return -1;
}

static Dtb*
finddtb(char *name)
{
	int i;

	for(i = 0; i < nlkldtbs; i++)
		if(strcmp(lkldtbs[i].name, name) == 0)
			return &lkldtbs[i];
	return nil;
}

/*
 * Claim and map one #P device window.  Every driver's ioremap() resolves
 * through lklhost_mapdev()'s mmio[] table, so the family manifest lists
 * every window its drivers touch, not only the controller.
 */
static int
mapdevice(char *node)
{
	char path[96];
	u64 base, size;
	int fd;

	snprint(path, sizeof path, "#P/%s/claim", node);
	if((fd = open(path, OWRITE)) < 0){
		lkl_printf("lklboot: claim %s: %r\n", node);
		return -1;
	}
	write(fd, "lkld", 4);
	close(fd);
	if(lklhost_mapdev(node, &base, &size) < 0){
		lkl_printf("lklboot: map %s: %r\n", node);
		return -1;
	}
	lkl_printf("lklboot: %s reg %#llux size %#llux\n", node, base, size);
	return 0;
}

static int
mapfamily(Family *f)
{
	char devs[256], *p, *q;
	int bad;

	snprint(devs, sizeof devs, "%s", f->devices);
	bad = 0;
	for(p = devs; *p != 0; p = q){
		while(*p == ',' || *p == ' ')
			p++;
		if(*p == 0)
			break;
		for(q = p; *q != 0 && *q != ',' && *q != ' '; q++)
			;
		if(*q != 0)
			*q++ = 0;
		if(mapdevice(p) < 0)
			bad = 1;
	}
	return bad ? -1 : 0;
}

static void
watchfamily(Family *f)
{
	char devs[256], *p, *q;
	int irq;

	snprint(devs, sizeof devs, "%s", f->devices);
	for(p = devs; *p != 0; p = q){
		while(*p == ',' || *p == ' ')
			p++;
		if(*p == 0)
			break;
		for(q = p; *q != 0 && *q != ',' && *q != ' '; q++)
			;
		if(*q != 0)
			*q++ = 0;
		irq = nodeirq(p);
		if(irq <= 0)
			continue;	/* no interrupt on this node */
		if(lklhost_irqthread(p, irq) == 0)
			lkl_printf("lklboot: %s irq %d watched\n", p, irq);
		else
			lkl_printf("lklboot: %s irq %d watch failed\n", p, irq);
	}
}

static void
setupfamily(Family *f)
{
	switch(f->setup){
	case FamEmmc:
		setemmcclock();
		break;
	case FamUsb:
		/*
		 * The USB PHYs and PD_PIPE are still native responsibilities;
		 * the vendor dwc3-rockchip glue is not in the 6.12 tree.
		 */
		lkl_printf("lklboot: usb family: native PHY handoff assumed\n");
		break;
	default:
		break;
	}
}

void
main(int argc, char **argv)
{
	int fd, hosttest, kerneltest, console, famidx;
	char *fname;
	Family *f;
	Dtb *dtb;

	hosttest = 0;
	kerneltest = 0;
	console = 0;
	fname = "emmc";
	ARGBEGIN{
	case 'd':
		lklhost_debug(1);
		break;
	case 's':
		hosttest = 1;
		break;
	case 't':
		kerneltest = 1;
		break;
	case 'c':
		console = 1;
		break;
	case 'f':
		fname = ARGF();
		break;
	default:
		fprint(2, "usage: lklboot [-c] [-d] [-f family] [-s] [-t] "
			  "[platform-node]\n");
		exits("usage");
	}ARGEND
	/*
	 * Diagnostics go to /dev/lkllog, a synthetic file backed by a ring
	 * buffer.  Writing to /dev/cons from these threads is not safe: the
	 * console path busy-waits on the UART FIFO with interrupts masked,
	 * so a slow reader parks the machine.  Read the log afterwards with
	 * "cat /dev/lkllog".
	 */
	{
		/*
		 * A path literal in .rodata reaches open() as an empty name on
		 * this port; a stack copy works, and the same trick is used for
		 * the #P paths.
		 */
		char path[32];

		snprint(path, sizeof path, "%s", "/dev/lkllog");
		fd = console ? 2 : open(path, OWRITE);
	}
	if(fd < 0){
		char eb[160];
		int m = snprint(eb, sizeof eb, "lklboot: /dev/lkllog: %r; using console\n");
		write(2, eb, m);
		fd = 1;
	}
	lklhost_setout(fd);
	if(hosttest){
		say("hostselftest: start\n");
		lklhost_selftest();
		say("hostselftest: done\n");
		exits(nil);
	}
	if(argc > 0)
		probe(argv[0]);

	famidx = findfamily(fname);
	if(famidx < 0){
		fprint(2, "lklboot: no family %s\n", fname);
		exits("family");
	}
	f = &lklfamilies[famidx];
	dtb = finddtb(f->dtbname);
	if(dtb == nil){
		fprint(2, "lklboot: no dtb %s\n", f->dtbname);
		exits("dtb");
	}

	lkl_printf("lklboot: family %s (%s), %s\n", f->name, f->program,
		   f->mem);
	lklhost_raminit(f->memsz + 16*1024*1024);
	if(lkl_init(&lkl_harvey_ops) < 0){
		say("lklboot: lkl_init failed\n");
		exits("init");
	}
	say("lklboot: init ok\n");
	setupfamily(f);
	if(mapfamily(f) < 0){
		say("lklboot: device map failed\n");
		exits("map");
	}
	lkl_set_dtb(dtb->start, dtb->end - dtb->start);
	if(lkl_start_kernel("mem=%s", f->mem) < 0){
		say("lklboot: lkl_start_kernel failed\n");
		exits("start");
	}
	say("lklboot: kernel up\n");
	watchfamily(f);

	/*
	 * The USB family's job starts after the kernel is up: wait for the
	 * Linux input device and translate it to /dev/kbd.  Other families
	 * keep their existing post-boot path.
	 */
	if(f->setup == FamUsb && !kerneltest)
		usbservice();

	if(kerneltest){
		selftest();
		/*
		 * lkl_sys_halt() runs the reboot syscall and unwinds the
		 * kernel: it stops the idle task, joins the kernel threads
		 * and frees the arena.
		 */
		if(lkl_sys_halt() < 0)
			say("lklboot: halt failed\n");
		lkl_cleanup();
		say("lklboot: halted\n");
		exits(nil);
	}

	/*
	 * A driver server runs until it is stopped.  Halting here would
	 * race device probe workqueues (the mmc block device attaches
	 * asynchronously) against teardown.
	 */
	for(;;)
		sleep(1000);
}
