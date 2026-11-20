/*
 * lklusb -- the USB family service.
 *
 * LKL owns the DWC3 controller, the USB core, hubs and the class drivers.
 * This file is the Harvey side: it waits for the Linux input device to show
 * up and translates input_event records into Plan 9 /dev/kbd characters.
 *
 * Block devices (/dev/sd*) and a proper /srv/input 9P service come after
 * the keyboard path is proven; the first milestone is a USB keyboard
 * typing into Rio through the LKL-owned controller.
 */
#include <u.h>
#include <libc.h>
#include <lkl/asm/unistd.h>

extern long	lkl_syscall(long, long*);
extern int	lkl_printf(const char*, ...);
extern char	*lklsrvname;

typedef struct Uevent Uevent;
struct Uevent {
	long	sec;
	long	usec;
	ushort	type;
	ushort	code;
	int	value;
};

enum {
	EvSyn	= 0,
	EvKey	= 1,
	EvRel	= 2,

	KeyEsc		= 1,
	Key1		= 2,
	Key0		= 11,
	KeyMinus	= 12,
	KeyEqual	= 13,
	KeyBackspace	= 14,
	KeyTab		= 15,
	KeyQ		= 16,
	KeyW		= 17,
	KeyE		= 18,
	KeyR		= 19,
	KeyT		= 20,
	KeyY		= 21,
	KeyU		= 22,
	KeyI		= 23,
	KeyO		= 24,
	KeyP		= 25,
	KeyLeftBrace	= 26,
	KeyRightBrace	= 27,
	KeyEnter	= 28,
	KeyLeftCtrl	= 29,
	KeyA		= 30,
	KeyS		= 31,
	KeyD		= 32,
	KeyF		= 33,
	KeyG		= 34,
	KeyH		= 35,
	KeyJ		= 36,
	KeyK		= 37,
	KeyL		= 38,
	KeySemicolon	= 39,
	KeyApostrophe	= 40,
	KeyGrave	= 41,
	KeyLeftShift	= 42,
	KeyBackslash	= 43,
	KeyZ		= 44,
	KeyX		= 45,
	KeyC		= 46,
	KeyV		= 47,
	KeyB		= 48,
	KeyN		= 49,
	KeyM		= 50,
	KeyComma	= 51,
	KeyDot		= 52,
	KeySlash	= 53,
	KeyRightShift	= 54,
	KeyLeftAlt	= 56,
	KeySpace	= 57,
	KeyRightCtrl	= 97,
	KeyRightAlt	= 100,

	AtFdcwd		= -100,
	O_Rdonly	= 0,
	O_Wronly	= 1,
	O_Rdwr		= 2,
	O_Nonblock	= 04000,
};

static int
lklsleep(long ms)
{
	long p[6];
	struct { long long sec, nsec; } ts;

	ts.sec = ms / 1000;
	ts.nsec = (ms % 1000) * 1000000LL;
	memset(p, 0, sizeof p);
	p[0] = (long)&ts;
	return lkl_syscall(__lkl__NR_nanosleep, p);
}

static long
lklopenflags(char *path, int flags)
{
	long p[6];

	memset(p, 0, sizeof p);
	p[0] = AtFdcwd;
	p[1] = (long)path;
	p[2] = flags;
	return lkl_syscall(__lkl__NR_openat, p);
}

static long
lklopen(char *path)
{
	return lklopenflags(path, O_Rdonly);
}

static long
lklwrite(long fd, void *buf, long len)
{
	long p[6];

	memset(p, 0, sizeof p);
	p[0] = fd;
	p[1] = (long)buf;
	p[2] = len;
	return lkl_syscall(__lkl__NR_write, p);
}

