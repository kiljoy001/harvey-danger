/*
 * sdprobe: userspace bring-up of the RK3566 SDIO host (sdmmc1 / DW MSHC)
 * and enumeration of the Unisoc UWE5622 WiFi module on it.
 *
 * Stage 1 of the WiFi port: power the module (WL_REG_ON = GPIO0_D3),
 * mux the SDMMC1 pins, enable the clocks, initialise the DesignWare
 * MSHC controller in polled PIO mode, then run CMD5/CMD3/CMD7 and dump
 * the card's CCCR and CIS.
 *
 * Everything is reached through #P: the controller, GRF (pinmux) and
 * CRU (clock gates).  GPIO power comes from the gpiofs service.
 */
#include <u.h>
#include <libc.h>
#include <rockchip/rk356x.h>
#include "wifi_conf.h"

enum {
	/* DesignWare MSHC registers */
	Ctl	= 0x000,
	Pwren	= 0x004,
	Clkdiv	= 0x008,
	Clksrc	= 0x00c,
	Clkena	= 0x010,
	Tmout	= 0x014,
	Ctype	= 0x018,
	Blksiz	= 0x01c,
	Bytcnt	= 0x020,
	Intmask	= 0x024,
	Cmdarg	= 0x028,
	Cmd	= 0x02c,
	Resp0	= 0x030,
	Mintsts	= 0x040,
	Rintsts	= 0x044,
	Status	= 0x048,
	Fifoth	= 0x04c,
	Cdetect	= 0x050,
	RstN	= 0x078,
	Tcbcnt	= 0x05c,
	Tbbcnt	= 0x060,
	Verid	= 0x06c,
	Hcon	= 0x070,
	Bmod	= 0x080,
	Idsts	= 0x08c,
	Idinten	= 0x090,
	Fifo	= 0x200,
	Dbaddr	= 0x088,

	/* CTRL */
	CtlReset	= 1<<0,
	CtlFifoReset	= 1<<1,
	CtlDmaReset	= 1<<2,
	CtlIntEn	= 1<<4,
	CtlDmaEn	= 1<<5,
	CtlUseIdmac	= 1<<25,

	/* CMD */
	CmdStart	= 1u<<31,
	CmdUseHold	= 1u<<29,
	CmdUpdClk	= 1u<<21,
	CmdInit		= 1u<<15,
	CmdStop		= 1u<<14,
	CmdPrvData	= 1u<<13,
	CmdSendStop	= 1u<<12,
	CmdDataExp	= 1u<<9,
	CmdDatWr	= 1u<<10,
	CmdRespCrc	= 1u<<8,
	CmdRespLong	= 1u<<7,
	CmdRespExp	= 1u<<6,

	/* RINTSTS */
	IntCd		= 1<<0,
	IntRespErr	= 1<<1,
	IntCmdDone	= 1<<2,
	IntDataOver	= 1<<3,
	IntTxdr		= 1<<4,
	IntRxdr		= 1<<5,
	IntRcrc		= 1<<6,
	IntDcrc		= 1<<7,
	IntRto		= 1<<8,
	IntDrto		= 1<<9,
	IntHto		= 1<<10,
	IntFrun		= 1<<11,
	IntHle		= 1<<12,
	IntSbe		= 1<<13,
	IntAcD		= 1<<14,
	IntEbe		= 1<<15,

	BmodIdmac	= 1<<7,
	BmodFb		= 1<<1,
	BmodSwr		= 1<<0,
	IdmacTi		= 1<<0,
	IdmacRi		= 1<<1,
	IdmacNi		= 1<<8,

	/* CLKENA */
	ClkenEnable	= 1<<0,
	ClkenLowPwr	= 1<<16,

	/* CTYPE */
	Ctype4Bit	= 1<<0,
	Ctype8Bit	= 1<<16,

	/* SDIO */
	RcaShift	= 16,
	Ocr32_33	= 0x00200000,

	/* clk_sdmmc1 mux 5 = clk_osc0_div_750k (24 MHz / 32); the DW
	 * divider is bypassed (CLKDIV 0) as Linux does for low rates. */
	BaseClock	= 750000,
	InitDiv		= 0,
};

static volatile u32 *mmc, *cru, *grf, *pmugrf;
static u32 lastst;

static u32
rd(volatile u32 *b, int off)
{
	return b[off/4];
}

static void
wr(volatile u32 *b, int off, u32 v)
{
	b[off/4] = v;
}

/* Rockchip write-mask: value in the low half, mask in the high half. */
static void
wm(volatile u32 *b, int off, u32 mask, u32 v)
{
	b[off/4] = (v & mask) | ((mask & 0xffff) << 16);
}

static u8 *
mapnode(char *node, u64 want)
{
	char path[128], buf[128], *p;
	u64 base, size, len;
	u8 *m;
	int fd;

	snprint(path, sizeof path, "#P/%s/reg", node);
	if((fd = open(path, OREAD)) < 0)
		sysfatal("sdprobe: no %s in #P: %r", node);
	if(read(fd, buf, sizeof buf - 1) <= 0)
		sysfatal("sdprobe: cannot read %s reg", node);
	close(fd);
	buf[sizeof buf - 1] = 0;
	base = strtoull(buf, &p, 0);
	snprint(path, sizeof path, "#P/%s/size", node);
	if((fd = open(path, OREAD)) < 0)
		sysfatal("sdprobe: cannot open %s size", node);
	if(read(fd, buf, sizeof buf - 1) <= 0)
		sysfatal("sdprobe: cannot read %s size", node);
	close(fd);
	buf[sizeof buf - 1] = 0;
	size = strtoull(buf, &p, 0);
	if(base != want || size == 0)
		sysfatal("sdprobe: unexpected %s %#llux+%#llux", node, base, size);
	len = (size + 0x1fffff) & ~0x1fffffULL;
	m = segattach(0, node, 0, len);
	if(m == (void *)-1)
		sysfatal("sdprobe: segattach %s: %r", node);
	return m + (base & 0x1fffff);
}

static int i2cctl = -1, i2cdat = -1;

static int
i2copen(void)
{
	char path[64], buf[1024], *p, *q;
	int fd, n, bus;

	if(access("/mnt/i2c", AEXIST) < 0)
		create("/mnt/i2c", OREAD, DMDIR|0755);
	if((fd = open("/srv/i2c", OREAD)) < 0)
		return -1;
	if(mount(fd, -1, "/mnt/i2c", MREPL|MCREATE, "") < 0){
		close(fd);
		return -1;
	}
	close(fd);
	bus = -1;
	if((fd = open("/mnt/i2c/status", OREAD)) >= 0){
		n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if(n > 0){
			buf[n] = 0;
			for(p = buf; (p = strstr(p, "base 0x")) != nil; p++){
				if(strtoul(p+7, nil, 16) != 0xfdd40000)
					continue;
				q = p;
				while(q > buf && q[-1] != '\n')
					q--;
				if(strncmp(q, "i2c", 3) == 0){
					bus = atoi(q+3);
					break;
				}
			}
		}
	}
	if(bus < 0)
		return -1;
	snprint(path, sizeof path, "/mnt/i2c/i2c%d/ctl", bus);
	i2cctl = open(path, ORDWR);
	snprint(path, sizeof path, "/mnt/i2c/i2c%d/data", bus);
	i2cdat = open(path, OREAD);
	return (i2cctl >= 0 && i2cdat >= 0) ? 0 : -1;
}

static int
pmicwrite(u8 reg, u8 val)
{
	char cmd[64];
	int k;

	k = snprint(cmd, sizeof cmd, "w %d %d %d", 0x20, reg, val);
	return write(i2cctl, cmd, strlen(cmd)) == strlen(cmd) ? 0 : -1;
}

/* RK817/RK809 CLK32KOUT2_EN is bit 7 of register 0x20. */
static void
lpo(void)
{
	if(i2copen() < 0){
		print("sdprobe: no i2c service, 32k not enabled\n");
		return;
	}
	if(pmicwrite(0x20, 0x80) < 0)
		print("sdprobe: RK809 CLK32KOUT write failed\n");
	else
		print("sdprobe: RK809 CLK32KOUT enabled\n");
}

static u32
gpioread(void)
{
	char buf[64];
	int fd, n;

	if((fd = open("/mnt/gpio/gpio0/data", OREAD)) < 0)
		return 0;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		return 0;
	buf[n] = 0;
	return strtoul(buf, nil, 16);
}

static void
gpiomount(void)
{
	int fd;

	if((fd = open("/srv/gpio", ORDWR)) < 0)
		sysfatal("sdprobe: /srv/gpio: %r");
	create("/mnt/gpio", OREAD, DMDIR|0755);
	if(mount(fd, -1, "/mnt/gpio", MREPL|MCREATE, "") < 0)
		sysfatal("sdprobe: mount gpio: %r");
	close(fd);
}

static void
gpioset(int pin, int val)
{
	char cmd[64];
	int fd;

	if((fd = open("/mnt/gpio/gpio0/ctl", OWRITE)) < 0)
		sysfatal("sdprobe: gpio ctl: %r");
	snprint(cmd, sizeof cmd, "%d out %d", pin, val);
	if(write(fd, cmd, strlen(cmd)) != strlen(cmd)){
		close(fd);
		sysfatal("sdprobe: gpio write: %r");
	}
	close(fd);
}

static void
setmux(int bank, int pin, int func)
{
	int reg, shift;
	u32 v;

	reg = RK_IOMUX_REG(bank, pin);
	shift = RK_IOMUX_SHIFT(pin);
	v = rd(grf, reg);
	v &= ~(0xfu << shift);
	v |= (u32)(func & 0xf) << shift;
	wr(grf, reg, v);
}

