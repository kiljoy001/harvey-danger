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

char	*lklsrvname = "lkl";

extern struct lkl_host_operations lkl_harvey_ops;

extern int	lkl_init(struct lkl_host_operations*);
extern int	lkl_start_kernel(const char *fmt, ...);
extern void	lkl_set_dtb(void*, unsigned long);
extern long	lkl_sys_halt(void);
extern void	lkl_cleanup(void);
extern void	lklhost_setout(int);
extern int	wifiscan(void);
extern void	lklhost_debug(int);
extern void	lklhost_selftest(void);
extern int	lklhost_mapdev(char*, u64*, u64*);
extern int	lklhost_iomem(u64, void*, int, int);
extern int	lklhost_irqthread(char*, int);
extern int	lklhost_irqthread_wait(char*, int, char*);
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
		if(r < 0 || d < sn / 1000000 || d > 3000)
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

	/* the claim lasts while its file is open: keep it */
	snprint(path, sizeof path, "#P/%s/claim", node);
	if((fd = open(path, OWRITE)) < 0)
		lkl_printf("probe: claim %s: %r\n", node);
	else if(write(fd, "lkld", 4) != 4)
		lkl_printf("probe: claim %s: %r\n", node);
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
nodeirqfile(char *node, char *file)
{
	char path[80], buf[32];
	int fd, n;

	snprint(path, sizeof path, "#P/%s/%s", node, file);
	if((fd = open(path, OREAD)) < 0)
		return -1;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		return -1;
	buf[n] = 0;
	return atoi(buf);
}

static int
nodeirq(char *node)
{
	return nodeirqfile(node, "irq");
}

/*
 * The eMMC controller's card clock comes from the CRU, outside the
 * controller's register block.  Boot-critical clocks stay native: release
 * the controller resets and put cclk_emmc on the oscillator-derived
 * identify clock (parent 5), which is what the working vendor sequence
 * uses.  The LKL driver divides it further for 400 kHz.
 */
static int claimdevice(char*, int);