static void
ttyraw(long fd)
{
	struct {
		u32 c_iflag, c_oflag, c_cflag, c_lflag;
		uchar c_line;
		uchar c_cc[19];
	} t;
	long p[6];

	memset(&t, 0, sizeof t);
	memset(p, 0, sizeof p);
	p[0] = fd;
	p[1] = 0x5401; /* TCGETS */
	p[2] = (long)&t;
	if(lkl_syscall(__lkl__NR_ioctl, p) < 0)
		return;
	t.c_iflag = 0;
	t.c_oflag = 0;
	t.c_lflag = 0;
	t.c_cc[6] = 1; /* VMIN */
	t.c_cc[5] = 0; /* VTIME */
	memset(p, 0, sizeof p);
	p[0] = fd;
	p[1] = 0x5402; /* TCSETS */
	p[2] = (long)&t;
	lkl_syscall(__lkl__NR_ioctl, p);
}

static long
lklmkdir(char *path, int mode)
{
	long p[6];

	memset(p, 0, sizeof p);
	p[0] = AtFdcwd;
	p[1] = (long)path;
	p[2] = mode;
	return lkl_syscall(__lkl__NR_mkdirat, p);
}

static long
lklsymlink(char *target, char *linkpath)
{
	long p[6];

	memset(p, 0, sizeof p);
	p[0] = (long)target;
	p[1] = AtFdcwd;
	p[2] = (long)linkpath;
	return lkl_syscall(__lkl__NR_symlinkat, p);
}

static long
lklmount(char *src, char *tgt, char *fstype)
{
	long p[6];

	memset(p, 0, sizeof p);
	p[0] = (long)src;
	p[1] = (long)tgt;
	p[2] = (long)fstype;
	return lkl_syscall(__lkl__NR_mount, p);
}

static long
lklmknod(char *path, int mode, ulong dev)
{
	long p[6];

	memset(p, 0, sizeof p);
	p[0] = AtFdcwd;
	p[1] = (long)path;
	p[2] = mode;
	p[3] = dev;
	return lkl_syscall(__lkl__NR_mknodat, p);
}

static long
lklread(long fd, void *buf, long len)
{
	long p[6];

	memset(p, 0, sizeof p);
	p[0] = fd;
	p[1] = (long)buf;
	p[2] = len;
	return lkl_syscall(__lkl__NR_read, p);
}

static long
lklclose(long fd)
{
	long p[6];

	memset(p, 0, sizeof p);
	p[0] = fd;
	return lkl_syscall(__lkl__NR_close, p);
}

static int
lklwritestr(char *path, char *val)
{
	long fd, n, len;

	fd = lklopenflags(path, O_Wronly);
	if(fd < 0){
		lkl_printf("lklusb: open %s: %ld\n", path, fd);
		return -1;
	}
	len = strlen(val);
	n = lklwrite(fd, val, len);
	lklclose(fd);
	if(n != len){
		lkl_printf("lklusb: write %s: %ld\n", path, n);
		return -1;
	}
	return 0;
}

/*
 * One table for the US layout: keycode -> unshifted, shifted.  Zero means
 * the key is not translated yet (function keys, arrows, media keys).
 */
static struct {
	int	code;
	char	plain;
	char	shift;
} keymap[] = {
	{ Key1, '1', '!' }, { Key0, '0', ')' },
	{ KeyMinus, '-', '_' }, { KeyEqual, '=', '+' },
	{ KeyQ, 'q', 'Q' }, { KeyW, 'w', 'W' }, { KeyE, 'e', 'E' },
	{ KeyR, 'r', 'R' }, { KeyT, 't', 'T' }, { KeyY, 'y', 'Y' },
	{ KeyU, 'u', 'U' }, { KeyI, 'i', 'I' }, { KeyO, 'o', 'O' },
	{ KeyP, 'p', 'P' }, { KeyLeftBrace, '[', '{' },
	{ KeyRightBrace, ']', '}' },
	{ KeyA, 'a', 'A' }, { KeyS, 's', 'S' }, { KeyD, 'd', 'D' },
	{ KeyF, 'f', 'F' }, { KeyG, 'g', 'G' }, { KeyH, 'h', 'H' },
	{ KeyJ, 'j', 'J' }, { KeyK, 'k', 'K' }, { KeyL, 'l', 'L' },
	{ KeySemicolon, ';', ':' }, { KeyApostrophe, '\'', '"' },
	{ KeyGrave, '`', '~' }, { KeyBackslash, '\\', '|' },
	{ KeyZ, 'z', 'Z' }, { KeyX, 'x', 'X' }, { KeyC, 'c', 'C' },
	{ KeyV, 'v', 'V' }, { KeyB, 'b', 'B' }, { KeyN, 'n', 'N' },
	{ KeyM, 'm', 'M' }, { KeyComma, ',', '<' }, { KeyDot, '.', '>' },
	{ KeySlash, '/', '?' }, { KeySpace, ' ', ' ' },
};