static void
pullup(int bank, int pin)
{
	int reg, bit;
	u32 v;

	reg = 0x80 + (bank-1)*0x10 + (pin/8)*4;
	bit = (pin % 8) * 2;
	v = rd(grf, reg);
	v &= ~(3u << bit);
	v |= 1u << bit;			/* 1 = pull-up */
	wr(grf, reg, v);
}

static void
pinmux(void)
{
	/* SDMMC1: D0-D3 = GPIO2_A3..A6, CMD = A7, CLK = B0 */
	setmux(2, 3, 1);
	setmux(2, 4, 1);
	setmux(2, 5, 1);
	setmux(2, 6, 1);
	setmux(2, 7, 1);
	setmux(2, 8, 1);
	pullup(2, 3);
	pullup(2, 4);
	pullup(2, 5);
	pullup(2, 6);
	pullup(2, 7);
	pullup(2, 8);
	print("sdprobe: pinmux gpio2 A3-A7,B0 -> sdmmc1, pull-ups on\n");
}

static void
clocks(void)
{
	/* clk_sdmmc1: gate off, mux xin24m, phase 0, gate on */
	wm(cru, 0x33c, 1<<3, 1<<3);		/* gate clk_sdmmc1 */
	wm(cru, 0x33c, 1<<2, 0);		/* enable hclk_sdmmc1 */
	wm(cru, 0x178, 7<<12, 5<<12);		/* CLKSEL_CON(30) mux */
	wr(cru, 0x588, 1);			/* drive phase 90 deg */
	wr(cru, 0x58c, 0);			/* sample phase 0 */
	wm(cru, 0x33c, 1<<3, 0);		/* ungate clk_sdmmc1 */
	sleep(10);
	print("sdprobe: clocks on (750 kHz parent, gates open)\n");
}

static void
softreset(void)
{
	/* SRST_SDMMC1 = 214: SOFTRST_CON(13) bit 6 */
	wm(cru, 0x434, 1<<6, 1<<6);
	sleep(10);
	wm(cru, 0x434, 1<<6, 0);
	sleep(10);
	print("sdprobe: CRU soft reset sdmmc1\n");
}

static int
waitclr(int off, u32 bit, int ms)
{
	u64 t0;

	t0 = nsec();
	while(rd(mmc, off) & bit){
		if(nsec() - t0 > (u64)ms * 1000000)
			return -1;
	}
	return 0;
}

/*
 * Tell the CIU to latch CLKENA/CLKDIV.  START is required: without it the
 * controller never updates the clock and no command ever runs (U-Boot's
 * dwmci_control_clken does the same).
 */
static void
clkupdate(void)
{
	u32 st = 0;
	int i;

	wr(mmc, Rintsts, 0xffffffff);
	wr(mmc, Cmdarg, 0);
	wr(mmc, Cmd, CmdStart | CmdPrvData | CmdUpdClk);
	for(i = 0; i < 200000; i++){
		st = rd(mmc, Rintsts);
		if(st & IntCmdDone)
			break;
	}
	wr(mmc, Rintsts, IntCmdDone);
	if(!(st & IntCmdDone))
		print("sdprobe: clock update incomplete (rintsts %08ux)\n", st);
}

static void
reset(void)
{
	wr(mmc, Pwren, 1);
	wr(mmc, Ctl, CtlReset);
	if(waitclr(Ctl, CtlReset, 100) < 0)
		sysfatal("sdprobe: controller reset timeout");

	/* stop the card clock, program it, start it again */
	wr(mmc, Clkena, 0);
	clkupdate();
	wr(mmc, Clkdiv, InitDiv);
	wr(mmc, Clksrc, 0);
	wr(mmc, Clkena, ClkenEnable | ClkenLowPwr);
	clkupdate();

	wr(mmc, Rintsts, 0xffffffff);
	wr(mmc, Intmask, 0);
	wr(mmc, Ctl, CtlIntEn);			/* global interrupt enable, polled */
	wr(mmc, Tmout, 0xffffffff);
	wr(mmc, Bmod, 1);
	wr(mmc, Fifoth, (2u<<28) | 0x7f | (0x80 << 16));
	wr(mmc, Ctype, 0);			/* 1 bit for init */
	wr(mmc, RstN, 0);			/* assert card reset */
	sleep(1);
	wr(mmc, RstN, 1);			/* release */
	sleep(1);

	print("sdprobe: controller reset, verid %08ux hcon %08ux clkdiv %ud\n",
	      rd(mmc, Verid), rd(mmc, Hcon), rd(mmc, Clkdiv));
	print("sdprobe: clksel30 %08ux ckgate15 %08ux pwren %08ux rstn %08ux clkena %08ux\n",
	      rd(cru, 0x178), rd(cru, 0x33c), rd(mmc, Pwren), rd(mmc, RstN),
	      rd(mmc, Clkena));
	print("sdprobe: phase drv %08ux sample %08ux\n",
	      rd(cru, 0x588), rd(cru, 0x58c));
}

static int
cmderr(u32 st)
{
	if(st & IntRespErr) return 1;
	if(st & IntRto) return 2;
	if(st & IntRcrc) return 3;
	if(st & IntHle) return 4;
	if(st & IntFrun) return 5;
	if(!(st & IntCmdDone)) return 6;
	return 0;
}

static int
cmd(int idx, u32 arg, int flags, u32 *resp)
{
	u32 st;
	long i;

	wr(mmc, Rintsts, 0xffffffff);
	wr(mmc, Cmdarg, arg);
	wr(mmc, Cmd, CmdStart | CmdUseHold | (idx & 0x3f) | (u32)flags);
	st = 0;
	for(i = 0; i < 2000000; i++){
		st = rd(mmc, Rintsts);
		if(st & IntCmdDone)
			break;
		if(st & (IntRespErr|IntRto|IntRcrc|IntHle|IntFrun))
			break;
	}
	lastst = st;
	wr(mmc, Rintsts, 0xffffffff);
	if(resp)
		*resp = rd(mmc, Resp0);
	return cmderr(st);
}

static int
cmdcheck(int idx, u32 arg, int flags, u32 *resp, char *what)
{
	int e;
	u32 r;

	e = cmd(idx, arg, flags, &r);
	if(e){
		print("sdprobe: %s (cmd%d) failed: %d, rintsts %08ux status %08ux\n",
		      what, idx, e, rd(mmc, Rintsts), rd(mmc, Status));
		return e;
	}
	if(resp)
		*resp = r;
	return 0;
}

/* Byte-mode CMD53 read into buf. */
static int
cmd53read(int func, u32 addr, void *buf, int n, int incr)
{
	u32 arg, st, v;
	u8 *p = buf, *q;
	u64 t0;
	int got, i, want;

	if(n <= 0 || n > 512)
		return -1;
	arg = ((u32)func << 28) | (incr ? 1u<<26 : 0) |
	      ((addr & 0x1ffff) << 9) | (u32)(n & 0x1ff);
	wr(mmc, Blksiz, n);
	wr(mmc, Bytcnt, n);
	wr(mmc, Rintsts, 0xffffffff);
	wr(mmc, Cmdarg, arg);
	wr(mmc, Cmd, CmdStart | CmdUseHold | 53 | CmdRespExp | CmdRespCrc |
	    CmdDataExp | CmdPrvData);
	got = 0;
	t0 = nsec();
	for(;;){
		st = rd(mmc, Status);
		want = (st >> 17) & 0x1fff;
		if(want > 0){
			for(i = 0; i < want && got < n; i++){
				v = rd(mmc, Fifo);
				q = (u8 *)&v;
				for(int k = 0; k < 4 && got < n; k++)
					p[got++] = q[k];
			}
			continue;
		}
		st = rd(mmc, Rintsts);
		if(st & (IntDrto|IntDcrc|IntFrun|IntHto|IntSbe|IntEbe)){
			wr(mmc, Rintsts, 0xffffffff);
			return -2;
		}
		if(st & IntDataOver)
			break;
		if(nsec() - t0 > 1000000000ULL){
			wr(mmc, Rintsts, 0xffffffff);
			return -3;
		}
	}
	wr(mmc, Rintsts, 0xffffffff);
	return got == n ? 0 : -4;
}

static int cmd52(int, int, u32, u32, u32*);

/* SSIDT DT-mode: set the 32-bit CP system address through FBR 0x15c..0x15f. */
static int
dtsetaddr(u32 addr)
{
	u32 r;
	int i;

	for(i = 0; i < 4; i++){
		if(cmd52(1, 0, 0x15c + i, (addr >> (8*i)) & 0xff, &r) < 0)
			return -1;
	}
	return 0;
}

typedef struct IdmacDesc IdmacDesc;
struct IdmacDesc {
	u32 des0, des1, des2, des3;
};

static u8 *dmava;
static u64 dmabase, dmasize, datapa, descpa;
static u8 *datava, *descva;

static u64
readnum(char *path)
{
	char buf[64];
	int fd, n;

	if((fd = open(path, OREAD)) < 0)
		return 0;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		return 0;
	buf[n] = 0;
	return strtoull(buf, nil, 0);
}

static u64
dmaalloc(u64 size, u64 align)
{
	char buf[64];
	int fd, n;

	if((fd = open("#P/dma/alloc", ORDWR)) < 0)
		return 0;
	snprint(buf, sizeof buf, "%llud %llud\n", size, align);
	if(write(fd, buf, strlen(buf)) != strlen(buf)){
		close(fd);
		return 0;
	}
	n = pread(fd, buf, sizeof buf - 1, 0);
	close(fd);
	if(n <= 0)
		return 0;
	buf[n] = 0;
	return strtoull(buf, nil, 0);
}

