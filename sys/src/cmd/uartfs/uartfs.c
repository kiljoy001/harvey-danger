/* uartfs: a 9P file server that owns a UART through the kernel resource
 * grant.  It discovers the console UART from #P, maps its MMIO with
 * segattach, and serves data, status, and ctl over /srv/uart. */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

enum {
	Uartdw8250,	/* Synopsys DW 8250 / Rockchip rk3568 */
	Uartpl011,	/* ARM PrimeCell PL011 */

	/* DW 8250 register byte offsets, accessed as 32-bit words. */
	Dwthr = 0x00,	/* transmit holding */
	Dwrbr = 0x00,	/* receive buffer */
	Dwlsr = 0x14,	/* line status: bit 0 = data ready */
	Dwusr = 0x7c,	/* status: bit 1 = transmit FIFO not full */

	/* PL011 register byte offsets, accessed as 32-bit words. */
	Pl011dr = 0x00,
	Pl011fr = 0x18,	/* bit 4 = RX empty, bit 5 = TX full */
};

typedef struct Uart Uart;
struct Uart {
	QLock lk;
	volatile u32 *regs;
	u64 base;
	u64 size;
	u32 type;
};

static Uart uart;
static char uartname[64];

/* Build "#P/<dev>/<file>".  Avoids the FP-trapping varargs formatter. */
static char *
mkpath(char *dst, char *dev, char *file)
{
	strcpy(dst, "#P/");
	strcat(dst, dev);
	strcat(dst, "/");
	strcat(dst, file);
	return dst;
}

/* Report an error on the console and exit. */
static void
fail(char *msg)
{
	write(2, msg, strlen(msg));
	write(2, "\n", 1);
	_exits(msg);
}

static long
readfile(char *path, char *buf, long n)
{
	int fd;
	long m;

	fd = open(path, OREAD);
	if(fd < 0)
		return -1;
	m = read(fd, buf, n - 1);
	close(fd);
	if(m < 0)
		return -1;
	buf[m] = 0;
	return m;
}

/* Return 0 if #P/<name> is the console UART and fill *out. */
static int
probedev(char *name, u64 *base, u64 *size, u32 *type)
{
	char path[128], buf[128];
	long n;

	mkpath(path, name, "class");
	n = readfile(path, buf, sizeof buf);
	if(n < 0 || strcmp(buf, "uart\n") != 0)
		return -1;

	mkpath(path, name, "reg");
	n = readfile(path, buf, sizeof buf);
	if(n < 0)
		return -1;
	*base = strtoull(buf, nil, 0);

	mkpath(path, name, "size");
	n = readfile(path, buf, sizeof buf);
	if(n < 0)
		return -1;
	*size = strtoull(buf, nil, 0);

	mkpath(path, name, "compatible");
	n = readfile(path, buf, sizeof buf);
	if(n < 0)
		return -1;
	*type = strstr(buf, "pl011") != nil ? Uartpl011 : Uartdw8250;
	return 0;
}

/* Return nonzero if #P/<name> is the console UART (stdout-path). */
static int
isconsole(char *name)
{
	char path[128], buf[16];
	long n;

	mkpath(path, name, "console");
	n = readfile(path, buf, sizeof buf);
	return n > 0 && buf[0] == '1';
}