static int
translate(int code, int shift)
{
	int i;
	char c;

	if(code >= Key1 && code <= Key0){
		/* 2..11 map to 1..0; shift gives the US symbols. */
		static char plain[] = "1234567890";
		static char shifted[] = "!@#$%^&*()";
		c = shift ? shifted[code - Key1] : plain[code - Key1];
		return c;
	}
	for(i = 0; i < nelem(keymap); i++)
		if(keymap[i].code == code)
			return shift ? keymap[i].shift : keymap[i].plain;
	if(code == KeyEnter)
		return '\n';
	if(code == KeyTab)
		return '\t';
	if(code == KeyBackspace)
		return 0x7f;
	if(code == KeyEsc)
		return 0x1b;
	return 0;
}

static long
lklgetdents64(long fd, void *buf, long len)
{
	long p[6];

	memset(p, 0, sizeof p);
	p[0] = fd;
	p[1] = (long)buf;
	p[2] = len;
	return lkl_syscall(__lkl__NR_getdents64, p);
}

static int
findudc(char *udc, int udcsize)
{
	long fd, n, off;
	char *buf, *name;
	ushort reclen;

	fd = lklopen("/sys/class/udc");
	if(fd < 0)
		return -1;
	buf = malloc(1024);
	if(buf == nil){
		lklclose(fd);
		return -1;
	}
	n = lklgetdents64(fd, buf, 1024);
	lklclose(fd);
	for(off = 0; off + 20 <= n; off += reclen){
		reclen = *(ushort*)(buf + off + 16);
		if(reclen < 20 || off + reclen > n)
			break;
		name = buf + off + 19;
		if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
			continue;
		if(strncmp(name, "dummy", 5) == 0)
			continue;
		strecpy(udc, udc + udcsize, name);
		free(buf);
		return 0;
	}
	free(buf);
	return -1;
}

