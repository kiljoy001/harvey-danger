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

	/* CTRL */
	CtlReset	= 1<<0,
	CtlFifoReset	= 1<<1,
	CtlDmaReset	= 1<<2,
	CtlIntEn	= 1<<4,
	CtlDmaEn	= 1<<5,

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

	print("sdprobe: DONE\n");
	exits(nil);
}