static void
setemmcclock(void)
{
	char *node = "clock-controller@fdd20000";
	u64 base, size;
	u32 v;

	/* the CRU serves every block on the SoC: claimed shared */
	if(claimdevice(node, 1) < 0 || lklhost_mapdev(node, &base, &size) < 0){
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
 * A family's device: "<node>", or "<node>+" for one another family
 * drives too (lkl-families.json), claimed shared.  Strips the mark.
 */
static int
nodeshared(char *node)
{
	int n;

	n = strlen(node);
	if(n > 0 && node[n-1] == '+'){
		node[n-1] = 0;
		return 1;
	}
	return 0;
}

/*
 * Claim a #P device: #P maps a window only for its claim's holder.  The
 * claim lasts while its file is open, so it is kept for the guest's life.
 * Shared for a block other drivers touch too: one the family marks, or a
 * SoC-wide block (the clock and reset unit, a GRF, a GPIO bank).
 */
static int
claimdevice(char *node, int shared)
{
	char path[96], class[32], *who;
	int fd, n;

	snprint(path, sizeof path, "#P/%s/class", node);
	if((fd = open(path, OREAD)) >= 0){
		n = read(fd, class, sizeof class - 1);
		close(fd);
		class[n > 0 ? n : 0] = 0;
		if(strncmp(class, "cru", 3) == 0 || strncmp(class, "grf", 3) == 0 ||
		   strncmp(class, "gpio", 4) == 0)
			shared = 1;
	}
	snprint(path, sizeof path, "#P/%s/claim", node);
	if((fd = open(path, OWRITE)) < 0){
		lkl_printf("lklboot: claim %s: %r\n", node);
		return -1;
	}
	who = shared ? "lkld shared" : "lkld";
	if(write(fd, who, strlen(who)) != strlen(who)){
		lkl_printf("lklboot: claim %s: %r\n", node);
		close(fd);
		return -1;
	}
	return 0;
}

/*
 * Claim and map one #P device window.  Every driver's ioremap() resolves
 * through lklhost_mapdev()'s mmio[] table, so the family manifest lists
 * every window its drivers touch, not only the controller.
 */
static int
mapdevice(char *node, int shared)
{
	u64 base, size;

	if(claimdevice(node, shared) < 0)
		return -1;
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
	char devs[512], *p, *q;
	int bad;

	snprint(devs, sizeof devs, "%s", f->devices);
	bad = 0;
	for(p = devs; *p != 0; p = q){
		while(*p == ' ')
			p++;
		if(*p == 0)
			break;
		for(q = p; *q != 0 && *q != ' '; q++)
			;
		if(*q != 0)
			*q++ = 0;
		if(mapdevice(p, nodeshared(p)) < 0){
			if(strcmp(p, "dwc3@4e00000") == 0 ||
			   strcmp(f->name, "bengal_usb") != 0)
				bad = 1;
		}
	}
	return bad ? -1 : 0;
}

static void
watchfamily(Family *f)
{
	char devs[512], *p, *q;
	int irq;

	snprint(devs, sizeof devs, "%s", f->devices);
	for(p = devs; *p != 0; p = q){
		while(*p == ' ')
			p++;
		if(*p == 0)
			break;
		for(q = p; *q != 0 && *q != ' '; q++)
			;
		if(*q != 0)
			*q++ = 0;
		nodeshared(p);
		irq = nodeirq(p);
		if(irq > 0){
			if(lklhost_irqthread(p, irq) == 0)
				lkl_printf("lklboot: %s irq %d watched\n", p, irq);
			else
				lkl_printf("lklboot: %s irq %d watch failed\n", p, irq);
		}
		irq = nodeirqfile(p, "irq2");
		if(irq > 0){
			if(lklhost_irqthread_wait(p, irq, "wait2") == 0)
				lkl_printf("lklboot: %s irq2 %d watched\n", p, irq);
			else
				lkl_printf("lklboot: %s irq2 %d watch failed\n", p, irq);
		}
	}
}

/*
 * UWE5622 native handoff.  The guest owns the DW MSHC controller, but the
 * CRU gates/mux, the SDIO pinmux and the module's power/reset lines stay
 * native.  sclk_sdmmc1 is set to cpll_100m (the DT declares ciu fixed at
 * 100 MHz); the controller divides it down to the card clock.
 */
static u64
wcnmap(char *node)
{
	u64 base, size;

	/* CRU, pinmux and GRF blocks other drivers use too: shared */
	if(claimdevice(node, 1) < 0 || lklhost_mapdev(node, &base, &size) < 0){
		lkl_printf("lklboot: map %s: %r\n", node);
		return ~0ULL;
	}
	return base;
}

static u32
wcnrd(u64 base, u32 off)
{
	u32 v = 0;

	lklhost_iomem(base + off, &v, 4, 0);
	return v;
}

static void
wcnwr(u64 base, u32 off, u32 v)
{
	lklhost_iomem(base + off, &v, 4, 1);
}

static void
setwcnpower(void)
{
	u64 cru, pmugrf, sysgrf, gpio0, gpio2;
	u32 v;

	cru = wcnmap("clock-controller@fdd20000");
	pmugrf = wcnmap("syscon@fdc20000");
	sysgrf = wcnmap("syscon@fdc60000");
	gpio0 = wcnmap("gpio@fdd60000");
	gpio2 = wcnmap("gpio@fe750000");
	if(cru == ~0ULL || pmugrf == ~0ULL || sysgrf == ~0ULL ||
	   gpio0 == ~0ULL || gpio2 == ~0ULL)
		return;

	/*
	 * hclk_sdmmc1/clk_sdmmc1 gates (0 enables), sclk_sdmmc1 mux =
	 * cpll_100m (field 12-14, hiword mask, so 0x7<<28 | 3<<12).
	 */
	wcnwr(cru, 0x33c, 0x000c0000);
	wcnwr(cru, 0x178, 0x70003000);

	/* sdmmc1 pins: A3..A7 and B0, function 1. */
	wcnwr(sysgrf, 0x20, 0xf0001000);
	wcnwr(sysgrf, 0x24, 0xffff1111);
	wcnwr(sysgrf, 0x28, 0x000f0001);

	/* WL_REG_ON (GPIO0_D3) and BT_REG_ON (GPIO2_B7) become GPIOs. */
	wcnwr(pmugrf, 0x18, 0xf0000000);
	wcnwr(sysgrf, 0x2c, 0xf0000000);

	/*
	 * GPIO v2 writes carry a write-enable mask in the upper 16 bits:
	 * a bit is updated when both its mask bit and value bit are set.
	 * GPIO0_D3 is pin 27 (DR_H 0x04, DDR_H 0x0c, bit 11) and
	 * GPIO2_B7 is pin 15 (DR_L 0x00, DDR_L 0x08, bit 15).
	 */
	wcnwr(gpio0, 0x0c, (1u<<(16+11)) | (1u<<11));
	wcnwr(gpio2, 0x08, (1u<<(16+15)) | (1u<<15));

	/* Drive WL_REG_ON low, then high; BT_REG_ON high. */
	wcnwr(gpio0, 0x04, 1u<<(16+11));
	sleep(10);
	wcnwr(gpio0, 0x04, (1u<<(16+11)) | (1u<<11));
	wcnwr(gpio2, 0x00, (1u<<(16+15)) | (1u<<15));
	sleep(200);

	lkl_printf("lklboot: wcn cru33c %#ux cru178 %#ux grf20 %#ux "
		   "grf24 %#ux grf28 %#ux grf2c %#ux pmugrf18 %#ux "
		   "gpio0 ddr %#ux dr %#ux gpio2 ddr %#ux dr %#ux\n",
		   wcnrd(cru, 0x33c), wcnrd(cru, 0x178),
		   wcnrd(sysgrf, 0x20), wcnrd(sysgrf, 0x24),
		   wcnrd(sysgrf, 0x28), wcnrd(sysgrf, 0x2c),
		   wcnrd(pmugrf, 0x18), wcnrd(gpio0, 0x0c),
		   wcnrd(gpio0, 0x04), wcnrd(gpio2, 0x08),
		   wcnrd(gpio2, 0x00));
}

static void
wifiservice(void)
{
	/* The guest driver owns the bus; keep the process (and kernel) up. */
	for(;;)
		sleep(1000);
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
	case FamWifi:
		setwcnpower();
		break;
	default:
		break;
	}
}

extern void	lklhost_run_shared(void (*)(void*), void*);

typedef struct Mainargs Mainargs;
struct Mainargs {
	char	*fname;
	int	nohw;
	int	kerneltest;
	int	scan;
};
static Mainargs mainargs;

static void
runmain(void *arg)
{
	Mainargs *a = arg;
	Family *f;
	Dtb *dtb;
	int famidx;

	famidx = findfamily(a->fname);
	if(famidx < 0){
		fprint(2, "lklboot: no family %s\n", a->fname);
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
	/* stderr is the screen: name this instance so a later
	 * "user fault pid N lklboot" line can be attributed. */
	fprint(2, "lklboot: family %s pid %d\n", f->name, getpid());
	lklhost_raminit(f->memsz + 16*1024*1024);
	if(lkl_init(&lkl_harvey_ops) < 0){
		say("lklboot: lkl_init failed\n");
		exits("init");
	}
	say("lklboot: init ok\n");
	setupfamily(f);
	if(!a->nohw && mapfamily(f) < 0){
		say("lklboot: device map failed\n");
		exits("map");
	}
	if(a->nohw)
		say("lklboot: hardware mapping skipped\n");
	lkl_set_dtb(dtb->start, dtb->end - dtb->start);
	if(strcmp(f->name, "bengal_ctl") == 0)
		lklsrvname = "lklctl";
	if(lkl_start_kernel("mem=%s%s", f->mem,
	   f->setup == FamUsb ? " clk_ignore_unused" : "") < 0){
		say("lklboot: lkl_start_kernel failed\n");
		exits("start");
	}
	say("lklboot: kernel up\n");
	/*
	 * Start forwarding device interrupts now.  lkl_start_kernel() returns
	 * once the guest reaches /init, i.e. after every initcall has run, and
	 * the drivers that need interrupts probe from workqueues (MMC card
	 * detection) or after this point (the WiFi SDIO power-on).
	 */
	if(!a->nohw)
		watchfamily(f);

	/*
	 * The USB family's job starts after the kernel is up: wait for the
	 * Linux input device and translate it to /dev/kbd.  Other families
	 * keep their existing post-boot path.
	 */
	if(f->setup == FamUsb && !a->kerneltest)
		usbservice();
	if(f->setup == FamWifi && !a->kerneltest){
		if(a->scan)
			wifiscan();
		wifiservice();
	}

	if(a->kerneltest){
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

void
main(int argc, char **argv)
{
	int fd, hosttest, kerneltest, console, nohw, background, scan;
	char *fname;

	hosttest = 0;
	kerneltest = 0;
	console = 0;
	nohw = 0;
	background = 0;
	scan = 0;
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
	case 'n':
		nohw = 1;
		break;
	case 'b':
		background = 1;
		break;
	case 'w':
		scan = 1;
		break;
	case 'f':
		fname = ARGF();
		break;
	default:
		fprint(2, "usage: lklboot [-b] [-c] [-d] [-f family] [-n] "
			  "[-s] [-t] [-w] [platform-node]\n");
		exits("usage");
	}ARGEND
	/*
	 * A driver family is a service, not a command: detach before any LKL
	 * state exists so the shell that started it gets its prompt back and
	 * the CPU-lock owner is the detached process.  RFNOTEG keeps console
	 * interrupts away from a machine that must keep serving.
	 */
	if(background && rfork(RFPROC|RFNOTEG) != 0)
		exits(nil);
	/*
	 * The guest kernel owns one virtual CPU cooperatively; its
	 * per-CPU state is only valid under that ownership.  Wire this
	 * process (and, by inheritance, every thread it spawns) to a
	 * single physical CPU so guest threads can never execute in
	 * parallel on separate cores while that state is in flux.
	 */
	{
		char ctl[64];
		int cfd;

		snprint(ctl, sizeof ctl, "/proc/%d/ctl", getpid());
		if((cfd = open(ctl, OWRITE)) >= 0){
			write(cfd, "wired 1", 7);
			close(cfd);
		}
	}
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

	mainargs.fname = fname;
	mainargs.nohw = nohw;
	mainargs.kerneltest = kerneltest;
	mainargs.scan = scan;
	lklhost_run_shared(runmain, &mainargs);
}
