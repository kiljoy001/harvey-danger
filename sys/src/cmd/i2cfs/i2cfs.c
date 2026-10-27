/* i2cfs: a 9P file server that owns the RK356x I2C controllers through the
 * kernel resource grant.  It discovers the six controllers and the
 * CRU/PMUCRU from #P, maps their MMIO with segattach, enables each
 * controller's clock, and serves per-controller ctl/data files over
 * /srv/i2c.  Transfers are polled against the RK3x interrupt-pending
 * register; register map and FSM translated from Linux i2c-rk3x.c.
 *
 * Interface:
 *   /status        discovered controllers (read)
 *   /i2cN/ctl      "w <addr> <b0> <b1> ..."        write bytes
 *                  "r <addr> <reg> <count>"        read register
 *                  "r <addr> <count>"              read (no register)
 *   /i2cN/data     hex bytes from the last read
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "../o9drvman/health.h"
#include <rockchip/rk356x.h>

enum {
	Maxi2c = 6,
	Fctl = 0,
	Fdata = 1,
};

typedef struct I2c I2c;
struct I2c {
	char name[64];
	u64 base;
	volatile u32 *regs;
	u8 rbuf[32];
	int rlen;
};

typedef struct Gpio Gpio;
struct Gpio {
	QLock lk;
	I2c i2c[Maxi2c];
	int ni2c;
	volatile u32 *cru;
	volatile u32 *pmucru;
	u64 crubase;
	u64 pmucrubase;
};

static Gpio g;

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

static void
addi2c(char *name, u64 base)
{
	int i, j;

	if(g.ni2c == Maxi2c)
		return;
	for(i = 0; i < g.ni2c; i++)
		if(base < g.i2c[i].base)
			break;
	for(j = g.ni2c; j > i; j--)
		g.i2c[j] = g.i2c[j - 1];
	memset(&g.i2c[i], 0, sizeof g.i2c[i]);
	strncpy(g.i2c[i].name, name, sizeof(g.i2c[i].name) - 1);
	g.i2c[i].base = base;
	g.ni2c++;
}

static int
discover(void)
{
	uchar dirbuf[16384];
	char strs[512], cruname[64], pmucruname[64];
	u64 base, crubase, pmucrubase;
	Dir d;
	int fd, havecru, havepmucru, i;
	long n, m;

	g.ni2c = 0;
	crubase = pmucrubase = 0;
	havecru = havepmucru = 0;
	cruname[0] = pmucruname[0] = 0;

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
		if(probe(d.name, "i2c", nil, &base) == 0){
			addi2c(d.name, base);
			continue;
		}
		if(probe(d.name, "cru", "pmucru", &base) == 0){
			pmucrubase = base;
			strcpy(pmucruname, d.name);
			havepmucru = 1;
			continue;
		}
		if(probe(d.name, "cru", "cru", &base) == 0){
			crubase = base;
			strcpy(cruname, d.name);
			havecru = 1;
		}
	}
	if(g.ni2c == 0)
		return -1;
	for(i = 0; i < g.ni2c; i++)
		g.i2c[i].regs = mapdev(g.i2c[i].name, g.i2c[i].base, 0x1000);
	if(havecru)
		g.cru = mapdev(cruname, crubase, 0x10000);
	if(havepmucru)
		g.pmucru = mapdev(pmucruname, pmucrubase, 0x10000);
	g.crubase = crubase;
	g.pmucrubase = pmucrubase;
	return 0;
}

/* Enable PCLK + CLK for controller n via the CRU/PMUCRU clock gate.
 * Gates use HIWORD-MASK + SET-TO-DISABLE: write 1<<(16+b) to enable bit b. */
static void
i2cclkenable(int n)
{
	volatile u32 *cru;
	u32 reg, bit;

	if(n == 0){
		cru = g.pmucru;
		reg = RK_PMU_CLKGATE(1) / 4;
		bit = 0;
	}else{
		cru = g.cru;
		reg = RK_CRU_CLKGATE(30) / 4;
		bit = 2 * (n - 1);
	}
	if(cru == nil)
		return;
	cru[reg] = (1u << (16 + bit)) | (1u << (16 + bit + 1));
}

/* Program CLKDIV for ~100 kHz SCL at the RK356x 100 MHz I2C clock. */
static void
i2cinit(I2c *i)
{
	u32 total, divl, divh;

	total = 100000000 / (100000 * 8);
	if(total < 2)
		total = 2;
	divl = total * 54 / 100;
	divh = total - divl;
	i->regs[RK_I2C_CLKDIV / 4] = ((divh - 1) << 16) | ((divl - 1) & 0xffff);
}

