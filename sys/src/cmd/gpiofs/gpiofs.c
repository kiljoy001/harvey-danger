/* gpiofs: a 9P file server that owns the RK356x GPIO banks through the
 * kernel resource grant.  It discovers the five banks and the GRF/PMUGRF
 * pinmux from #P, maps their MMIO with segattach, and serves per-bank
 * data/dir/ctl files plus a global status file over /srv/gpio.
 *
 * Interface:
 *   /status          discovered banks and pinmux (read)
 *   /gpioN/data      32-bit pin state; read = actual pins, write = data out
 *   /gpioN/dir       32-bit direction (1 = output)
 *   /gpioN/ctl       "N in", "N out [0|1]"  (muxes the pin to GPIO)
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <rockchip/rk356x.h>

enum {
	Maxbank = 5,
	Fdata = 0,
	Fdir = 1,
	Fctl = 2,
};

typedef struct Bank Bank;
struct Bank {
	char name[64];
	u64 base;
	volatile u32 *regs;
};

typedef struct Gpio Gpio;
struct Gpio {
	QLock lk;
	Bank bank[Maxbank];
	int nbank;
	volatile u32 *grf;
	volatile u32 *pmugrf;
	u64 grfbase;
	u64 pmugrfbase;
};

static Gpio g;

/* Encode a bank/type pair in a file's aux pointer (0 = root status). */
#define AUX(bank, ftype) ((void *)((uintptr)(1 + ((bank) << 2) + (ftype))))

static char *
mkpath(char *dst, char *dev, char *file)
{
	strcpy(dst, "#P/");
	strcat(dst, dev);
	strcat(dst, "/");
	strcat(dst, file);
	return dst;
}

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

/* Probe #P/<name>; return 0 and fill *base if it matches class/compat. */
static int
probe(char *name, char *class, char *compat, u64 *base)
{
	char path[128], buf[128];
	long n;

	mkpath(path, name, "class");
	n = readfile(path, buf, sizeof buf);
	if(n < 0)
		return -1;
	for(n = 0; buf[n] != 0 && buf[n] != '\n'; n++)
		;
	buf[n] = 0;
	if(strcmp(buf, class) != 0)
		return -1;

	if(compat != nil){
		mkpath(path, name, "compatible");
		n = readfile(path, buf, sizeof buf);
		if(n < 0 || strstr(buf, compat) == nil)
			return -1;
	}

	mkpath(path, name, "reg");
	n = readfile(path, buf, sizeof buf);
	if(n < 0)
		return -1;
	*base = strtoull(buf, nil, 0);
	return 0;
}

static volatile u32 *
mapdev(char *name, u64 base, u64 size)
{
	u64 len;
	void *map;

	len = (size + 0x1fffff) & ~0x1fffffULL;
	if(len == 0)
		len = 0x200000;
	map = segattach(0, name, 0, len);
	if(map == (void *)-1)
		return nil;
	return (volatile u32 *)((uchar *)map + (base & 0x1fffff));
}

/* Insert a bank into the table keeping it sorted by base address. */
static void
addbank(char *name, u64 base)
{
	int i, j;

	if(g.nbank == Maxbank)
		return;
	for(i = 0; i < g.nbank; i++)
		if(base < g.bank[i].base)
			break;
	for(j = g.nbank; j > i; j--)
		g.bank[j] = g.bank[j - 1];
	strncpy(g.bank[i].name, name, sizeof(g.bank[i].name) - 1);
	g.bank[i].name[sizeof(g.bank[i].name) - 1] = 0;
	g.bank[i].base = base;
	g.bank[i].regs = nil;
	g.nbank++;
}