static int
setupgadget(char *udc, int udcsize)
{
	int i;
	long r;

	lklmkdir("/sys", 0755);
	r = lklmount("sysfs", "/sys", "sysfs");
	if(r < 0 && r != -16)	/* -EBUSY */
		lkl_printf("lklusb: mount sysfs: %ld\n", r);
	lklmkdir("/sys/kernel", 0755);
	lklmkdir("/sys/kernel/config", 0755);
	r = lklmount("configfs", "/sys/kernel/config", "configfs");
	if(r < 0 && r != -16)
		lkl_printf("lklusb: mount configfs: %ld\n", r);

	for(i = 0; i < 30; i++){
		if(findudc(udc, udcsize) == 0)
			break;
		lklsleep(100);
	}
	if(i >= 30)
		return -1;

	lkl_printf("lklusb: found UDC %s\n", udc);
	if(strcmp(udc, "4e00000.dwc3") == 0){
		extern u32 lklhost_mmio_r32(u64);
		extern void lklhost_mmio_w32(u64, u32);
		u32 hs = lklhost_mmio_r32(0x4ef8810ULL);
		lkl_printf("lklusb: pre-bind qscratch GEN=%#ux HS=%#ux SS=%#ux\n",
			lklhost_mmio_r32(0x4ef8808ULL), hs,
			lklhost_mmio_r32(0x4ef8830ULL));
		if(hs != 0xdeadbeefU)
			lklhost_mmio_w32(0x4ef8810ULL, hs | (1U<<20) | (1U<<28));
	}
	lklmkdir("/sys/kernel/config/usb_gadget/g1", 0755);
	lklwritestr("/sys/kernel/config/usb_gadget/g1/idVendor", "0x1d6b\n");
	lklwritestr("/sys/kernel/config/usb_gadget/g1/idProduct", "0x0104\n");
	lklwritestr("/sys/kernel/config/usb_gadget/g1/bcdDevice", "0x0100\n");
	lklwritestr("/sys/kernel/config/usb_gadget/g1/bcdUSB", "0x0200\n");

	lklmkdir("/sys/kernel/config/usb_gadget/g1/strings/0x409", 0755);
	lklwritestr("/sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber", "harvey01\n");
	lklwritestr("/sys/kernel/config/usb_gadget/g1/strings/0x409/manufacturer", "Harvey\n");
	lklwritestr("/sys/kernel/config/usb_gadget/g1/strings/0x409/product", "Harvey ARM64 USB\n");

	lklmkdir("/sys/kernel/config/usb_gadget/g1/configs/c.1", 0755);
	lklmkdir("/sys/kernel/config/usb_gadget/g1/configs/c.1/strings/0x409", 0755);
	lklwritestr("/sys/kernel/config/usb_gadget/g1/configs/c.1/strings/0x409/configuration", "CDC ACM\n");
	lklwritestr("/sys/kernel/config/usb_gadget/g1/configs/c.1/MaxPower", "250\n");

	lklmkdir("/sys/kernel/config/usb_gadget/g1/functions/acm.usb0", 0755);
	lklsymlink("/sys/kernel/config/usb_gadget/g1/functions/acm.usb0",
		"/sys/kernel/config/usb_gadget/g1/configs/c.1/acm.usb0");

	/* If this guest owns eMMC (mmcblk0), also expose it read-only
	 * over USB Mass Storage for fast host extraction. */
	if(lklopen("/sys/bus/platform/devices/4744000.mmc") >= 0){
		int tries;
		long sfd;
		char dbuf[32];

		for(tries = 0; tries < 20; tries++){
			sfd = lklopen("/sys/block/mmcblk0/dev");
			if(sfd >= 0){
				long rn = lklread(sfd, dbuf, sizeof dbuf - 1);
				lklclose(sfd);
				if(rn > 0){
					char *col;
					dbuf[rn] = 0;
					col = strchr(dbuf, ':');
					if(col != nil){
						ulong maj = strtoul(dbuf, nil, 10);
						ulong min = strtoul(col + 1, nil, 10);
						lklmkdir("/dev", 0755);
						lklmknod("/dev/mmcblk0", 0060600,
							((maj & 0xfff) << 8) | (min & 0xff) |
							((min & ~0xff) << 12));
						lklmkdir("/sys/kernel/config/usb_gadget/g1/functions/mass_storage.usb0", 0755);
						lklwritestr("/sys/kernel/config/usb_gadget/g1/functions/mass_storage.usb0/lun.0/ro", "1\n");
						lklwritestr("/sys/kernel/config/usb_gadget/g1/functions/mass_storage.usb0/lun.0/removable", "1\n");
						lklwritestr("/sys/kernel/config/usb_gadget/g1/functions/mass_storage.usb0/lun.0/file", "/dev/mmcblk0\n");
						lklsymlink("/sys/kernel/config/usb_gadget/g1/functions/mass_storage.usb0",
							"/sys/kernel/config/usb_gadget/g1/configs/c.1/mass_storage.usb0");
						lkl_printf("lklusb: attached /dev/mmcblk0 (ro) to mass_storage.usb0\n");
					}
				}
				break;
			}
			lklsleep(250);
		}
	}

	r = lklwritestr("/sys/kernel/config/usb_gadget/g1/UDC", udc);
	if(r < 0){
		lkl_printf("lklusb: bind UDC %s failed: %ld\n", udc, r);
		return -1;
	}
	lkl_printf("lklusb: bound CDC-ACM gadget to %s\n", udc);
	{
		extern u32 lklhost_mmio_r32(u64);
		lklsleep(50);
		lkl_printf("lklusb: dwc3 GCTL=%#ux PHY2=%#ux DCTL=%#ux DEVTEN=%#ux DSTS=%#ux EVTCNT=%#ux\n",
			lklhost_mmio_r32(0x4e0c110ULL),
			lklhost_mmio_r32(0x4e0c200ULL),
			lklhost_mmio_r32(0x4e0c704ULL),
			lklhost_mmio_r32(0x4e0c708ULL),
			lklhost_mmio_r32(0x4e0c70cULL),
			lklhost_mmio_r32(0x4e0c40cULL));
	}
	return 0;
}