/* Poll IPD until any bit in mask is set; return 0 on success, -1 on timeout. */
static int
i2cwait(I2c *i, u32 mask)
{
	int t;

	for(t = 0; t < 200; t++){
		if(i->regs[RK_I2C_IPD / 4] & mask)
			return 0;
		sleep(1);
	}
	return -1;
}

static int
i2cwrite(I2c *i, u8 addr, u8 *data, int len)
{
	volatile u32 *r = i->regs;
	u32 con, val;
	int total, w, b, cnt;

	if(len < 0 || len > 31)
		return -1;
	total = len + 1;	/* address byte + data */

	/* fill TX FIFO: (addr<<1) then data, 4 bytes per word */
	cnt = 0;
	for(w = 0; w < 8 && cnt < total; w++){
		val = 0;
		for(b = 0; b < 4 && cnt < total; b++){
			u8 byte = cnt == 0 ? (u8)(addr << 1) : data[cnt - 1];
			val |= (u32)byte << (b * 8);
			cnt++;
		}
		r[(RK_I2C_TXBUF + w * 4) / 4] = val;
	}

	r[RK_I2C_IPD / 4] = RK_I2C_INT_ALL;
	con = RK_I2C_EN | RK_I2C_MOD_TX | RK_I2C_START | RK_I2C_ACTACK;
	r[RK_I2C_CON / 4] = con;
	r[RK_I2C_MTXCNT / 4] = total;

	if(i2cwait(i, RK_I2C_INT_MBTF | RK_I2C_INT_NAK) < 0)
		return -1;
	if(r[RK_I2C_IPD / 4] & RK_I2C_INT_NAK)
		return -1;

	r[RK_I2C_CON / 4] = con | RK_I2C_STOP;
	i2cwait(i, RK_I2C_INT_STOP);
	r[RK_I2C_IPD / 4] = RK_I2C_INT_ALL;
	return 0;
}

static int
i2cread(I2c *i, u8 addr, u8 *reg, int reglen, u8 *buf, int len)
{
	volatile u32 *r = i->regs;
	u32 con, val, regaddr;
	int k;

	if(reglen < 0 || reglen > 3 || len <= 0 || len > 32)
		return -1;

	if(reglen == 0){
		r[RK_I2C_MRXADDR / 4] = ((u32)(addr << 1) | 1) | RK_I2C_ADDR_VALID(0);
		r[RK_I2C_MRXRADDR / 4] = 0;
	}else{
		r[RK_I2C_MRXADDR / 4] = ((u32)(addr << 1)) | RK_I2C_ADDR_VALID(0);
		regaddr = 0;
		for(k = 0; k < reglen; k++)
			regaddr |= ((u32)reg[k] << (k * 8)) | RK_I2C_ADDR_VALID(k);
		r[RK_I2C_MRXRADDR / 4] = regaddr;
	}

	r[RK_I2C_IPD / 4] = RK_I2C_INT_ALL;
	con = RK_I2C_EN | RK_I2C_MOD_REG_TX | RK_I2C_START |
		RK_I2C_LASTACK | RK_I2C_ACTACK;
	r[RK_I2C_CON / 4] = con;
	r[RK_I2C_MRXCNT / 4] = len;

	if(i2cwait(i, RK_I2C_INT_MBRF | RK_I2C_INT_NAK) < 0)
		return -1;
	if(r[RK_I2C_IPD / 4] & RK_I2C_INT_NAK)
		return -1;

	for(k = 0; k < len; k++){
		if(k % 4 == 0)
			val = r[(RK_I2C_RXBUF + (k / 4) * 4) / 4];
		buf[k] = (val >> ((k % 4) * 8)) & 0xff;
	}

	r[RK_I2C_CON / 4] = con | RK_I2C_STOP;
	i2cwait(i, RK_I2C_INT_STOP);
	r[RK_I2C_IPD / 4] = RK_I2C_INT_ALL;
	return 0;
}