static int
discover(void)
{
	uchar dirbuf[16384];
	char strs[512], grfname[64], pmugrfname[64];
	u64 base, grfbase, pmugrfbase;
	Dir d;
	int fd, havegrf, havepmugrf;
	long i, n, m;

	g.nbank = 0;
	grfbase = pmugrfbase = 0;
	havegrf = havepmugrf = 0;
	grfname[0] = pmugrfname[0] = 0;

	fd = open("#P", OREAD);
	if(fd < 0)
		return -1;
	n = 0;
	while(n < (long)sizeof dirbuf){
		m = read(fd, dirbuf + n, sizeof dirbuf - n);
		if(m <= 0)
			break;
		n += m;
	}
	close(fd);
	if(n <= 0)
		return -1;
	for(i = 0; i < n; i += GBIT16(dirbuf + i) + BIT16SZ){
		if(convM2D(dirbuf + i, GBIT16(dirbuf + i) + BIT16SZ, &d, strs) == 0)
			break;
		if(strcmp(d.name, ".") == 0)
			continue;
		if(probe(d.name, "gpio", "gpio-bank", &base) == 0){
			addbank(d.name, base);
			continue;
		}
		if(probe(d.name, "grf", "pmugrf", &base) == 0){
			pmugrfbase = base;
			strcpy(pmugrfname, d.name);
			havepmugrf = 1;
			continue;
		}
		if(probe(d.name, "grf", "grf", &base) == 0){
			grfbase = base;
			strcpy(grfname, d.name);
			havegrf = 1;
		}
	}

	if(g.nbank == 0)
		return -1;

	/* Map banks, then the pinmux, in base order. */
	for(i = 0; i < g.nbank; i++)
		g.bank[i].regs = mapdev(g.bank[i].name, g.bank[i].base, 0x100);
	if(havegrf)
		g.grf = mapdev(grfname, grfbase, 0x10000);
	if(havepmugrf)
		g.pmugrf = mapdev(pmugrfname, pmugrfbase, 0x10000);
	g.grfbase = grfbase;
	g.pmugrfbase = pmugrfbase;
	return 0;
}

/* 16-bit half register access with the V2 write-enable mask. */
static u32
gpio_get(volatile u32 *reg)
{
	return *reg & 0xffff;
}

static void
gpio_set(volatile u32 *reg, u32 val)
{
	*reg = (val & 0xffff) | 0xffff0000;
}

static void
setmux(int bank, int pin, u32 func)
{
	volatile u32 *base;
	u32 reg, shift, val;

	base = bank == 0 ? g.pmugrf : g.grf;
	if(base == nil)
		return;
	reg = RK_IOMUX_REG(bank, pin);
	shift = RK_IOMUX_SHIFT(pin);
	val = base[reg / 4];
	val &= ~(0xfu << shift);
	val |= (func & 0xf) << shift;
	base[reg / 4] = val;
}

static void
setdir(int bank, int pin, int out)
{
	Bank *b;
	volatile u32 *reg;
	u32 off, shift, val;

	b = &g.bank[bank];
	if(b->regs == nil)
		return;
	off = pin < 16 ? RK_GPIO_DDR_L : RK_GPIO_DDR_H;
	shift = pin % 16;
	reg = &b->regs[off / 4];
	val = gpio_get(reg);
	if(out)
		val |= 1u << shift;
	else
		val &= ~(1u << shift);
	gpio_set(reg, val);
	setmux(bank, pin, RK_IOMUX_FUNC_GPIO);
}

static void
setvalue(int bank, int pin, int val)
{
	Bank *b;
	volatile u32 *reg;
	u32 off, shift, v;

	b = &g.bank[bank];
	if(b->regs == nil)
		return;
	off = pin < 16 ? RK_GPIO_DR_L : RK_GPIO_DR_H;
	shift = pin % 16;
	reg = &b->regs[off / 4];
	v = gpio_get(reg);
	if(val)
		v |= 1u << shift;
	else
		v &= ~(1u << shift);
	gpio_set(reg, v);
}

/* "N in" | "N out" | "N out 0|1" */
static char *
ctlcmd(int bank, char *s)
{
	char *p;
	int pin, out, val;

	pin = strtol(s, &p, 0);
	if(p == s || pin < 0 || pin > 31)
		return "bad pin";
	while(*p == ' ' || *p == '\t')
		p++;
	if(strncmp(p, "in", 2) == 0){
		out = 0;
		val = 0;
	}else if(strncmp(p, "out", 3) == 0){
		out = 1;
		p += 3;
		while(*p == ' ' || *p == '\t')
			p++;
		if(*p == '0' || *p == '1')
			val = *p - '0';
		else
			val = 0;
	}else
		return "want in|out";

	setdir(bank, pin, out);
	if(out)
		setvalue(bank, pin, val);
	return nil;
}