typedef struct Ringbuf Ringbuf;
struct Ringbuf {
	Lock	lk;
	uchar	data[65536];
	int	head;
	int	tail;
	int	eof;
};

static int
rbput(Ringbuf *rb, uchar *src, int n)
{
	int i;

	lock(&rb->lk);
	for(i = 0; i < n; i++){
		int next = (rb->head + 1) % sizeof(rb->data);
		if(next == rb->tail)
			break;
		rb->data[rb->head] = src[i];
		rb->head = next;
	}
	unlock(&rb->lk);
	return i;
}

static int
rbget(Ringbuf *rb, uchar *dst, int max)
{
	int i;

	lock(&rb->lk);
	for(i = 0; i < max && rb->tail != rb->head; i++){
		dst[i] = rb->data[rb->tail];
		rb->tail = (rb->tail + 1) % sizeof(rb->data);
	}
	unlock(&rb->lk);
	return i;
}

extern int harvey_rfork_call(void *newsp, void *arg, void (*fn)(void*), int flags);

typedef struct Rcreaderarg Rcreaderarg;
struct Rcreaderarg {
	Ringbuf *rb;
	int fd;
};

static void
rcreader(void *v)
{
	Rcreaderarg *a = v;
	uchar tmp[512];
	long rn;

	for(;;){
		uchar *p;
		long rem, w;
		rn = read(a->fd, tmp, sizeof tmp);
		if(rn <= 0){
			a->rb->eof = 1;
			exits(nil);
		}
		p = tmp;
		rem = rn;
		while(rem > 0){
			w = rbput(a->rb, p, rem);
			if(w > 0){
				p += w;
				rem -= w;
			}else
				sleep(5);
		}
	}
}

static void
ensurettygs0(void)
{
	long fd, n;
	char buf[32], *colon;
	ulong maj, min, dev;

	lklmkdir("/dev", 0755);
	lklmount("devtmpfs", "/dev", "devtmpfs");
	fd = lklopenflags("/dev/ttyGS0", O_Rdwr | O_Nonblock);
	if(fd >= 0){
		lklclose(fd);
		return;
	}
	fd = lklopen("/sys/class/tty/ttyGS0/dev");
	if(fd < 0){
		lkl_printf("lklusb: /sys/class/tty/ttyGS0/dev: %ld\n", fd);
		return;
	}
	n = lklread(fd, buf, sizeof(buf) - 1);
	lklclose(fd);
	if(n <= 0)
		return;
	buf[n] = 0;
	colon = strchr(buf, ':');
	if(colon == nil)
		return;
	*colon = 0;
	maj = strtoul(buf, nil, 10);
	min = strtoul(colon + 1, nil, 10);
	dev = ((maj & 0xfff) << 8) | (min & 0xff) | ((min & ~0xffUL) << 12);
	n = lklmknod("/dev/ttyGS0", 0020600, dev);
	lkl_printf("lklusb: mknod /dev/ttyGS0 (%lud:%lud) -> %ld\n", maj, min, n);
}