/* Find the console UART in #P, falling back to the first enabled UART. */
static int
discover(void)
{
	uchar dirbuf[4096];
	char strs[512], fallback[64];
	u64 fbase, fsize;
	u32 ftype;
	Dir d;
	int fd, found, havefallback;
	long i, n;

	found = -1;
	havefallback = 0;
	fd = open("#P", OREAD);
	if(fd < 0)
		return -1;
	n = read(fd, dirbuf, sizeof dirbuf);
	close(fd);
	if(n <= 0)
		return -1;
	for(i = 0; i < n; i += GBIT16(dirbuf + i) + BIT16SZ){
		u64 base, size;
		u32 type;

		if(convM2D(dirbuf + i, GBIT16(dirbuf + i) + BIT16SZ,
		   &d, strs) == 0)
			break;
		if(strcmp(d.name, ".") == 0)
			continue;
		if(probedev(d.name, &base, &size, &type) < 0)
			continue;
		if(isconsole(d.name)){
			uart.base = base;
			uart.size = size;
			uart.type = type;
			strcpy(uartname, d.name);
			found = 0;
			break;
		}
		if(!havefallback){
			fbase = base;
			fsize = size;
			ftype = type;
			strcpy(fallback, d.name);
			havefallback = 1;
		}
	}
	if(found < 0 && havefallback){
		uart.base = fbase;
		uart.size = fsize;
		uart.type = ftype;
		strcpy(uartname, fallback);
		found = 0;
	}
	return found;
}

static int
uartgetc(void)
{
	if(uart.type == Uartpl011){
		if(uart.regs[Pl011fr / 4] & (1 << 4))
			return -1;
		return uart.regs[Pl011dr / 4] & 0xff;
	}
	if((uart.regs[Dwlsr / 4] & 1) == 0)
		return -1;
	return uart.regs[Dwrbr / 4] & 0xff;
}

static void
uartputc(int c)
{
	if(uart.type == Uartpl011){
		while(uart.regs[Pl011fr / 4] & (1 << 5))
			;
		uart.regs[Pl011dr / 4] = c;
		return;
	}
	while((uart.regs[Dwusr / 4] & (1 << 1)) == 0)
		;
	uart.regs[Dwthr / 4] = c;
}

static void
uartwrite(char *s, int n)
{
	while(n-- > 0)
		uartputc(*s++);
}

static void
fsread(Req *r)
{
	char *name;
	char buf[128];
	int c, n;

	name = r->fid->file->aux;
	if(strcmp(name, "data") == 0){
		n = 0;
		while(n < r->ifcall.count && (c = uartgetc()) >= 0)
			buf[n++] = c;
		if(n > 0)
			memmove(r->ofcall.data, buf, n);
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}
	if(strcmp(name, "status") == 0){
		strcpy(buf, uart.type == Uartpl011 ? "pl011\n" : "dw8250\n");
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	respond(r, "read not permitted");
}

static void
fswrite(Req *r)
{
	char *name;

	name = r->fid->file->aux;
	if(strcmp(name, "data") == 0){
		uartwrite((char *)r->ifcall.data, r->ifcall.count);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	if(strcmp(name, "ctl") == 0){
		/* Configuration is accepted but not interpreted yet. */
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	respond(r, "write not permitted");
}

static void
fsdestroyfile(File *f)
{
	USED(f);
}

Srv fs = {
	.read = fsread,
	.write = fswrite,
};

void
main(int argc, char **argv)
{
	u64 len;
	void *map;

	USED(argc);
	USED(argv);
	if(discover() < 0)
		fail("uartfs: no UART in #P");
	len = (uart.size + 0x1fffff) & ~0x1fffffULL;
	if(len == 0)
		len = 0x200000;
	map = segattach(0, uartname, 0, len);
	if(map == (void *)-1)
		fail("uartfs: segattach failed");
	uart.regs = (volatile u32 *)((uchar *)map + (uart.base & 0x1fffff));

	fs.tree = alloctree(nil, nil, DMDIR | 0777, fsdestroyfile);
	createfile(fs.tree->root, "data", nil, 0666, "data");
	createfile(fs.tree->root, "status", nil, 0444, "status");
	createfile(fs.tree->root, "ctl", nil, 0666, "ctl");

	uartwrite("HARVEY-ARM64 UARTFS PASS\n",
		sizeof("HARVEY-ARM64 UARTFS PASS\n") - 1);
	postmountsrv(&fs, "uart", nil, MREPL | MCREATE);
	exits(nil);
}