/* Parse a ctl command; on success update the controller's read buffer. */
static char *
ctlcmd(I2c *i, char *s)
{
	char *p;
	u8 addr, reg[3], buf[32];
	int k, n;

	while(*s == ' ' || *s == '\t')
		s++;
	if(*s == 'w'){
		s++;
		addr = strtol(s, &p, 0);
		if(p == s)
			return "bad addr";
		for(n = 0; n < 32; n++){
			while(*p == ' ' || *p == '\t')
				p++;
			if(*p == 0 || *p == '\n')
				break;
			buf[n] = strtol(p, &p, 0);
		}
		if(i2cwrite(i, addr, buf, n) < 0)
			return "write failed";
		i->rlen = 0;
		return nil;
	}
	if(*s == 'r'){
		s++;
		addr = strtol(s, &p, 0);
		if(p == s)
			return "bad addr";
		for(k = 0; k < 3 && *p != 0; ){
			while(*p == ' ' || *p == '\t')
				p++;
			if(*p == 0 || *p == '\n')
				break;
			reg[k++] = strtol(p, &p, 0);
		}
		/* "r addr count" vs "r addr reg count" */
		if(k == 1){
			n = reg[0];
			if(i2cread(i, addr, nil, 0, buf, n) < 0)
				return "read failed";
		}else{
			n = reg[k - 1];
			if(i2cread(i, addr, reg, k - 1, buf, n) < 0)
				return "read failed";
		}
		if(n < 0 || n > 32)
			return "bad count";
		memmove(i->rbuf, buf, n);
		i->rlen = n;
		return nil;
	}
	return "want w|r";
}

static void
fsread(Req *r)
{
	char buf[256];
	uintptr tag;
	int bank, ftype, i, n;

	tag = (uintptr)r->fid->file->aux;
	if(tag == 0){
		n = 0;
		n += snprint(buf + n, sizeof(buf) - n, "i2c %d\n", g.ni2c);
		for(i = 0; i < g.ni2c; i++)
			n += snprint(buf + n, sizeof(buf) - n,
				"i2c%d base 0x%llx %s\n", i,
				(unsigned long long)g.i2c[i].base,
				g.i2c[i].regs == nil ? "unmapped" : "mapped");
		n += snprint(buf + n, sizeof(buf) - n,
			"cru 0x%llx %s\n", (unsigned long long)g.crubase,
			g.cru == nil ? "unmapped" : "mapped");
		n += snprint(buf + n, sizeof(buf) - n,
			"pmucru 0x%llx %s\n", (unsigned long long)g.pmucrubase,
			g.pmucru == nil ? "unmapped" : "mapped");
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	bank = (tag - 1) >> 2;
	ftype = (tag - 1) & 3;
	if(bank >= g.ni2c || g.i2c[bank].regs == nil){
		respond(r, "controller not mapped");
		return;
	}
	if(ftype == Fdata){
		n = 0;
		for(i = 0; i < g.i2c[bank].rlen && n < (int)sizeof(buf) - 3; i++)
			n += snprint(buf + n, sizeof(buf) - n, "%02x",
				g.i2c[bank].rbuf[i]);
		buf[n++] = '\n';
		buf[n] = 0;
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	respond(r, "read not permitted");
}

static void
fswrite(Req *r)
{
	char buf[64], *e;
	uintptr tag;
	int bank, ftype;
	long n;

	tag = (uintptr)r->fid->file->aux;
	if(tag == 0){
		respond(r, "write not permitted");
		return;
	}
	bank = (tag - 1) >> 2;
	ftype = (tag - 1) & 3;
	if(bank >= g.ni2c || g.i2c[bank].regs == nil){
		respond(r, "controller not mapped");
		return;
	}
	n = r->ifcall.count;
	if(n >= sizeof buf)
		n = sizeof buf - 1;
	memmove(buf, r->ifcall.data, n);
	buf[n] = 0;
	if(ftype != Fctl){
		respond(r, "write not permitted");
		return;
	}
	e = ctlcmd(&g.i2c[bank], buf);
	if(e != nil){
		respond(r, e);
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
	DrvHealth *health;
	File *d;
	char name[16];
	int i;

	USED(argc);
	USED(argv);
	if(discover() < 0)
		fail("i2cfs: no I2C controllers in #P");

	fs.tree = alloctree(nil, nil, DMDIR | 0777, fsdestroyfile);
	createfile(fs.tree->root, "status", nil, 0444, nil);
	for(i = 0; i < g.ni2c; i++){
		if(g.i2c[i].regs == nil)
			continue;
		i2cclkenable(i);
		i2cinit(&g.i2c[i]);
		snprint(name, sizeof name, "i2c%d", i);
		d = createfile(fs.tree->root, name, nil, DMDIR | 0777, nil);
		createfile(d, "ctl", nil, 0666, AUX(i, Fctl));
		createfile(d, "data", nil, 0444, AUX(i, Fdata));
	}

	postmountsrv(&fs, "i2c", nil, MREPL | MCREATE);
	health = drvhealth_openenv();
	if(health != nil){
		drvhealth_ready(health);
		for(;;){
			drvhealth_report(health, DrvHealthHealthy, 0, 0, 0,
			                 0, 0, 0, DrvHealthRecommendNone, "none");
			if(drvhealth_stopwait(health, 1000))
				break;
		}
		drvhealth_close(health);
	}else
		for(;;)
			sleep(1000);
	exits(nil);
}