static int
dmainit(void)
{
	if(datava != nil)
		return 0;
	dmabase = readnum("#P/dma/base");
	dmasize = readnum("#P/dma/size");
	if(dmabase == 0 || dmasize == 0)
		return -1;
	dmava = segattach(0, "dma", 0, dmasize);
	if(dmava == (void*)-1){
		dmava = nil;
		return -1;
	}
	datapa = dmaalloc(4096, 4096);
	descpa = dmaalloc(4096, 4096);
	if(datapa == 0 || descpa == 0)
		return -1;
	datava = dmava + (datapa - dmabase);
	descva = dmava + (descpa - dmabase);
	memset(datava, 0, 4096);
	memset(descva, 0, 4096);
	print("sdprobe: idmac data %#llux desc %#llux\n", datapa, descpa);
	return 0;
}

static void
dmaclean(u64 pa, u64 len)
{
	char buf[64];
	int fd;

	if((fd = open("#P/dma/cleaninval", OWRITE)) < 0)
		return;
	snprint(buf, sizeof buf, "%#llux %#llux\n", pa, len);
	write(fd, buf, strlen(buf));
	close(fd);
}

static int
idmac_write(int func, u32 addr, u8 *buf, int len)
{
	IdmacDesc *d;
	u32 arg, st, ids;
	u64 t0;

	if(len <= 0 || len > 4096 || (len % 512) != 0)
		return -1;
	if(dmainit() < 0)
		return -1;
	wr(mmc, Ctl, CtlIntEn|CtlDmaReset);
	for(int w = 0; w < 1000 && (rd(mmc, Ctl) & CtlDmaReset); w++)
		;
	wr(mmc, Ctl, CtlIntEn);
	memmove(datava, buf, len);
	dmaclean(datapa, len);

	d = (IdmacDesc*)descva;
	memset(d, 0, 32);
	d[0].des0 = 0x80000000u | (1u<<3) | (1u<<2);	/* OWN|FD|LD */
	d[0].des1 = len & 0x1fff;
	d[0].des2 = datapa;
	d[0].des3 = descpa + sizeof(IdmacDesc);
	d[1].des0 = 1u<<5;				/* ER */
	d[1].des3 = descpa;
	dmaclean(descpa, 32);

	wr(mmc, Bmod, BmodSwr);
	for(int w = 0; w < 1000 && (rd(mmc, Bmod) & BmodSwr); w++)
		;
	wr(mmc, Idinten, IdmacTi|IdmacRi|IdmacNi);
	wr(mmc, Idsts, 0xffffffff);
	wr(mmc, Dbaddr, descpa);
	wr(mmc, Bmod, BmodIdmac|BmodFb);
	wr(mmc, Ctl, CtlIntEn|CtlDmaEn|CtlUseIdmac);
	wr(mmc, Rintsts, 0xffffffff);
	wr(mmc, Blksiz, 512);
	wr(mmc, Bytcnt, len);
	arg = (1u<<31) | ((u32)func << 28) | (1u<<27) | (1u<<26) |
	      ((addr & 0x1ffff) << 9) | (u32)(len/512);
	wr(mmc, Cmdarg, arg);
	wr(mmc, Cmd, CmdStart | CmdUseHold | 53 | CmdRespExp | CmdRespCrc |
	    CmdDataExp | CmdDatWr);

	t0 = nsec();
	for(;;){
		st = rd(mmc, Rintsts);
		ids = rd(mmc, Idsts);
		if(ids & IdmacTi)
			break;
		if(st & (IntDrto|IntDcrc|IntFrun|IntHto|IntSbe|IntEbe) ||
		   (ids & IdmacRi)){
			print("sdprobe: idmac err rintsts %08ux idsts %08ux bmod %08ux ctl %08ux dbaddr %08ux\n",
			      st, ids, rd(mmc, Bmod), rd(mmc, Ctl), rd(mmc, Dbaddr));
			wr(mmc, Ctl, CtlIntEn);
			wr(mmc, Idsts, 0xffffffff);
			wr(mmc, Rintsts, 0xffffffff);
			return -2;
		}
		if(nsec() - t0 > 3000000000ULL){
			wr(mmc, Ctl, CtlIntEn);
			return -3;
		}
	}
	wr(mmc, Idsts, 0xffffffff);
	wr(mmc, Rintsts, 0xffffffff);
	wr(mmc, Ctl, CtlIntEn);
	return 0;
}

/* CMD53 write, block mode when n == 512 and block != 0, else byte mode. */
static int
cmd53write(int func, u32 addr, void *buf, int n, int block)
{
	u32 arg, st, v;
	u8 *p = buf;
	u64 t0;
	int i, words, used;

	if(n <= 0 || n > 4096)
		return -1;
	if(block && addr == 0x20)
		return idmac_write(func, addr, buf, n);
	if(block){
		if(n % 512 != 0)
			return -1;
		arg = (1u<<31) | ((u32)func << 28) | (1u<<27) | (1u<<26) |
		      ((addr & 0x1ffff) << 9) | (u32)(n/512);
		wr(mmc, Blksiz, 512);
	}else{
		arg = (1u<<31) | ((u32)func << 28) | (1u<<26) |
		      ((addr & 0x1ffff) << 9) | (u32)(n & 0x1ff);
		wr(mmc, Blksiz, n);
	}
	wr(mmc, Bytcnt, n);
	wr(mmc, Rintsts, 0xffffffff);
	wr(mmc, Cmdarg, arg);
	wr(mmc, Cmd, CmdStart | CmdUseHold | 53 | CmdRespExp | CmdRespCrc |
	    CmdDataExp | CmdDatWr);

	words = (n + 3) / 4;
	t0 = nsec();
	for(i = 0; i < words; i++){
		for(;;){
			st = rd(mmc, Rintsts);
			if(st & (IntDrto|IntDcrc|IntFrun|IntHto|IntSbe|IntEbe))
				return -2;
			used = (rd(mmc, Status) >> 17) & 0x1fff;
			if(used < 128)
				break;
			if(nsec() - t0 > 2000000000ULL){
				wr(mmc, Rintsts, 0xffffffff);
				return -3;
			}
		}
		if(i*4 + 4 <= n)
			memmove(&v, p + i*4, 4);
		else{
			v = 0;
			memmove(&v, p + i*4, n - i*4);
		}
		wr(mmc, Fifo, v);
	}

	t0 = nsec();
	for(;;){
		st = rd(mmc, Rintsts);
		if(st & (IntDrto|IntDcrc|IntFrun|IntHto|IntSbe|IntEbe)){
			wr(mmc, Rintsts, 0xffffffff);
			return -2;
		}
		if(st & IntDataOver)
			break;
		if(nsec() - t0 > 2000000000ULL){
			wr(mmc, Rintsts, 0xffffffff);
			return -3;
		}
	}
	wr(mmc, Rintsts, 0xffffffff);
	return 0;
}

/* DT-mode write: set the system address, then stream through 0x0f. */
static int
dtwrite(u32 addr, void *buf, int len)
{
	u8 *p = buf;
	int n, e;

	if(dtsetaddr(addr) < 0)
		return -1;
	while(len > 0){
		n = len > 512 ? 512 : len;
		e = cmd53write(1, 0x0f, p, n, n == 512);
		if(e)
			return e;
		p += n;
		len -= n;
	}
	return 0;
}

/* CMD53 block-mode read from function `func`, address `addr`. */
static int
cmd53blockread(int func, u32 addr, void *buf, int nblk)
{
	u32 arg, st, v;
	u8 *p = buf, *q;
	u64 t0;
	int got, i, n, want;

	n = nblk * 512;
	if(n <= 0 || n > 4096)
		return -1;
	arg = ((u32)func << 28) | (1u<<27) | (1u<<26) |
	      ((addr & 0x1ffff) << 9) | (u32)(nblk & 0x1ff);
	wr(mmc, Blksiz, 512);
	wr(mmc, Bytcnt, n);
	wr(mmc, Rintsts, 0xffffffff);
	wr(mmc, Cmdarg, arg);
	wr(mmc, Cmd, CmdStart | CmdUseHold | 53 | CmdRespExp | CmdRespCrc |
	    CmdDataExp | CmdPrvData);
	got = 0;
	t0 = nsec();
	for(;;){
		st = rd(mmc, Status);
		want = (st >> 17) & 0x1fff;
		if(want > 0){
			for(i = 0; i < want && got < n; i++){
				v = rd(mmc, Fifo);
				q = (u8 *)&v;
				for(int k = 0; k < 4 && got < n; k++)
					p[got++] = q[k];
			}
			continue;
		}
		st = rd(mmc, Rintsts);
		if(st & (IntDrto|IntDcrc|IntFrun|IntHto|IntSbe|IntEbe)){
			wr(mmc, Rintsts, 0xffffffff);
			return -2;
		}
		if(st & IntDataOver)
			break;
		if(nsec() - t0 > 2000000000ULL){
			wr(mmc, Rintsts, 0xffffffff);
			return -3;
		}
	}
	wr(mmc, Rintsts, 0xffffffff);
	return got == n ? 0 : -4;
}

static int
dtread(u32 addr, void *buf, int len)
{
	u8 *p = buf;
	int n, e;

	if(dtsetaddr(addr) < 0)
		return -1;
	while(len > 0){
		n = len > 512 ? 512 : len;
		if(n == 512)
			e = cmd53blockread(1, 0x0f, p, 1);
		else
			e = cmd53read(1, 0x0f, p, n, 1);
		if(e)
			return e;
		p += n;
		len -= n;
	}
	return 0;
}