static void
fsread(Req *r)
{
	char buf[256];
	uintptr tag;
	int bank, ftype, i, n;
	u32 v;

	tag = (uintptr)r->fid->file->aux;
	if(tag == 0){
		/* /status */
		n = 0;
		n += snprint(buf + n, sizeof(buf) - n, "banks %d\n", g.nbank);
		for(i = 0; i < g.nbank; i++)
			n += snprint(buf + n, sizeof(buf) - n,
				"gpio%d base 0x%llx %s\n", i,
				(unsigned long long)g.bank[i].base,
				g.bank[i].regs == nil ? "unmapped" : "mapped");
		n += snprint(buf + n, sizeof(buf) - n,
			"grf 0x%llx %s\n", (unsigned long long)g.grfbase,
			g.grf == nil ? "unmapped" : "mapped");
		n += snprint(buf + n, sizeof(buf) - n,
			"pmugrf 0x%llx %s\n", (unsigned long long)g.pmugrfbase,
			g.pmugrf == nil ? "unmapped" : "mapped");
		readstr(r, buf);
		respond(r, nil);
		return;
	}

	bank = (tag - 1) >> 2;
	ftype = (tag - 1) & 3;
	if(bank >= g.nbank || g.bank[bank].regs == nil){
		respond(r, "bank not mapped");
		return;
	}
	switch(ftype){
	case Fdata:
		v = g.bank[bank].regs[RK_GPIO_EXT_PORT / 4];
		snprint(buf, sizeof buf, "0x%08x\n", v);
		break;
	case Fdir:
		v = gpio_get(&g.bank[bank].regs[RK_GPIO_DDR_L / 4]);
		v |= gpio_get(&g.bank[bank].regs[RK_GPIO_DDR_H / 4]) << 16;
		snprint(buf, sizeof buf, "0x%08x\n", v);
		break;
	default:
		respond(r, "read not permitted");
		return;
	}
	readstr(r, buf);
	respond(r, nil);
}

static void
fswrite(Req *r)
{
	char buf[64], *e;
	uintptr tag;
	int bank, ftype;
	u32 v, lo, hi;
	long n;

	tag = (uintptr)r->fid->file->aux;
	if(tag == 0){
		respond(r, "write not permitted");
		return;
	}
	bank = (tag - 1) >> 2;
	ftype = (tag - 1) & 3;
	if(bank >= g.nbank || g.bank[bank].regs == nil){
		respond(r, "bank not mapped");
		return;
	}
	n = r->ifcall.count;
	if(n >= sizeof buf)
		n = sizeof buf - 1;
	memmove(buf, r->ifcall.data, n);
	buf[n] = 0;
	switch(ftype){
	case Fdata:
		v = strtoul(buf, nil, 0);
		lo = v & 0xffff;
		hi = (v >> 16) & 0xffff;
		gpio_set(&g.bank[bank].regs[RK_GPIO_DR_L / 4], lo);
		gpio_set(&g.bank[bank].regs[RK_GPIO_DR_H / 4], hi);
		break;
	case Fdir:
		v = strtoul(buf, nil, 0);
		gpio_set(&g.bank[bank].regs[RK_GPIO_DDR_L / 4], v & 0xffff);
		gpio_set(&g.bank[bank].regs[RK_GPIO_DDR_H / 4], (v >> 16) & 0xffff);
		break;
	case Fctl:
		e = ctlcmd(bank, buf);
		if(e != nil){
			respond(r, e);
			return;
		}
		break;
	default:
		respond(r, "write not permitted");
		return;
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
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
	File *d;
	char name[16];
	int i;

	USED(argc);
	USED(argv);
	if(discover() < 0)
		fail("gpiofs: no GPIO banks in #P");

	fs.tree = alloctree(nil, nil, DMDIR | 0777, fsdestroyfile);
	createfile(fs.tree->root, "status", nil, 0444, nil);
	for(i = 0; i < g.nbank; i++){
		snprint(name, sizeof name, "gpio%d", i);
		d = createfile(fs.tree->root, name, nil, DMDIR | 0777, nil);
		createfile(d, "data", nil, 0666, AUX(i, Fdata));
		createfile(d, "dir", nil, 0666, AUX(i, Fdir));
		createfile(d, "ctl", nil, 0666, AUX(i, Fctl));
	}

	postmountsrv(&fs, "gpio", nil, MREPL | MCREATE);
	exits(nil);
}