static void
usbgadgetservice(char *udc)
{
	long ttyfd, n, w, off;
	int inpipe[2], outpipe[2], i;
	Ringbuf *rb;
	uchar *rxbuf, *txbuf;

	USED(udc);
	ensurettygs0();
	ttyfd = -1;
	for(i = 0; i < 50 && ttyfd < 0; i++){
		ttyfd = lklopenflags("/dev/ttyGS0", O_Rdwr | O_Nonblock);
		if(ttyfd < 0)
			lklsleep(100);
	}
	if(ttyfd < 0){
		lkl_printf("lklusb: /dev/ttyGS0 open failed: %ld\n", ttyfd);
		return;
	}
	ttyraw(ttyfd);
	lkl_printf("lklusb: /dev/ttyGS0 open (fd %ld)\n", ttyfd);

	if(pipe(inpipe) < 0 || pipe(outpipe) < 0){
		lkl_printf("lklusb: pipe failed: %r\n");
		lklclose(ttyfd);
		return;
	}

	switch(rfork(RFPROC|RFFDG|RFNAMEG)){
	case -1:
		lkl_printf("lklusb: rfork rc failed: %r\n");
		lklclose(ttyfd);
		return;
	case 0: {
		char a0[16], a1[8], a2[8], a3[16];
		char *rcargv[5];
		snprint(a0, sizeof a0, "%s", "/boot/rc");
		snprint(a1, sizeof a1, "%s", "-i");
		snprint(a2, sizeof a2, "%s", "-m");
		snprint(a3, sizeof a3, "%s", "/boot/rcmain");
		rcargv[0] = a0;
		rcargv[1] = a1;
		rcargv[2] = a2;
		rcargv[3] = a3;
		rcargv[4] = nil;
		dup(inpipe[0], 0);
		dup(outpipe[1], 1);
		dup(outpipe[1], 2);
		close(inpipe[0]);
		close(inpipe[1]);
		close(outpipe[0]);
		close(outpipe[1]);
		exec(rcargv[0], rcargv);
		exits("exec rc");
	}
	}
	close(inpipe[0]);
	close(outpipe[1]);

	rb = mallocz(sizeof *rb, 1);
	rxbuf = malloc(1024);
	txbuf = malloc(1024);
	if(rb == nil || rxbuf == nil || txbuf == nil){
		lkl_printf("lklusb: malloc failed\n");
		return;
	}

	/*
	 * Harvey-only reader thread: drains rc stdout/stderr into the BSEG
	 * ring buffer so only the T1 BSEG-stack thread touches LKL syscalls.
	 */
	{
		uchar *rcst = mallocz(64*1024, 1);
		Rcreaderarg *rcarg = mallocz(sizeof *rcarg, 1);
		if(rcst == nil || rcarg == nil){
			lkl_printf("lklusb: reader alloc failed\n");
			return;
		}
		rcarg->rb = rb;
		rcarg->fd = outpipe[0];
		if(harvey_rfork_call(rcst + 64*1024 - 16, rcarg, rcreader,
		    RFPROC|RFMEM|RFNOWAIT) < 0){
			lkl_printf("lklusb: rfork reader failed: %r\n");
			return;
		}
	}

	for(;;){
		int didwork = 0;

		n = lklread(ttyfd, rxbuf, 1024);
		if(n > 0){
			if(memchr(rxbuf, '~', n) != nil){
				char line[80];
				int k, llen, loff;
				for(k = 0; k < 100; k++){
					llen = snprint(line, sizeof line,
						"TXLINE %04d 0123456789abcdef0123456789abcdef0123456789abcdef\n", k);
					loff = 0;
					while(loff < llen){
						w = lklwrite(ttyfd, line + loff, llen - loff);
						if(w > 0) loff += w;
						else if(w == -11 || w == 0) lklsleep(5);
						else break;
					}
				}
				didwork = 1;
				continue;
			}
			write(inpipe[1], rxbuf, n);
			didwork = 1;
		}else if(n < 0 && n != -11){
			/* Wait quietly if USB host isn't connected yet. */
			lklsleep(50);
		}

		n = rbget(rb, txbuf, 1024);
		if(n > 0){
			off = 0;
			while(off < n){
				w = lklwrite(ttyfd, txbuf + off, n - off);
				if(w > 0){
					off += w;
					continue;
				}
				if(w == -11 || w == 0){
					lklsleep(5);
					continue;
				}
				lkl_printf("lklusb: ttyGS0 write err %ld\n", w);
				break;
			}
			didwork = 1;
		}
		if(!didwork)
			lklsleep(5);
	}
}