static u32
puh(u32 len, u32 type, u32 subtype)
{
	return (len << 7) | (subtype << 24) | (type << 28);
}

static void
putle16(u8 *p, u32 v)
{
	p[0] = v;
	p[1] = v >> 8;
}

static void
putle32(u8 *p, u32 v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static u32
getle16(u8 *p)
{
	return p[0] | (p[1] << 8);
}

static u32
getle32(u8 *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static u16
crc16(u8 *buf, int len)
{
	static const u16 tab[] = {
		0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00,
		0x2800, 0xE401, 0xA001, 0x6C00, 0x7800, 0xB401,
		0x5000, 0x9C01, 0x8801, 0x4400,
	};
	u16 crc = 0xFFFF;
	u8 c;
	int i;

	for(i = 0; i < len; i++){
		c = *buf++;
		crc = tab[(c ^ crc) & 15] ^ (crc >> 4);
		crc = tab[((c >> 4) ^ crc) & 15] ^ (crc >> 4);
	}
	return crc;
}

/* Abort the current SDIO data transfer (CCCR ABORT, function 0). */
static void
sdioabort(void)
{
	u32 r;

	cmd52(1, 0, 0x06, 0x01, &r);
}

/* Write a PK packet the way the vendor's sdio_writesb does: one block-mode
   CMD53 for the full 512-byte chunks, then a byte-mode CMD53 for the rest. */
static int
pkwrite(u8 *buf, int len)
{
	int e, nb, rem;

	if(len > 512){
		nb = len / 512;
		rem = len - nb*512;
		e = cmd53write(1, 0x20, buf, nb*512, 1);
		if(e){
			sdioabort();
			return e;
		}
		if(rem){
			e = cmd53write(1, 0x20, buf + nb*512, rem, 0);
			if(e)
				sdioabort();
			return e;
		}
		return 0;
	}
	e = cmd53write(1, 0x20, buf, len, 0);
	if(e)
		sdioabort();
	return e;
}

/* Defined with the data-path code below; declared here for the RX parser. */
static u8 mymac[6];
static u8 bssid[6];
static int lutidx;
static int connected;
static void framin(u8*, int);
static int subcount[16];
static int flow[4];		/* TX credits: VO, VI, BE, BK */
static int txfail;

typedef struct Ap Ap;
struct Ap {
	u8 bssid[6];
	u8 ch;
	char ssid[33];
};
enum { MaxAp = 32 };
static Ap aps[MaxAp];
static int naps;

/* Decode a WIFI_EVENT_MGMT_FRAME scan result: channel, signal, BSSID, SSID. */
static void
scanframe(u8 *p, int len)
{
	char ssid[34];
	int i, off, id, elen, flen, ch, sig, hdr;
	u8 *f;

	if(len < 12)
		return;
	/* Payload is either {type,ch,sig,rsvd,bssid[6],len} + frame, or a
	   bare 802.11 frame starting at p+12. */
	if(p[12] >= 1 && p[12] <= 5){
		hdr = 12;
		ch = p[13];
		sig = (signed char)p[14];
		flen = getle16(p + 22);
	}else{
		hdr = 0;
		ch = 0;
		sig = 0;
		flen = len - 12;
	}
	f = p + 12 + hdr;
	if(flen > len - 12 - hdr)
		flen = len - 12 - hdr;
	if(hdr == 0 && len - 12 - flen >= 12){
		u8 *t = f + flen;

		if(t[0] >= 1 && t[0] <= 5 && t[1] >= 1 && t[1] <= 14){
			ch = t[1];
			sig = (signed char)t[2];
		}
	}
	if(flen < 36)
		return;
	ssid[0] = 0;
	off = 36;			/* 24-byte header + 12 fixed fields */
	while(off + 2 <= flen){
		id = f[off];
		elen = f[off + 1];
		if(off + 2 + elen > flen)
			break;
		if(id == 0){
			if(elen > 32)
				elen = 32;
			for(i = 0; i < elen; i++)
				ssid[i] = f[off + 2 + i] >= 32 && f[off + 2 + i] < 127 ?
					  f[off + 2 + i] : '.';
			ssid[elen] = 0;
		}
		off += 2 + elen;
	}
	print("sdprobe: scan ap ch %d sig %d bssid %02x:%02x:%02x:%02x:%02x:%02x ssid [%s]\n",
	      ch, sig, f[10], f[11], f[12], f[13], f[14], f[15], ssid);
	if(naps < MaxAp && ssid[0] != 0){
		Ap *a = &aps[naps++];

		memmove(a->bssid, f + 10, 6);
		a->ch = ch;
		for(i = 0; i < (int)sizeof a->ssid - 1 && ssid[i] != 0; i++)
			a->ssid[i] = ssid[i];
		a->ssid[i] = 0;
	}
}

/* Print one packet from an RX aggregation window; return its subtype or -1. */
static int
rxdump(u8 *rx, int valid)
{
	char line[132];
	int off, h, len, type, sub, eof, ck, i, j, k, last;
	static int nlogshown, nraw, ndata;
	u8 *p;

	if(valid <= 0 || valid > 2040)
		return -1;
	last = -1;
	for(off = 0; off + 4 <= valid; ){
		h = getle32(rx + off);
		len = (h >> 7) & 0xffff;
		eof = (h >> 23) & 1;
		ck = (h >> 6) & 1;
		sub = (h >> 24) & 0xf;
		type = (h >> 28) & 0xf;
		p = rx + off + 4;
		print("sdprobe: rx off %d type %d sub %d len %d eof %d ck %d\n",
		      off, type, sub, len, eof, ck);
		if(eof || len <= 0)
			break;
		last = sub;
		if(sub >= 0 && sub < 16)
			subcount[sub]++;
		if(sub == 10 && len >= 12){
			print("sdprobe: PK reply id %d plen %d status %d rspcnt %d\n",
			      p[1], getle16(p + 2), (signed char)p[8], p[9]);
			if(p[1] == 131 && nraw++ < 1){
				for(i = 0; i < 160 && i < len; i += 32){
					print("sdprobe: raw");
					for(k = 0; k < 32 && i + k < len; k++)
						print(" %02x", p[i + k]);
					print("\n");
				}
			}
			last = 0x4000 | sub;
			if(p[1] == 131)
				scanframe(p + 12, len - 12);
			else if(p[1] == 130)
				print("sdprobe: scan done code %d\n", p[12]);
			for(i = 0; i < 64 && i < len; i++)
				print("%02x", p[i]);
			print("\n");
		}
		if(sub == 12){
			if(++ndata <= 4)
				print("sdprobe: data pkt len %d b0 %02x b1 %02x b2 %02x b3 %02x\n",
				      len, len > 0 ? p[0] : 0, len > 1 ? p[1] : 0,
				      len > 2 ? p[2] : 0, len > 3 ? p[3] : 0);
		}
		if(sub == 12 && len > 4){
			int eoff = p[1];
			int elen = p[2] | (p[3] << 8);

			if(eoff <= 0 || eoff + 14 > len)
				eoff = 28;
			if(eoff + elen > len)
				elen = len - eoff;
			if(elen >= 14 && (nraw++ < 6))
				print("sdprobe: eth %02x:%02x:%02x:%02x:%02x:%02x <- %02x:%02x:%02x:%02x:%02x:%02x type %04x len %d\n",
				      p[eoff+6], p[eoff+7], p[eoff+8], p[eoff+9], p[eoff+10], p[eoff+11],
				      p[eoff], p[eoff+1], p[eoff+2], p[eoff+3], p[eoff+4], p[eoff+5],
				      (p[eoff+12] << 8) | p[eoff+13], elen);
			if(elen >= 14)
				framin(p + eoff, elen);
		}
		if(sub == 10 && p[1] == 128){
			print("sdprobe: connect event status %d code %d bssid %02x:%02x:%02x:%02x:%02x:%02x ch %d sig %d\n",
			      p[12], p[14], p[13], p[14], p[15], p[16], p[17], p[18],
			      p[19], (signed char)p[20]);
			if(p[12] == 0){
				memmove(bssid, p + 13, 6);
				connected = 1;
			}
		}
		if(sub == 10 && p[1] == 0xb3 && len >= 16){
			flow[0] = p[12]; flow[1] = p[13];
			flow[2] = p[14]; flow[3] = p[15];
			print("sdprobe: flow vo %d vi %d be %d bk %d\n",
			      flow[0], flow[1], flow[2], flow[3]);
		}
		if(sub == 10 && p[1] == 0xf5){
			lutidx = p[14];
			print("sdprobe: sta lut index %d ctx %d action %d\n",
			      lutidx, p[12], p[13]);
		}
		if(sub == 3){
			int show = 0;

			for(i = 0; i + 7 < len; i++)
				if(memcmp(p + i, "TXC_FAIL", 8) == 0 ||
				   memcmp(p + i, "tx fail", 7) == 0 ||
				   memcmp(p + i, "no rate", 7) == 0){
					show = 1;
					txfail++;
				}
			if(show){
				for(i = 0; i < len && i < 2048; i += 128){
					j = len - i < 128 ? len - i : 128;
					for(k = 0; k < j; k++){
						int c = p[i + k];
						line[k] = c >= 32 && c < 127 ? c : '.';
					}
					line[j] = 0;
					print("sdprobe: log %s\n", line);
				}
			}
		}
		off += 4 + ((len + (ck ? 2 : 0) + 3) & ~3);
	}
	return last;
}

/* Read one RX window and return the valid byte count in it, or -1. */
static int
rxwindow(u8 *rx)
{
	int e, valid;

	memset(rx, 0, 2048);
	e = cmd53blockread(1, 0x20, rx, 4);
	if(e){
		sdioabort();
		return e;
	}
	valid = getle32(rx + 2040);
	if(valid > 2040)
		valid = 0;
	return valid;
}

static u32
cpaon(u32 reg)
{
	u32 v;

	if(cmd52(0, 0, reg, 0, &v) < 0)
		return 0xffffffff;
	return v & 0xff;
}

static void
cpstate(char *tag)
{
	print("sdprobe: %s slp %02ux slpctl %02ux pubint %02ux intx %02ux\n",
	      tag, cpaon(0x148), cpaon(0x1a2), cpaon(0x1f0), cpaon(0x05));
}

/* Poll the SDIO card interrupt pending bit for function 1. */
static int
waitint(int ms)
{
	u32 v;
	int i;
	u64 t0;

	t0 = nsec();
	for(i = 0; nsec() - t0 < (u64)ms*1000000ULL; i++){
		if(cmd52(0, 0, 0x05, 0, &v) == 0 && (v & 0x02))
			return 1;
		if((i % 20) == 0)
			cpstate("poll");
		sleep(5);
	}
	return 0;
}

/* Wait for a command reply on RX subtype 10. */
static int
waitreply(int cmdid, int ms)
{
	u8 *rx;
	int valid, sub, seen;
	u64 t0;

	rx = malloc(2048);
	if(rx == nil)
		return 0;
	seen = 0;
	if(!waitint(ms)){
		print("sdprobe: no card interrupt\n");
		free(rx);
		return 0;
	}
	t0 = nsec();
	for(; nsec() - t0 < (u64)ms*1000000ULL; ){
		valid = rxwindow(rx);
		if(valid <= 0)
			break;
		sub = rxdump(rx, valid);
		if(sub == 10)
			seen = 1;
		if(!waitint(200))
			break;
	}
	cpstate("after rx");
	free(rx);
	return seen;
}

static void
rxdrain(int n, int ms)
{
	u8 *rx;
	int i, valid;

	rx = malloc(2048);
	if(rx == nil)
		return;
	for(i = 0; i < n; i++){
		valid = rxwindow(rx);
		if(valid > 0)
			rxdump(rx, valid);
		sleep(ms);
	}
	free(rx);
	print("sdprobe: rxdrain done\n");
}

static void
drainrx(void)
{
	u8 *rx;
	int valid;
	u32 v;

	if(cmd52(0, 0, 0x05, 0, &v) == 0)
		print("sdprobe: drainrx intx %02ux\n", v & 0xff);
	rx = malloc(2048);
	if(rx == nil)
		return;
	valid = rxwindow(rx);
	if(valid > 0)
		rxdump(rx, valid);
	else
		print("sdprobe: drainrx no data\n");
	free(rx);
}

static int pksend(int, u8*, int, int);

static int
send_ini(u8 *data, int len, int sec)
{
	u8 *buf;
	u16 crc;
	int e, got;
	u32 r;

	buf = malloc(4 + len + 2);
	if(buf == nil)
		return -1;
	memset(buf, 0, 4);
	buf[0] = sec;
	memmove(buf + 4, data, len);
	crc = crc16(data, len);
	buf[4 + len] = crc;
	buf[5 + len] = crc >> 8;
	print("sdprobe: INI section %d len %d crc %04ux\n", sec, len, crc);
	if(cmd52(1, 0, 0x1a2, 0, &r) == 0)
		sleep(10);
	got = pksend(76, buf, len + 6, 3000);
	free(buf);
	if(got & 0x4000){
		print("sdprobe: INI section %d acked\n", sec);
		return 0;
	}
	print("sdprobe: INI section %d no reply\n", sec);
	return -1;
}

/* Send one WLAN command packet and wait for a reply. */
static int
pksend(int cmdid, u8 *data, int len, int waitms)
{
	u8 *tx;
	int plen, total, n, e, i, valid, got;
	u8 *rx;

	plen = 12 + len;
	total = 4 + ((plen + 3) & ~3) + 4;
	n = ((total + 511) / 512) * 512;
	tx = malloc(n);
	if(tx == nil)
		return -1;
	memset(tx, 0, n);
	putle32(tx, puh(plen, 0, 8));
	tx[4] = 0x10;			/* cmd, rsp=1 */
	tx[5] = cmdid;
	putle16(tx + 6, plen);
	if(len > 0)
		memmove(tx + 16, data, len);
	putle32(tx + 4 + ((plen + 3) & ~3), 0x00800000);
	print("sdprobe: cmd %d len %d plen %d blocks %d\n",
	      cmdid, len, plen, n/512);
	e = pkwrite(tx, n);
	free(tx);
	if(e){
		print("sdprobe: cmd %d write e=%d\n", cmdid, e);
		return -1;
	}
	rx = malloc(2048);
	if(rx == nil)
		return -1;
	got = 0;
	for(i = 0; i < waitms/50 + 1; i++){
		sleep(50);
		valid = rxwindow(rx);
		if(valid > 0){
			got |= rxdump(rx, valid);
			if(got & 0x4000)
				break;
		}
	}
	free(rx);
	return got;
}

/* Drain RX windows for a while, printing every packet. */
static void
pklisten(int ms)
{
	u8 *rx;
	int valid, n;
	u64 t0;

	rx = malloc(2048);
	if(rx == nil)
		return;
	n = 0;
	t0 = nsec();
	while(nsec() - t0 < (u64)ms*1000000ULL){
		valid = rxwindow(rx);
		if(valid > 0){
			rxdump(rx, valid);
			n++;
		}else if(valid < 0)
			print("sdprobe: rx window error %d\n", valid);
		sleep(20);
	}
	free(rx);
	print("sdprobe: listen done, %d windows\n", n);
}

/* ---- WLAN data path: 802.3 frames in tx_msdu_dscr/rx_msdu_desc ---- */

static u8 mymac[6] = { 0x02, 0, 0, 0, 0, 0x01 };
static u8 bssid[6];
static int lutidx = 0;
static int connected;

static u8 dh_yi[4], dh_sid[4], dh_mask[4], dh_gw[4], dh_dns[4];
static u32 dh_xid;
static int dh_msgtype;

static void
putbe16(u8 *p, u32 v)
{
	p[0] = v >> 8;
	p[1] = v;
}

static void
putbe32(u8 *p, u32 v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static u32
getbe32(u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static u16
ipcsum(u8 *p, int n)
{
	u32 s = 0;
	int i;

	for(i = 0; i + 1 < n; i += 2)
		s += (u32)p[i] << 8 | p[i+1];
	if(i < n)
		s += (u32)p[i] << 8;
	while(s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return ~s & 0xffff;
}

/* Wait until the CP has granted a BE TX credit, draining RX meanwhile. */
static int
waitflow(int ac, int ms)
{
	u8 *rx;
	int valid;
	u64 t0;

	if(flow[ac] > 0)
		return 1;
	rx = malloc(2048);
	if(rx == nil)
		return 0;
	t0 = nsec();
	while(nsec() - t0 < (u64)ms*1000000ULL){
		if(flow[ac] > 0)
			break;
		valid = rxwindow(rx);
		if(valid > 0)
			rxdump(rx, valid);
		sleep(10);
	}
	free(rx);
	return flow[ac] > 0;
}

/* Send one 802.3 frame on the WLAN data channel. */
static int
datatx(u8 *frame, int len)
{
	u8 *buf;
	int pktlen, n, e;

	if(!waitflow(2, 2000)){
		print("sdprobe: datatx no tx credit\n");
		return -1;
	}
	pktlen = 11 + len;
	n = ((4 + pktlen + 4 + 511) / 512) * 512;
	buf = malloc(n);
	if(buf == nil)
		return -1;
	memset(buf, 0, n);
	putle32(buf, puh(pktlen, 0, 10));	/* TX data channel 10 */
	buf[4] = 2;				/* SPRDWL_TYPE_DATA, ctx 0 */
	buf[5] = 11;				/* dscr offset */
	buf[6] = 0;				/* tx_ctrl */
	putle16(buf + 7, len);			/* pkt_len */
	buf[9] = 0;				/* buffer_info */
	buf[10] = lutidx;			/* sta_lut_index */
	memmove(buf + 15, frame, len);
	putle32(buf + 4 + ((pktlen + 3) & ~3), 0x00800000);
	e = pkwrite(buf, n);
	if(e == 0 && flow[2] > 0)
		flow[2]--;
	print("sdprobe: datatx %d bytes e=%d lut %d be %d\n", len, e, lutidx, flow[2]);
	free(buf);
	return e;
}

/* Build a DHCP message into an Ethernet frame; returns the frame length. */
static int
dhbuild(u8 *f, int type)
{
	u8 *ip, *udp, *dh, *o;
	int tot, ulen;

	memset(f, 0, 600);
	memset(f, 0xff, 6);
	memmove(f + 6, mymac, 6);
	putbe16(f + 12, 0x0800);
	ip = f + 14;
	ip[0] = 0x45;
	ip[8] = 64;
	ip[9] = 17;
	memset(ip + 12, 0, 4);
	memset(ip + 16, 0xff, 4);
	udp = ip + 20;
	putbe16(udp, 68);
	putbe16(udp + 2, 67);
	dh = udp + 8;
	dh[0] = 1;			/* BOOTREQUEST */
	dh[1] = 1;			/* Ethernet */
	dh[2] = 6;
	putbe32(dh + 4, dh_xid);
	putbe16(dh + 10, 0x8000);	/* broadcast */
	memmove(dh + 28, mymac, 6);
	dh[236] = 63; dh[237] = 82; dh[238] = 83; dh[239] = 99;
	o = dh + 240;
	*o++ = 53; *o++ = 1; *o++ = type;
	if(type == 3){			/* DHCPREQUEST */
		*o++ = 50; *o++ = 4;
		memmove(o, dh_yi, 4); o += 4;
		*o++ = 54; *o++ = 4;
		memmove(o, dh_sid, 4); o += 4;
	}
	*o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;
	*o++ = 255;
	ulen = 8 + (o - dh);
	putbe16(udp + 4, ulen);
	tot = 20 + ulen;
	putbe16(ip + 2, tot);
	putbe16(ip + 10, ipcsum(ip, 20));
	return 14 + tot;
}

/* Handle one received 802.3 frame (minimal DHCP client). */
static void
framin(u8 *f, int len)
{
	u8 *ip, *udp, *dh, *o;
	int tot, ulen, hl;
	u32 xid;

	if(len < 42 || getle16(f + 12) != 0x0008)
		return;
	ip = f + 14;
	if((ip[0] >> 4) != 4 || ip[9] != 17)
		return;
	hl = (ip[0] & 0xf) * 4;
	udp = ip + hl;
	tot = (ip[2] << 8) | ip[3];
	ulen = (udp[4] << 8) | udp[5];
	if(tot < hl + 8 + 240 || ulen < 248)
		return;
	dh = udp + 8;
	if(dh[0] != 2)
		return;
	xid = getbe32(dh + 4);
	if(xid != dh_xid)
		return;
	o = dh + 240;
	while(o + 2 <= dh + (ulen - 8) && *o != 255){
		int t = o[0], l = o[1];

		if(t == 53 && l == 1)
			dh_msgtype = o[2];
		else if(t == 54 && l == 4)
			memmove(dh_sid, o + 2, 4);
		else if(t == 1 && l == 4)
			memmove(dh_mask, o + 2, 4);
		else if(t == 3 && l >= 4)
			memmove(dh_gw, o + 2, 4);
		else if(t == 6 && l >= 4)
			memmove(dh_dns, o + 2, 4);
		o += 2 + l;
	}
	memmove(dh_yi, dh + 16, 4);
	print("sdprobe: dhcp type %d yi %d.%d.%d.%d mask %d.%d.%d.%d gw %d.%d.%d.%d\n",
	      dh_msgtype, dh_yi[0], dh_yi[1], dh_yi[2], dh_yi[3],
	      dh_mask[0], dh_mask[1], dh_mask[2], dh_mask[3],
	      dh_gw[0], dh_gw[1], dh_gw[2], dh_gw[3]);
}

/* Wait for a DHCP message of the given type. */
static int
dhwait(int want, int ms)
{
	u8 *rx;
	int valid, sub;
	u64 t0;

	rx = malloc(2048);
	if(rx == nil)
		return 0;
	t0 = nsec();
	while(nsec() - t0 < (u64)ms*1000000ULL){
		valid = rxwindow(rx);
		if(valid > 0){
			sub = rxdump(rx, valid);
			if((sub & 0xf) == 12 && dh_msgtype == want){
				free(rx);
				return 1;
			}
		}
		sleep(10);
	}
	free(rx);
	return 0;
}

/* Send an ARP request for the given (big-endian) IPv4 address. */
static void
arpwho(u32 ip, int unidst)
{
	u8 f[60];

	memset(f, 0, sizeof f);
	if(unidst)
		memmove(f, bssid, 6);
	else
		memset(f, 0xff, 6);
	memmove(f + 6, mymac, 6);
	putbe16(f + 12, 0x0806);
	putbe16(f + 14, 1);		/* Ethernet */
	putbe16(f + 16, 0x0800);
	f[18] = 6; f[19] = 4;
	putbe16(f + 20, 1);		/* request */
	memmove(f + 22, mymac, 6);
	memmove(f + 28, dh_yi, 4);	/* sender IP (may be 0) */
	memset(f + 32, 0, 6);
	putbe32(f + 38, ip);
	print("sdprobe: arp who-has %d.%d.%d.%d\n",
	      (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
	datatx(f, sizeof f);
}

static int
dhcp(void)
{
	u8 f[600];
	int n;

	dh_xid = 0x5a5a0001;
	dh_msgtype = 0;
	n = dhbuild(f, 1);
	for(int try = 0; try < 3; try++){
		print("sdprobe: dhcp discover %d bytes try %d\n", n, try);
		if(datatx(f, n) < 0)
			return -1;
		if(dhwait(2, 4000))
			break;
		print("sdprobe: no dhcp offer yet\n");
		if(try == 2){
			arpwho(0xc0a80101, 0);	/* 192.168.1.1, broadcast */
			dhwait(99, 3000);
			print("sdprobe: txfail %d after bcast\n", txfail);
			arpwho(0xc0a80101, 1);	/* unicast to the BSSID */
			dhwait(99, 3000);
			print("sdprobe: subs 3=%d 10=%d 12=%d txfail %d\n",
			      subcount[3], subcount[10], subcount[12], txfail);
			return -1;
		}
	}
	n = dhbuild(f, 3);
	print("sdprobe: dhcp request %d bytes\n", n);
	if(datatx(f, n) < 0)
		return -1;
	if(!dhwait(5, 4000)){
		print("sdprobe: no dhcp ack\n");
		return -1;
	}
	print("sdprobe: subs 3=%d 10=%d 12=%d\n",
	      subcount[3], subcount[10], subcount[12]);
	print("sdprobe: dhcp bound\n");
	return 0;
}

static int downloadfirmware(void);

/* Download firmware, send the INI sections and open station mode. */
static int
bringup(void)
{
	u8 open[8] = { 1, 0, 2, 0, 0, 0, 0, 1 };

	if(downloadfirmware() < 0)
		return -1;
	if(send_ini((u8*)wifi_sec1, sizeof(wifi_sec1), 1) < 0)
		return -1;
	if(send_ini((u8*)wifi_sec2, sizeof(wifi_sec2), 2) < 0)
		return -1;
	if(send_ini((u8*)wifi_sec3, sizeof(wifi_sec3), 3) < 0)
		return -1;
	sleep(100);
	cpstate("post ini");
	print("sdprobe: WIFI_CMD_OPEN\n");
	pksend(3, open, sizeof open, 3000);
	sleep(100);
	cpstate("post open");
	return 0;
}

/* Scan for APs and record them in the ap table. */
static void
doscan(int rounds)
{
	u8 scan[12];

	memset(scan, 0, sizeof scan);
	putle32(scan, 0x3fff);		/* channels 1..14 */
	for(int r = 0; r < rounds; r++){
		print("sdprobe: WIFI_CMD_SCAN round %d\n", r);
		pksend(11, scan, sizeof scan, 5000);
		pklisten(6000);
		sleep(500);
	}
}

/* Minimal SHA1/HMAC/PBKDF2, enough to derive a WPA2 PSK from a passphrase. */
typedef struct Sha1 Sha1;
struct Sha1 {
	u32 h[5];
	u64 n;
	u8 buf[64];
};

static u32
rol32(u32 x, int n)
{
	return (x << n) | (x >> (32 - n));
}

static void
sha1block(Sha1 *s, u8 *p)
{
	u32 w[80], a, b, c, d, e, t;
	int i;

	for(i = 0; i < 16; i++)
		w[i] = (u32)p[4*i] << 24 | (u32)p[4*i+1] << 16 |
		       (u32)p[4*i+2] << 8 | p[4*i+3];
	for(i = 16; i < 80; i++)
		w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
	for(i = 0; i < 80; i++){
		if(i < 20)
			t = 0x5a827999 + ((b & c) | (~b & d));
		else if(i < 40)
			t = 0x6ed9eba1 + (b ^ c ^ d);
		else if(i < 60)
			t = 0x8f1bbcdc + ((b & c) | (b & d) | (c & d));
		else
			t = 0xca62c1d6 + (b ^ c ^ d);
		t += rol32(a, 5) + e + w[i];
		e = d; d = c; c = rol32(b, 30); b = a; a = t;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void
sha1init(Sha1 *s)
{
	s->h[0] = 0x67452301; s->h[1] = 0xefcdab89;
	s->h[2] = 0x98badcfe; s->h[3] = 0x10325476; s->h[4] = 0xc3d2e1f0;
	s->n = 0;
}

static void
sha1write(Sha1 *s, u8 *p, int len)
{
	int i;

	for(i = 0; i < len; i++){
		s->buf[s->n % 64] = p[i];
		s->n++;
		if((s->n % 64) == 0)
			sha1block(s, s->buf);
	}
}

static void
sha1sum(Sha1 *s, u8 out[20])
{
	u64 bits = s->n * 8;
	u8 pad = 0x80, zero = 0, len[8];
	int i;

	sha1write(s, &pad, 1);
	while((s->n % 64) != 56)
		sha1write(s, &zero, 1);
	for(i = 0; i < 8; i++)
		len[i] = bits >> (56 - 8*i);
	sha1write(s, len, 8);
	for(i = 0; i < 5; i++){
		out[4*i] = s->h[i] >> 24;
		out[4*i+1] = s->h[i] >> 16;
		out[4*i+2] = s->h[i] >> 8;
		out[4*i+3] = s->h[i];
	}
}

static void
hmacsha1(u8 *key, int klen, u8 *msg, int mlen, u8 out[20])
{
	Sha1 s;
	u8 k[64], ipad[64], opad[64], inner[20];
	int i;

	memset(k, 0, sizeof k);
	if(klen > 64){
		sha1init(&s);
		sha1write(&s, key, klen);
		sha1sum(&s, k);
	}else
		memcpy(k, key, klen);
	for(i = 0; i < 64; i++){
		ipad[i] = k[i] ^ 0x36;
		opad[i] = k[i] ^ 0x5c;
	}
	sha1init(&s);
	sha1write(&s, ipad, 64);
	sha1write(&s, msg, mlen);
	sha1sum(&s, inner);
	sha1init(&s);
	sha1write(&s, opad, 64);
	sha1write(&s, inner, 20);
	sha1sum(&s, out);
}

static void
pbkdf2(u8 *pass, int plen, u8 *salt, int slen, int iter, u8 *out, int olen)
{
	u8 u[20], t[20], buf[128];
	int i, j, k, n;

	for(n = 0; n*20 < olen; n++){
		for(i = 0; i < slen; i++)
			buf[i] = salt[i];
		buf[slen] = (n+1) >> 24;
		buf[slen+1] = (n+1) >> 16;
		buf[slen+2] = (n+1) >> 8;
		buf[slen+3] = n+1;
		hmacsha1(pass, plen, buf, slen+4, u);
		memcpy(t, u, 20);
		for(i = 1; i < iter; i++){
			hmacsha1(pass, plen, u, 20, u);
			for(k = 0; k < 20; k++)
				t[k] ^= u[k];
		}
		for(j = 0; j < 20 && n*20+j < olen; j++)
			out[n*20+j] = t[j];
	}
}

/* WPA2-PSK connect.  psk is either 64 hex digits (the raw PSK) or a
   passphrase of at most 32 characters, which the firmware accepts as-is. */
static int
doconnect(char *ssid, char *psk, int forcech)
{
	u8 con[82];
	Ap *a;
	int i, n, ch;

	a = nil;
	for(i = 0; i < naps; i++)
		if(strcmp(aps[i].ssid, ssid) == 0){
			a = &aps[i];
			break;
		}
	memset(con, 0, sizeof con);
	ch = forcech;
	if(a != nil){
		memmove(con + 4, a->bssid, 6);
		if(ch == 0)
			ch = a->ch;
	}
	con[10] = ch;
	con[11] = 0;			/* auth: open */
	if(psk != nil && psk[0] != 0){
		con[12] = 4 | 0x80;	/* pairwise CCMP */
		con[13] = 4 | 0x80;	/* group CCMP */
		con[14] = 2 | 0x80;	/* key mgmt PSK */
		n = strlen(psk);
		if(n != 64)
			pbkdf2((u8*)psk, n, (u8*)ssid, strlen(ssid), 4096, con + 18, 32);
		if(n == 64){
			for(i = 0; i < 32; i++){
				int hi = psk[2*i], lo = psk[2*i+1];
				hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10;
				lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
				con[18 + i] = (hi << 4) | lo;
			}
			con[16] = 32;
		}else
			con[16] = 32;
	}
	n = strlen(ssid);
	if(n > 32)
		n = 32;
	con[17] = n;
	memmove(con + 50, ssid, n);
	print("sdprobe: WIFI_CMD_CONNECT ssid [%s] ch %d bssid %02x:%02x:%02x:%02x:%02x:%02x psk %d\n",
	      ssid, ch, con[4], con[5], con[6], con[7], con[8], con[9], con[16]);
	pksend(10, con, sizeof con, 5000);
	pklisten(15000);
	cpstate("post connect");
	return 0;
}

/* Bring up the CP and issue OPEN + SCAN. */
static int
pkcmd(void)
{
	if(bringup() < 0)
		return -1;
	doscan(3);
	cpstate("post scan");
	return 0;
}

#define CP_START_ADDR	0x40500000
#define CP_RESET_REG	0x40088288
#define SYNC_ADDR	0x405E73B0
#define SYNC_ALL_FINISHED 0xF0F0F0FF
#define SYNC_CALI_WAITING 0xF0F0F0F1
#define SYNC_CALI_WRITE_DONE 0xF0F0F0F2
#define SYNC_VERIFY_WAITING 0xF0F0F0F6
#define SYNC_SDIO_CONFIG_OFF 0x1c
/* sdio_config: enable, sdma rx, 512-byte blocks, inband irq, 20 ms wake. */
#define SYNC_SDIO_CONFIG 0x00880831u

static int
downloadfirmware(void)
{
	char path[] = "/boot/wcnmodem.bin";
	u8 *fw;
	u32 v, last;
	int fd, n, i, e;

	fd = open(path, OREAD);
	if(fd < 0){
		print("sdprobe: cannot open %s: %r\n", path);
		return -1;
	}
	n = seek(fd, 0, 2);
	seek(fd, 0, 0);
	fw = malloc(n);
	if(fw == nil){
		print("sdprobe: out of memory for firmware\n");
		close(fd);
		return -1;
	}
	if(readn(fd, fw, n) != n){
		print("sdprobe: short read on %s\n", path);
		free(fw);
		close(fd);
		return -1;
	}
	close(fd);
	print("sdprobe: firmware %s %d bytes\n", path, n);

	if(dtwrite(CP_START_ADDR, fw, n) < 0){
		print("sdprobe: firmware write failed\n");
		free(fw);
		return -1;
	}
	free(fw);
	print("sdprobe: firmware written to %#ux\n", CP_START_ADDR);

	if(dtread(CP_RESET_REG, &v, 4) < 0){
		print("sdprobe: reset register read failed\n");
		return -1;
	}
	print("sdprobe: CP_RESET_REG %08ux\n", v);
	v &= ~1u;
	if(dtwrite(CP_RESET_REG, &v, 4) < 0){
		print("sdprobe: reset register write failed\n");
		return -1;
	}
	print("sdprobe: CP reset released\n");

	last = 0xffffffff;
	for(i = 0; i < 200; i++){
		if(dtread(SYNC_ADDR, &v, 4) < 0){
			print("sdprobe: sync read failed\n");
			return -1;
		}
		if(v != last){
			print("sdprobe: sync %08ux\n", v);
			last = v;
		}
		if(v == SYNC_CALI_WAITING){
			u32 cfg = SYNC_SDIO_CONFIG;

			if(dtwrite(SYNC_ADDR + SYNC_SDIO_CONFIG_OFF, &cfg, 4) < 0){
				print("sdprobe: sdio_config write failed\n");
				return -1;
			}
			v = SYNC_CALI_WRITE_DONE;
			if(dtwrite(SYNC_ADDR, &v, 4) < 0){
				print("sdprobe: cali done write failed\n");
				return -1;
			}
			print("sdprobe: cali handshake, sdio_config %08ux\n", cfg);
			last = v;
			continue;
		}
		if(v == SYNC_VERIFY_WAITING)
			print("sdprobe: CP wants bind verify (unsupported)\n");
		if(v == SYNC_ALL_FINISHED){
			print("sdprobe: CP boot finished\n");
			return 0;
		}
		sleep(10);
	}
	print("sdprobe: CP boot timed out, last sync %08ux\n", v);
	return -1;
}

/* CMD52, returns the 8-bit data in *val; flags in the upper response bits. */
static int
cmd52(int write, int func, u32 reg, u32 val, u32 *resp)
{
	u32 arg, r;
	int e;

	arg = (write ? 1u<<31 : 0) | ((u32)func << 28) |
	      ((reg & 0x1ffff) << 9) | (val & 0xff);
	e = cmd(52, arg, CmdRespExp | CmdRespCrc, &r);
	if(resp)
		*resp = r;
	return e;
}

static int
sdio_read_cccr(int func, u32 reg, u32 *val)
{
	u32 r;
	int e;

	e = cmd52(0, func, reg, 0, &r);
	if(e)
		return e;
	*val = r & 0xff;
	if(r & 0x0000c000)		/* R5 error flags */
		return -1;
	return 0;
}

static void
cisparse(u8 *p, int n)
{
	int i, code, len;

	for(i = 0; i < n && p[i] != 0xff; ){
		code = p[i++];
		if(code == 0x00){
			print("sdprobe: cis null tuple\n");
			continue;
		}
		if(i >= n)
			break;
		len = p[i++];
		if(i + len > n){
			print("sdprobe: cis tuple %02ux len %d truncated\n", code, len);
			break;
		}
		switch(code){
		case 0x01:
		case 0x02: {
			char str[64];
			int k;

			k = len < (int)sizeof str - 1 ? len : (int)sizeof str - 1;
			memmove(str, p+i, k);
			str[k] = 0;
			print("sdprobe: cis %s \"%s\"\n",
			      code == 1 ? "manufacturer" : "product", str);
			break;
		}
		case 0x03:
			print("sdprobe: cis oid %02ux%02ux\n", p[i], p[i+1]);
			break;
		case 0x04:
			print("sdprobe: cis function extension\n");
			break;
		case 0x20:
			print("sdprobe: cis manufacturer id %02ux%02ux\n", p[i+1], p[i]);
			break;
		case 0x21:
			print("sdprobe: cis product id %02ux%02ux\n", p[i+1], p[i]);
			break;
		default:
			print("sdprobe: cis tuple %02ux len %d\n", code, len);
			break;
		}
		i += len;
	}
}

static void
readcis(int func, u32 ptr, char *what)
{
	u8 buf[512];
	int n;

	if(ptr == 0 || ptr == 0xffffffff){
		print("sdprobe: %s cis pointer empty\n", what);
		return;
	}
	n = 256;
	if(cmd53read(0, ptr, buf, n, 1) == 0){
		print("sdprobe: %s cis at %06ux\n", what, ptr);
		cisparse(buf, n);
	}else
		print("sdprobe: %s cis read failed\n", what);
}

void
main(int argc, char **argv)
{
	u32 r, v, ocr, rca, ptr, cc, ioex, blk;
	int i, e;

	USED(argc);
	USED(argv);
	print("sdprobe: UWE5622 SDIO bring-up\n");

	mmc = (volatile u32 *)mapnode("mmc@fe2c0000", 0xfe2c0000);
	grf = (volatile u32 *)mapnode("syscon@fdc60000", 0xfdc60000);
	pmugrf = (volatile u32 *)mapnode("syscon@fdc20000", 0xfdc20000);
	cru = (volatile u32 *)mapnode("clock-controller@fdd20000", 0xfdd20000);
	print("sdprobe: mmc %p grf %p cru %p\n", mmc, grf, cru);

	pinmux();
	lpo();

	/* WL_REG_ON on GPIO0_D3 = bank 0 pin 27 */
	gpiomount();
	gpioset(27, 0);
	sleep(20);
	gpioset(27, 1);
	print("sdprobe: WL_REG_ON high, gpio0 data %08ux (bit27 %d)\n",
	      gpioread(), (gpioread() >> 27) & 1);
	print("sdprobe: pmugrf vsel %08ux %08ux %08ux\n",
	      rd(pmugrf, 0x140), rd(pmugrf, 0x144), rd(pmugrf, 0x148));
	sleep(200);

	clocks();
	softreset();
	reset();

	/* CMD5: wake the SDIO core */
	e = -1;
	for(i = 0; i < 10; i++){
		e = cmd(5, 0, CmdRespExp, &r);
		print("sdprobe: cmd5 try %d: e=%d resp=%08ux rintsts=%08ux status=%08ux cmd=%08ux cdetect=%08ux\n",
		      i, e, r, lastst, rd(mmc, Status), rd(mmc, Cmd), rd(mmc, Cdetect));
		if(e == 0)
			break;
		sleep(10);
	}
	if(e){
		print("sdprobe: CMD5 no response (%d); no card on sdmmc1\n", e);
		exits("nocard");
	}
	print("sdprobe: CMD5 resp %08ux\n", r);

	/* negotiate the voltage window */
	ocr = (r & 0x00ffffff) & ~0x01000000;	/* clear S18R */
	if((ocr & 0x00ffe000) == 0)
		ocr = Ocr32_33;
	for(i = 0; i < 50; i++){
		e = cmd(5, ocr, CmdRespExp, &r);
		if(e == 0 && (r & (1u<<31)))
			break;
		sleep(10);
	}
	if(e || !(r & (1u<<31))){
		print("sdprobe: CMD5 OCR %08ux not ready (resp %08ux)\n", ocr, r);
		exits("ocr");
	}
	print("sdprobe: CMD5 ready, OCR %08ux\n", r);

	if(cmdcheck(3, 0, CmdRespExp|CmdRespCrc, &r, "SEND_RELATIVE_ADDR")){
		exits("cmd3");
	}
	rca = (r >> RcaShift) & 0xffff;
	print("sdprobe: CMD3 RCA %04ux (resp %08ux)\n", rca, r);

	if(cmdcheck(7, rca << RcaShift, CmdRespExp|CmdRespCrc, &r, "SELECT_CARD")){
		exits("cmd7");
	}
	print("sdprobe: CMD7 selected, resp %08ux\n", r);

	/* CCCR */
	for(i = 0; i <= 6; i++){
		if(sdio_read_cccr(0, i, &v) < 0){
			print("sdprobe: CCCR[%02x] read failed\n", i);
			exits("cccr");
		}
		print("sdprobe: CCCR[%02x] = %02ux\n", i, v);
		if(i == 0) cc = v;
		if(i == 2) ioex = v;
	}

	/* CIS pointer, CCCR 0x09..0x0b */
	ptr = 0;
	for(i = 0; i < 3; i++){
		if(sdio_read_cccr(0, 0x09 + i, &v) < 0)
			break;
		ptr |= v << (8*i);
	}
	print("sdprobe: common CIS pointer %06ux\n", ptr);
	readcis(0, ptr, "common");

	/* enable function 1 and read its CIS pointer from the FBR */
	if(cmd52(1, 0, 0x02, ioex | 0x02, &r) == 0){
		if(sdio_read_cccr(0, 0x03, &v) == 0){
			/* IOReady in the FBR window; wait for function 1 */
			for(i = 0; i < 100; i++){
				if(sdio_read_cccr(0, 0x03, &v) == 0 &&
				   (v & 0x02))
					break;
				sleep(10);
			}
			print("sdprobe: function 1 enabled, IOReady %02ux\n", v);
		}
	}

	/* Enable in-band SDIO interrupts (CCCR Int Enable Master). */
	if(sdio_read_cccr(0, 0x04, &v) == 0){
		print("sdprobe: CCCR[04] int enable %02ux\n", v);
		if((v & 1) == 0 && cmd52(1, 0, 0x04, v | 1, &r) == 0)
			print("sdprobe: card interrupt master enabled\n");
	}

	ptr = 0;
	for(i = 0; i < 3; i++){
		if(cmd52(0, 0, 0x109 + i, 0, &r) < 0)
			break;
		ptr |= (r & 0xff) << (8*i);
	}
	print("sdprobe: function 1 CIS pointer %06ux\n", ptr);
	readcis(1, ptr, "function 1");

	/* FBR: block size and enable */
	blk = 0;
	for(i = 0; i < 2; i++){
		if(cmd52(0, 0, 0x110 + i, 0, &r) == 0)
			blk |= (r & 0xff) << (8*i);
	}
	print("sdprobe: function 1 block size %ux\n", blk);

	/* The vendor driver sets 512-byte blocks and uses DT mode 0x0f. */
	if(blk != 512){
		u32 r2;

		if(cmd52(1, 0, 0x110, 0x00, &r2) == 0 &&
		   cmd52(1, 0, 0x111, 0x02, &r2) == 0){
			blk = 0;
			for(i = 0; i < 2; i++)
				if(cmd52(0, 0, 0x110 + i, 0, &r) == 0)
					blk |= (r & 0xff) << (8*i);
			print("sdprobe: block size set to %ux\n", blk);
		}
	}

	/* Read a harmless AON/APB register through the DT window. */
	{
		u8 dt[512];
		u32 *w = (u32*)dt;

		memset(dt, 0, sizeof dt);
		if(dtsetaddr(0x4083C000) == 0 &&
		   cmd53blockread(1, 0x0f, dt, 1) == 0){
			print("sdprobe: DT read AON_APB: %08ux %08ux %08ux %08ux\n",
			      w[0], w[1], w[2], w[3]);
		}else
			print("sdprobe: DT read failed\n");

		/* Write a pattern to CP SRAM and read it back. */
		for(i = 0; i < 512; i++)
			dt[i] = i;
		if(dtwrite(0x40500000, dt, 512) == 0 &&
		   dtsetaddr(0x40500000) == 0 &&
		   cmd53blockread(1, 0x0f, dt, 1) == 0){
			for(i = 0; i < 512; i++)
				if(dt[i] != (u8)i)
					break;
			print("sdprobe: DT write/read CP SRAM: %s (%d/512)\n",
			      i == 512 ? "ok" : "mismatch", i);
		}else
			print("sdprobe: DT write failed\n");
	}

	if(argc > 1 && (strcmp(argv[1], "firmware") == 0 ||
			 strcmp(argv[1], "pk") == 0))
		downloadfirmware();
	if(argc > 1 && strcmp(argv[1], "pk") == 0)
		pkcmd();
	if(argc > 1 && strcmp(argv[1], "connect") == 0){
		if(bringup() < 0){
			print("sdprobe: bringup failed\n");
		}else{
			for(int r = 0; r < 5; r++){
				int i, found = 0;

				doscan(1);
				for(i = 0; i < naps; i++)
					if(strcmp(aps[i].ssid, argv[2]) == 0)
						found = 1;
				if(found)
					break;
			}
			doconnect(argv[2], argc > 3 ? argv[3] : nil,
				  argc > 4 ? atoi(argv[4]) : 0);
			if(connected)
				dhcp();
		}
	}
	if(argc > 1 && strcmp(argv[1], "cmd") == 0){
		int id = argc > 2 ? atoi(argv[2]) : 1;

		downloadfirmware();
		cpstate("pre cmd");
		pksend(id, nil, 0, 2000);
		cpstate("post cmd");
	}
	if(argc > 1 && strcmp(argv[1], "ini") == 0){
		int nsec = argc > 2 ? atoi(argv[2]) : 1;
		int k;

		downloadfirmware();
		for(k = 1; k <= nsec; k++){
			if(k == 1)
				send_ini((u8*)wifi_sec1, sizeof(wifi_sec1), 1);
			else if(k == 2)
				send_ini((u8*)wifi_sec2, sizeof(wifi_sec2), 2);
			else
				send_ini((u8*)wifi_sec3, sizeof(wifi_sec3), 3);
			sleep(100);
			cpstate("after sec");
		}
		for(k = 0; k < 10; k++){
			cpstate("watch");
			sleep(500);
		}
	}
	if(argc > 1 && strcmp(argv[1], "slp") == 0){
		u32 r;

		downloadfirmware();
		for(i = 0; i < 24; i++){
			if(i == 8){
				cmd52(1, 0, 0x1a2, 0, &r);
				print("sdprobe: --- wrote slpctl(1a2)=0\n");
			}
			if(i == 14){
				cmd52(1, 0, 0x1aa, 0, &r);
				print("sdprobe: --- wrote slpctl(1aa)=0\n");
			}
			if(i == 20){
				cmd52(1, 0, 0x1a3, 6, &r);
				cmd52(1, 0, 0x1a2, 0, &r);
				print("sdprobe: --- wrote pmusel(1a3)=6 slpctl(1a2)=0\n");
			}
			cpstate("watch");
			sleep(500);
		}
	}

	print("sdprobe: DONE\n");
	exits(nil);
}
