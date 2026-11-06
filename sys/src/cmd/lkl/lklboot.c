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

extern struct lkl_host_operations lkl_harvey_ops;

extern int	lkl_init(struct lkl_host_operations*);
extern int	lkl_start_kernel(const char *fmt, ...);
extern long	lkl_sys_halt(void);
extern void	lkl_cleanup(void);
extern void	lklhost_setout(int);
extern void	lklhost_debug(int);
extern void	lklhost_selftest(void);
extern int	lklhost_mapdev(char*, u64*, u64*);
extern int	lklhost_iomem(u64, void*, int, int);
extern int	lkl_printf(const char*, ...);

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
		t0 = nsec();
		r = lklsleep(0, 87*1000*1000);
		t1 = nsec();
		d = (t1 - t0) / 1000000;
		lkl_printf("selftest: nanosleep %d ret=%ld %lldms\n", i, r, d);
		if(r < 0 || d < 60 || d > 1500)
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

void
main(int argc, char **argv)
{
	int fd, hosttest, kerneltest;

	hosttest = 0;
	kerneltest = 0;
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
	default:
		fprint(2, "usage: lklboot [-d] [-s] [-t] [platform-node]\n");
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
		fd = open(path, OWRITE);
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

	say("lklboot: start\n");
	if(lkl_init(&lkl_harvey_ops) < 0){
		say("lklboot: lkl_init failed\n");
		exits("init");
	}
	say("lklboot: init ok\n");
	if(lkl_start_kernel("mem=128M") < 0){
		say("lklboot: lkl_start_kernel failed\n");
		exits("start");
	}
	say("lklboot: kernel up\n");
	if(kerneltest)
		selftest();

	/*
	 * lkl_sys_halt() runs the reboot syscall and unwinds the kernel: it
	 * stops the idle task, joins the kernel threads and frees the arena.
	 */
	if(lkl_sys_halt() < 0)
		say("lklboot: halt failed\n");
	lkl_cleanup();
	say("lklboot: halted\n");
	exits(nil);
}