static void
dwc3watch(void *arg)
{
	extern u32 lklhost_mmio_r32(u64);
	int i;
	u32 dctl, dsts, evt, cmd, prev = 0xdeadbeefU;

	USED(arg);
	for(i = 0; i < 10; i++){
		dctl = lklhost_mmio_r32(0x4e0c704ULL);
		dsts = lklhost_mmio_r32(0x4e0c70cULL);
		evt = lklhost_mmio_r32(0x4e0c40cULL);
		cmd = lklhost_mmio_r32(0x4e0c80cULL);
		if(i == 0 || dsts != prev || evt != 0){
			lkl_printf("lklusb: dwc3 t=%ds DCTL=%#ux DSTS=%#ux EVTCNT=%#ux DEP0CMD=%#ux\n",
				i * 2, dctl, dsts, evt, cmd);
			prev = dsts;
		}
		sleep(2000);
	}
	exits(nil);
}

void
usbservice(void)
{
	long fd, n;
	char path[32], udc[64];
	Uevent ev;
	int i, shift, kbd, logged;
	char c;

	if(setupgadget(udc, sizeof udc) == 0){
		extern void lkl9p_start(char*);
		ensurettygs0();
		if(strcmp(udc, "4e00000.dwc3") == 0){
			uchar *st = mallocz(16*1024, 1);
			if(st != nil)
				harvey_rfork_call(st + 16*1024 - 16, nil, dwc3watch,
				    RFPROC|RFMEM|RFNOWAIT);
		}
		lkl9p_start(lklsrvname);
		return;
	}

	/*
	 * No UDC in this guest (the control guest owns USB): still
	 * export the filesystem so drivers can be inspected and the
	 * reboot watcher runs, then fall through to the input loop.
	 */
	{
		extern void lkl9p_start(char*);
		lkl9p_start(lklsrvname);
	}

	fd = -1;
	for(i = 0; i < 200 && fd < 0; i++){
		snprint(path, sizeof path, "/dev/input/event0");
		fd = lklopen(path);
		if(fd < 0)
			lklsleep(100);
	}
	if(fd < 0){
		lkl_printf("lklusb: no /dev/input/event0 after 20s\n");
		return;
	}
	lkl_printf("lklusb: input device up (fd %ld)\n", fd);

	kbd = open("/dev/kbd", OWRITE);
	if(kbd < 0)
		lkl_printf("lklusb: /dev/kbd: %r (logging only)\n");

	shift = 0;
	logged = 0;
	for(;;){
		n = lklread(fd, &ev, sizeof ev);
		if(n < (long)sizeof ev){
			lkl_printf("lklusb: read %ld, reopening\n", n);
			lklclose(fd);
			lklsleep(500);
			fd = lklopen(path);
			if(fd < 0)
				lklsleep(1000);
			continue;
		}
		if(ev.type != EvKey)
			continue;
		if(ev.code == KeyLeftShift || ev.code == KeyRightShift){
			shift = ev.value != 0;
			continue;
		}
		if(ev.value == 0)	/* release */
			continue;
		c = translate(ev.code, shift);
		if(c == 0)
			continue;
		if(kbd >= 0){
			if(write(kbd, &c, 1) != 1)
				lkl_printf("lklusb: kbd write: %r\n");
		}
		if(logged < 200){
			lkl_printf("lklusb: key code %d -> %d\n", ev.code, c);
			logged++;
		}
	}
}
