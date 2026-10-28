/* emmc: SDHCI PIO driver for the RK3566 eMMC (rockchip,rk3568-dwcmshc).
 *
 * Runs as a user process against the #P grant for mmc@fe310000.  No DMA:
 * commands go through the SDHCI command/response registers and data moves
 * through the buffer data port.  Enough to identify the card and read or
 * write 512-byte blocks.
 *
 *   emmc probe           identify the card and print CID/CSD
 *   emmc read <lba>      read one block, print a hex dump
 *   emmc write <lba>     write a pattern and read it back
 */
#include <u.h>
#include <libc.h>

/* SDHCI register offsets (SD Host Controller Simplified Spec 3.0). */
enum {
	Sdma		= 0x00,
	Blksize		= 0x04,
	Blkcount	= 0x06,
	Argument	= 0x08,
	Xfer		= 0x0C,
	Cmdreg		= 0x0E,
	Response	= 0x10,
	Buffer		= 0x20,
	Present		= 0x24,
	Hostctl		= 0x28,
	Power		= 0x29,
	Clock		= 0x2C,
	Timeout		= 0x2E,
	Swreset		= 0x2F,
	Nis		= 0x30,
	Eis		= 0x32,
	Nise		= 0x34,
	Eise		= 0x36,
	Hc2		= 0x3E,
	Caps		= 0x40,
	Version		= 0xFE,
};

/* Normal interrupt status bits. */
enum {
	IntCc	= 1<<0,
	IntTc	= 1<<1,
	IntBge	= 1<<2,
	IntDint	= 1<<3,
	IntBwr	= 1<<4,
	IntBrr	= 1<<5,
	IntErr	= 1<<15,
};

/*
 * Command register flags.  The command index lives in bits 8-15 and these
 * flags in bits 0-5 (SDHCI Simplified Spec 3.0, SDHCI_MAKE_CMD).
 */
enum {
	CmdData	= 1<<5,
	CmdIdx	= 1<<4,
	CmdCrc	= 1<<3,
	Rsp136	= 1<<0,
	Rsp48	= 2<<0,
	Rsp48b	= 3<<0,
};

static u8 *mmio;
static int verbose;
static int cmdwait = 500;

static void dllbypass(void);

static u32
rd32(int off)
{
	return *(volatile u32 *)(mmio + off);
}

static void
wr32(int off, u32 v)
{
	*(volatile u32 *)(mmio + off) = v;
}

static u16
rd16(int off)
{
	return *(volatile u16 *)(mmio + off);
}

static void
wr16(int off, u16 v)
{
	*(volatile u16 *)(mmio + off) = v;
}

static u8
rd8(int off)
{
	return *(volatile u8 *)(mmio + off);
}

static void
wr8(int off, u8 v)
{
	*(volatile u8 *)(mmio + off) = v;
}

static void
fail(char *fmt, ...)
{
	va_list a;

	va_start(a, fmt);
	vfprint(2, fmt, a);
	va_end(a);
	fprint(2, "\n");
	exits("emmc");
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
		fail("emmc: no %s in #P: %r", node);
	if(read(fd, buf, sizeof buf - 1) <= 0)
		fail("emmc: cannot read %s reg", node);
	close(fd);
	buf[sizeof buf - 1] = 0;
	base = strtoull(buf, &p, 0);
	snprint(path, sizeof path, "#P/%s/size", node);
	if((fd = open(path, OREAD)) < 0)
		fail("emmc: cannot open %s size", node);
	if(read(fd, buf, sizeof buf - 1) <= 0)
		fail("emmc: cannot read %s size", node);
	close(fd);
	buf[sizeof buf - 1] = 0;
	size = strtoull(buf, &p, 0);
	if(base != want || size == 0)
		fail("emmc: unexpected %s %#llux+%#llux", node, base, size);
	len = (size + 0x1fffff) & ~0x1fffffULL;
	m = segattach(0, node, 0, len);
	if(m == (void *)-1)
		fail("emmc: segattach %s failed: %r", node);
	/* The grant covers the 2 MiB block that contains the device. */
	m += base & 0x1fffff;
	if(verbose)
		print("emmc: %s %#llux mapped at %p\n", node, base, m);
	return m;
}

static int
mapdev(void)
{
	mmio = mapnode("mmc@fe310000", 0xfe310000);
	return 0;
}

/*
 * The eMMC clocks are gated in the CRU.  U-Boot loads the kernel over the
 * network and leaves them off, so the card interface has no clock and no
 * command ever completes.  Enable the ACLK/HCLK/BCLK/CCLK/TCLK gates and
 * select GPLL 200 MHz for the card clock.
 */
static void
clocks(void)
{
	u8 *cru;

	cru = mapnode("clock-controller@fdd20000", 0xfdd20000);
	*(volatile u32 *)(cru + 0x324) = (0x3e0u << 16) | 0;
	*(volatile u32 *)(cru + 0x170) = (0x7300u << 16) | 0x1000;
	__asm__ volatile("dsb sy" ::: "memory");
	sleep(10);
	if(verbose)
		print("emmc: cru gates %ux sel %ux\n",
		      *(volatile u32 *)(cru + 0x324),
		      *(volatile u32 *)(cru + 0x170));
}

static void
dumpregs(void)
{
	u32 caps, caps1;
	u16 ver;
	int base;

	ver = rd16(Version);
	caps = rd32(Caps);
	caps1 = rd32(Caps + 4);
	base = (caps >> 8) & 0xff;
	print("emmc: version %ux caps %ux %ux baseclk %d MHz\n",
	      ver, caps, caps1, base);
	print("emmc: present %ux clock %ux power %ux hostctl %ux hc2 %ux\n",
	      rd32(Present), rd16(Clock), rd8(Power), rd8(Hostctl), rd16(Hc2));
	print("emmc: nis %ux eis %ux timeout %ux\n",
	      rd16(Nis), rd16(Eis), rd8(Timeout));
}

/* DWCMSHC vendor registers (offsets from the host base). */
enum {
	DllCtrl		= 0x800,
	DllRxclk	= 0x804,
	DllTxclk	= 0x808,
	DllStrbin	= 0x80C,
	DllCmdout	= 0x810,
	DllStatus0	= 0x840,
	MiscCon		= 0x81C,
	VendorArea1	= 0xE8,
	HostCtrl3	= 0x08,
	/* PHY register area (DWC_MSHC_PTR_PHY_R). */
	PhyCnfg		= 0x300,	/* 32-bit */
	PhyCmdpad	= 0x304,	/* 16-bit */
	PhyDatapad	= 0x306,
	PhyClkpad	= 0x308,
	PhyStbpad	= 0x30A,
	PhyRstnpad	= 0x30C,
	PhySdclkdlCnfg	= 0x31D,	/* 8-bit */
	PhySdclkdlDc	= 0x31E,
	PhySmpldlCnfg	= 0x320,
	PhyAtdlCnfg	= 0x321,
	PhyDllCtrl	= 0x324,
	PhyDllCnfg2	= 0x326,
	PhyDlldlCnfg	= 0x328,
};

/*
 * dwcmshc_phy_init() from the 6.18 driver, installed there as the
 * voltage_switch callback so it runs during the initial 1.8 V switch.
 * The important part is PHY_CNFG_RSTN_DEASSERT: without it the PHY stays
 * in reset and the card never sees a clock or a command.
 */
static void
phyinit(void)
{
	u32 val;
	u8 b;
	int rxsel;

	/* vqmmc is vcc_1v8, so select the 1.8 V receiver. */
	rxsel = 1;			/* PHY_PAD_RXSEL_1V8 */

	val = 1;			/* PHY_CNFG_RSTN_DEASSERT */
	val |= 0x0c << 16;		/* PHY_CNFG_PAD_SP */
	val |= 0x0c << 20;		/* PHY_CNFG_PAD_SN */
	wr32(PhyCnfg, val);

	wr8(PhySdclkdlCnfg, 1<<4);	/* SDCLKDL_CNFG_UPDATE */
	wr8(PhySdclkdlDc, 0x32);	/* PHY_SDCLKDL_DC_DEFAULT */
	wr8(PhyDllCnfg2, 0x0a);		/* DLL_CNFG2_JUMPSTEP */
	b = rd8(PhySdclkdlCnfg) & ~(1<<4);
	wr8(PhySdclkdlCnfg, b);

	val = rxsel | (1<<3) | (0x3<<5) | (0x3<<9);	/* pullup, slew P/N */
	wr16(PhyCmdpad, val);
	wr16(PhyDatapad, val);
	wr16(PhyRstnpad, val);
	wr16(PhyClkpad, (0x3<<5) | (0x3<<9));
	wr16(PhyStbpad, rxsel | (2<<3) | (0x3<<5) | (0x3<<9));

	if(rxsel == 1)
		wr8(PhyDlldlCnfg, 0x3<<5);	/* SLV_INPSEL */
	wr8(PhyDllCtrl, 1);
	if(verbose)
		print("emmc: phy cnfg %ux cmdpad %ux dll %ux\n",
		      rd32(PhyCnfg), rd16(PhyCmdpad), rd8(PhyDllCtrl));
}

/*
 * Vendor bring-up for rk3568-dwcmshc, mirrored from the BSP driver:
 * the internal clock block must be enabled, the command conflict check
 * disabled, and the DLL bypassed below 52 MHz.  Without the internal
 * clock no command ever completes.
 */
static void
vendorinit(void)
{
	u16 area;
	u32 v;

	wr32(MiscCon, rd32(MiscCon) | (1<<1));	/* MISC_INTCLK_EN */
	area = rd16(VendorArea1);
	if(verbose)
		print("emmc: vendor area1 %ux misc %ux\n", area, rd32(MiscCon));
	if(area != 0 && area != 0xffff){
		v = rd32(area + HostCtrl3);
		wr32(area + HostCtrl3, v & ~1);	/* cmd conflict check off */
	}
	dllbypass();
}

/*
 * dwcmshc_rk3568_set_clock(): Linux calls clk_set_rate(cclk_emmc, 375000)
 * before sdhci_set_clock().  cclk_emmc's parent 5 is clk_osc0_div_375k
 * (xin24m / 64), so identify mode selects that parent and the SDHCI
 * divider is then 1 (field 0): cclk_emmc is the "core" clock, and
 * dwcmshc_get_max_clock() returns its rate, not CAPS.
 */
static void
cru_emmc_200m(void)
{
	u8 *cru;

	cru = mapnode("clock-controller@fdd20000", 0xfdd20000);
	*(volatile u32 *)(cru + 0x170) = (7u<<12)<<16 | (1u<<12);	/* gpll_200m */
	*(volatile u32 *)(cru + 0x324) = (1u<<8)<<16 | 0;
	__asm__ volatile("dsb sy" ::: "memory");
	sleep(10);
	if(verbose)
		print("emmc: cclk_emmc 200m sel %ux\n", *(volatile u32 *)(cru + 0x170));
}

static void
cru_emmc_375k(void)
{
	u8 *cru;

	cru = mapnode("clock-controller@fdd20000", 0xfdd20000);
	/*
	 * Parent 5 is clk_osc0_div_375k = clk_osc0_div_750k / 2, and that
	 * clock is a divider (CLKSEL_CON(82) bits 8-13) which only the clock
	 * framework programs.  With a zero divider the parent has no clock,
	 * so the card would never see a single edge.  Program it for
	 * 24 MHz / 32 = 750 kHz and enable its gate.
	 */
	*(volatile u32 *)(cru + 0x248) = (0x3fu<<8)<<16 | (0x1fu<<8);
	*(volatile u32 *)(cru + 0x38c) = (1u<<15)<<16 | 0;
	*(volatile u32 *)(cru + 0x170) = (7u<<12)<<16 | (5u<<12);
	*(volatile u32 *)(cru + 0x324) = (1u<<8)<<16 | 0;	/* gate on */
	__asm__ volatile("dsb sy" ::: "memory");
	sleep(20);
	if(verbose)
		print("emmc: cclk_emmc sel %ux gates %ux osc750 %ux\n",
		      *(volatile u32 *)(cru + 0x170),
		      *(volatile u32 *)(cru + 0x324),
		      *(volatile u32 *)(cru + 0x248));
}

/*
 * The vendor >52 MHz branch of dwcmshc_rk3568_set_clock(): reset the DLL,
 * start it, wait for lock, then program ATCTRL and the RX/TX taps.  Linux
 * runs this at probe time (sdhci_setup_host -> sdhci_set_clock(max_clk))
 * before any card command, so the PHY is initialised even when identify
 * mode later bypasses the DLL.
 */
static int
dllstart(void)
{
	u32 v, extra, lock;
	int i;

	wr32(DllCtrl, 1<<1);
	sleep(1);
	wr32(DllCtrl, 0);
	wr32(DllCtrl, (0x5<<16) | (0x2<<8) | 1);
	for(i = 0; i < 500; i++){
		v = rd32(DllStatus0);
		if(v & (1<<8))
			break;
		if(v & (1<<9)){
			print("emmc: DLL lock timeout %ux\n", v);
			return -1;
		}
		sleep(1);
	}
	if(i == 500)
		return -1;
	lock = ((rd32(DllStatus0) & 0xff) * 2) & 0xff;
	wr32(0x40, (1u<<16) | (3u<<17) | (3u<<19));	/* ATCTRL */
	extra = (1u<<27) | (1u<<31) | (1u<<25) | (lock << 8);
	wr32(DllRxclk, extra);
	wr32(DllTxclk, 0x10);				/* hs200_tx_tap */
	if(verbose)
		print("emmc: dll lock %ux rxclk %ux\n", lock, extra);
	return 0;
}

static int
clock200(void)
{
	int i;

	wr16(Clock, 0);
	wr16(Clock, 1);			/* INT_EN, div 0 = 200 MHz */
	for(i = 0; i < 150; i++){
		if(rd16(Clock) & 2)
			break;
		sleep(1);
	}
	if((rd16(Clock) & 2) == 0)
		return -1;
	if(dllstart() < 0)
		return -1;
	wr16(Clock, 5);
	return 0;
}

/* sdhci_set_clock() with the 375 kHz parent: no division. */
static int
setclockbase(void)
{
	int i;

	/*
	 * sdhci_calc_clk(): max_clk (375 kHz) <= clock, so div starts at 1,
	 * then div >>= 1 makes it 0 -- and SDHCI_QUIRK2_CLOCK_DIV_ZERO_BROKEN
	 * (set for rk35xx) forces it back to 1.  Writing 0 here leaves the
	 * card without a usable clock.
	 */
	wr16(Clock, 0);
	/*
	 * Reproduce the value the working 6.18 kernel writes: divider 267
	 * (0x10b, encoded as 0x0b in the low field and bit 6 set), which with
	 * cclk_emmc = 375 kHz gives 702 Hz.  Linux enables the card clock
	 * first and only then writes the DLL bypass registers.
	 */
	wr16(Clock, 0x0b41);		/* INT_EN, divider 267 */
	for(i = 0; i < 150; i++){
		if(rd16(Clock) & 2)
			break;
		sleep(1);
	}
	if((rd16(Clock) & 2) == 0)
		return -1;
	wr16(Clock, 0x0b47);		/* INT_EN | CARD_EN, divider 267 */
	dllbypass();
	if(verbose)
		print("emmc: clock %ux\n", rd16(Clock));
	return (rd16(Clock) & 4) ? 0 : -1;
}

static int
reset(void)
{
	int i;

	for(i = 0; i < 3; i++)
		wr8(Swreset, 1<<i);
	for(i = 0; i < 100; i++){
		if((rd8(Swreset) & 7) == 0)
			return 0;
		sleep(1);
	}
	return -1;
}

static void
dllbypass(void)
{
	wr32(DllCtrl, 0);
	wr32(DllCtrl, (1<<24) | 1);		/* bypass | start */
	wr32(DllRxclk, 1<<31);
	wr32(DllTxclk, 0);
	wr32(DllCmdout, 0);
	wr32(DllStrbin, (1<<27) | (1<<26) | (0x16 << 16));
}

static int
setclock(u32 hz)
{
	u32 base;
	int div, i;
	u16 v;

	base = (rd32(Caps) >> 8) & 0xff;
	if(base == 0)
		base = 200;
	/* SDCLK = base / (2 * div) for div > 0. */
	div = (base * 1000000) / (2 * hz);
	if(div < 1)
		div = 1;
	if(div > 0x3ff)
		div = 0x3ff;
	/* Same order as the vendor driver: set the divider and internal
	 * clock, disable the output clock, configure the DLL, then enable. */
	wr16(Clock, 0);
	wr16(Clock, ((div & 0xff) << 8) | ((div >> 8) << 6) | 1);
	for(i = 0; i < 100; i++){
		if(rd16(Clock) & 2)
			break;
		sleep(1);
	}
	if((rd16(Clock) & 2) == 0)
		return -1;
	if(hz <= 52000000)
		dllbypass();
	wr16(Clock, ((div & 0xff) << 8) | ((div >> 8) << 6) | 1 | 4);
	v = rd16(Clock);
	if(verbose)
		print("emmc: clock div %d -> %ux\n", div, v);
	return (v & 4) ? 0 : -1;
}

static int
command(int idx, u32 arg, u32 flags, u32 *resp)
{
	int i;
	u16 st;

	/* sdhci_send_command(): wait for CMD inhibit first, and always write
	 * the transfer mode register, even for a command without data. */
	for(i = 0; i < 100; i++){
		if((rd32(Present) & 1) == 0)
			break;
		sleep(1);
	}
	wr16(Nis, 0xffff);
	wr16(Eis, 0xffff);
	wr32(Argument, arg);
	if(flags & CmdData){
		u16 mode;

		mode = rd16(Xfer);
		wr16(Xfer, mode & ~((1<<2)|(1<<3)));	/* clear auto CMD12/23 */
	} else
		wr16(Xfer, 0);		/* no data: transfer mode must be zero */
	/* #P maps registers as Normal-NC; order the argument/mode before the
	 * command.  Without this the CPU may issue the command first. */
	__asm__ volatile("dsb sy" ::: "memory");
	wr16(Cmdreg, (idx << 8) | flags);
	__asm__ volatile("dsb sy" ::: "memory");
	for(i = 0; i < cmdwait; i++){
		st = rd16(Nis);
		if((st & IntErr) || (rd16(Eis) & 0xffff)){
			if(verbose)
				print("emmc: cmd %d error nis %ux eis %ux present %ux\n",
				      idx, st, rd16(Eis), rd32(Present));
			return -1;
		}
		if(st & IntCc)
			break;
		sleep(1);
	}
	if(i == cmdwait){
		if(verbose)
			print("emmc: cmd %d timeout nis %ux eis %ux present %ux\n",
			      idx, rd16(Nis), rd16(Eis), rd32(Present));
		return -1;
	}
	if(resp != nil){
		resp[0] = rd32(Response);
		resp[1] = rd32(Response + 4);
		resp[2] = rd32(Response + 8);
		resp[3] = rd32(Response + 12);
	}
	wr16(Nis, st);
	return 0;
}

static int
dataio(u8 *buf, int n, int write)
{
	int i, got;
	u16 st;

	if(write){
		for(i = 0; i < 20000; i++){
			st = rd16(Nis);
			if(st & IntErr){
				print("emmc: wr err nis %ux eis %ux present %ux\n",
				      st, rd16(Eis), rd32(Present));
				return -1;
			}
			if(rd32(Present) & (1<<10))	/* buffer write enable */
				break;
			sleep(1);
		}
		if(i == 20000){
			print("emmc: wr bwr timeout nis %ux eis %ux present %ux\n",
			      rd16(Nis), rd16(Eis), rd32(Present));
			return -1;
		}
		for(got = 0; got < n; got += 4)
			wr32(Buffer, *(u32 *)(buf + got));
		wr16(Nis, IntBwr);
	}else{
		for(got = 0; got < n; ){
			for(i = 0; i < 20000; i++){
				st = rd16(Nis);
				if(st & IntErr){
					print("emmc: data err nis %ux eis %ux present %ux\n",
					      st, rd16(Eis), rd32(Present));
					return -1;
				}
				if(rd32(Present) & (1<<11))	/* buffer read enable */
					break;
				sleep(1);
			}
			if(i == 20000){
				print("emmc: data brr timeout nis %ux eis %ux present %ux\n",
				      rd16(Nis), rd16(Eis), rd32(Present));
				return -1;
			}
			{
				int chunk = n - got;
				if(chunk > 512)
					chunk = 512;
				for(i = 0; i < chunk; i += 4)
					*(u32 *)(buf + got + i) = rd32(Buffer);
				got += chunk;
			}
			wr16(Nis, IntBrr);
		}
	}
	for(i = 0; i < 20000; i++){
		st = rd16(Nis);
		if(st & IntErr){
			print("emmc: data tc err nis %ux eis %ux present %ux\n",
			      st, rd16(Eis), rd32(Present));
			return -1;
		}
		if(st & IntTc){
			wr16(Nis, st);
			return 0;
		}
		sleep(1);
	}
	print("emmc: data tc timeout nis %ux eis %ux present %ux\n",
	      rd16(Nis), rd16(Eis), rd32(Present));
	return -1;
}

static int
readblock(u32 lba, u8 *buf)
{
	wr16(Blksize, 512);
	wr16(Blkcount, 1);
	wr16(Xfer, 1<<4);		/* read, single block (SDHCI_TRNS_READ) */
	u32 r[4];

	print("emmc: readblk lba %ud blksize %ux blkcnt %ux xfer %ux\n",
	      lba, rd16(Blksize), rd16(Blkcount), rd16(Xfer));
	if(command(17, lba, CmdData|Rsp48|CmdCrc|CmdIdx, r) < 0){
		print("emmc: cmd17 fail nis %ux eis %ux present %ux\n",
		      rd16(Nis), rd16(Eis), rd32(Present));
		return -1;
	}
	print("emmc: cmd17 ok r1 %08ux nis %ux present %ux\n", r[0],
	      rd16(Nis), rd32(Present));
	return dataio(buf, 512, 0);
}

static int
writeblock(u32 lba, u8 *buf)
{
	wr16(Blksize, 512);
	wr16(Blkcount, 1);
	wr16(Xfer, 0);			/* write, single block */
	{
		u32 r[4];

		if(command(24, lba, CmdData|Rsp48|CmdCrc|CmdIdx, r) < 0){
			print("emmc: cmd24 fail nis %ux eis %ux present %ux\n",
			      rd16(Nis), rd16(Eis), rd32(Present));
			return -1;
		}
		print("emmc: cmd24 ok r1 %08ux nis %ux present %ux\n", r[0],
		      rd16(Nis), rd32(Present));
	}
	return dataio(buf, 512, 1);
}

/* dwcmshc_rk35xx_init(): clear the command conflict check and DLL leftovers. */
static void
vendor_probe(void)
{
	u16 area;

	area = rd16(VendorArea1) & 0xfff;
	if(verbose)
		print("emmc: area1 %ux\n", area);
	if(area != 0)
		wr32(area + HostCtrl3, 0);
	wr32(DllTxclk, 0);
	wr32(DllStrbin, 0);
}

/* sdhci_reset(SDHCI_RESET_ALL) followed by rk35xx_sdhci_reset(). */
static void
resetall(void)
{
	u8 *cru;
	u32 extra;
	int i;

	/*
	 * rk35xx_sdhci_reset(): the creator's dtsi gives the controller five
	 * resets (SRST_A/H/B/C/T_EMMC = ids 117..121), which land in
	 * CRU SOFTRST_CON(7) = 0x41c, bits 5-9.  Pulse them before touching
	 * the controller; leaving them asserted holds the card interface in
	 * reset even though the register block still answers.
	 */
	cru = mapnode("clock-controller@fdd20000", 0xfdd20000);
	if(verbose)
		print("emmc: emmc resets before %ux\n",
		      *(volatile u32 *)(cru + 0x41c));
	/* Only deassert: the bootloader has already initialised the controller,
	 * and pulsing a controller reset here requires a full re-probe. */
	if(*(volatile u32 *)(cru + 0x41c) & 0x3e0){
		*(volatile u32 *)(cru + 0x41c) = (0x3e0u<<16) | 0;
		__asm__ volatile("dsb sy" ::: "memory");
		sleep(1);
	}
	if(verbose)
		print("emmc: emmc resets after %ux\n",
		      *(volatile u32 *)(cru + 0x41c));

	extra = rd32(MiscCon);
	wr8(Swreset, 4);
	for(i = 0; i < 100; i++){
		if((rd8(Swreset) & 4) == 0)
			break;
		sleep(1);
	}
	wr8(Timeout, 0x0e);		/* sdhci_init(): max data timeout */
	wr32(MiscCon, (1<<1) | extra);	/* MISC_INTCLK_EN | extra */
	if(verbose)
		print("emmc: reset all done misc %ux\n", rd32(MiscCon));
}

/* sdhci_voltage_switch(MMC_SIGNAL_VOLTAGE_180): vqmmc is vcc_1v8. */
static void
setvoltage180(void)
{
	wr16(Hc2, rd16(Hc2) | (1<<3));
	sleep(5);
	if(verbose)
		print("emmc: hc2 %ux\n", rd16(Hc2));
}

/* sdhci_set_power_noreg(): VCC comes from vmmc (vcc_3v3). */
static void
setpower(u8 pwr)
{
	wr8(Power, 0);
	wr8(Power, pwr | 1);		/* SDHCI_POWER_ON */
	if(verbose)
		print("emmc: power %ux\n", rd8(Power));
}

/* sdhci_set_bus_width() */
static void
setwidth(int width)
{
	u8 ctrl;

	ctrl = rd8(Hostctl);
	ctrl &= ~((1<<1) | (1<<5));	/* 4-bit and 8-bit select */
	if(width == 8)
		ctrl |= 1<<5;
	else if(width == 4)
		ctrl |= 1<<1;
	wr8(Hostctl, ctrl);
	if(verbose)
		print("emmc: hostctl %ux\n", rd8(Hostctl));
}

/*
 * RK809 POWER_EN(3) = 0xb4 holds both eMMC rails:
 *   SWITCH_REG1 (vcc_3v3, vmmc)  = BIT(2) with mask BIT(6)
 *   DCDC_REG5   (vcc_1v8, vqmmc) = BIT(1) with mask BIT(5)
 * Write 0x66 to assert both, in case the bootloader left them off.
 */
static int
rails_set(int v)
{
	char ctl[64], dat[64], cmd[32], h[32];
	int fd, m;

	if(access("/mnt/i2c", AEXIST) < 0)
		create("/mnt/i2c", OREAD, DMDIR|0755);
	fd = open("/srv/i2c", OREAD);
	if(fd < 0 || mount(fd, -1, "/mnt/i2c", MREPL|MCREATE, "") < 0)
		return -1;
	close(fd);
	snprint(ctl, sizeof ctl, "/mnt/i2c/i2c0/ctl");
	snprint(dat, sizeof dat, "/mnt/i2c/i2c0/data");
	fd = open(ctl, ORDWR);
	if(fd < 0)
		return -1;
	snprint(cmd, sizeof cmd, "w 32 180 %d", v);
	m = write(fd, cmd, strlen(cmd));
	close(fd);
	sleep(50);
	fd = open(ctl, ORDWR);
	write(fd, "r 32 180 1", 10);
	close(fd);
	fd = open(dat, OREAD);
	m = fd < 0 ? -1 : read(fd, h, sizeof h - 1);
	if(fd >= 0)
		close(fd);
	if(m > 0){
		h[m] = 0;
		print("emmc: power_en3 = 0x%s\n", h);
	}else
		print("emmc: power_en3 unreadable\n");
	return 0;
}

static int
rails_on(void)
{
	return rails_set(0x66);
}

/*
 * Transfer clock, following the vendor driver's model: cclk_emmc is set to
 * the target rate by the CRU and the SDHCI divider stays 0, so the card
 * sees cclk_emmc directly.  xin24m is 24 MHz, inside the eMMC legacy limit
 * and far above the 702 Hz identify clock.
 */
static int
setclock24m(void)
{
	u8 *cru;
	int i;

	cru = mapnode("clock-controller@fdd20000", 0xfdd20000);
	*(volatile u32 *)(cru + 0x170) = (7u<<12)<<16 | (0u<<12);	/* xin24m */
	*(volatile u32 *)(cru + 0x324) = (1u<<8)<<16 | 0;		/* gate on */
	__asm__ volatile("dsb sy" ::: "memory");
	sleep(10);
	wr16(Clock, 0);
	wr16(Clock, 1);			/* INT_EN, divider 0 */
	for(i = 0; i < 150; i++){
		if(rd16(Clock) & 2)
			break;
		sleep(1);
	}
	if((rd16(Clock) & 2) == 0)
		return -1;
	dllbypass();
	wr16(Clock, 5);			/* INT_EN | CARD_EN, divider 0 */
	if(verbose)
		print("emmc: 24m sel %ux clock %ux\n",
		      *(volatile u32 *)(cru + 0x170), rd16(Clock));
	return (rd16(Clock) & 4) ? 0 : -1;
}

/*
 * High-speed transfer: CMD6 sets HS_TIMING=1, then cclk_emmc switches to
 * cpll_50m (50 MHz) with the SDHCI divider at 0.  Falls back to 24 MHz.
 */
static int
setclock50m(void)
{
	u8 *cru;
	u32 r[4];
	int i;

	if(command(6, 0x03B90100, Rsp48b|CmdCrc|CmdIdx, r) < 0){
		if(verbose)
			print("emmc: cmd6 hs failed nis %ux eis %ux\n",
			      rd16(Nis), rd16(Eis));
		return -1;
	}
	for(i = 0; i < 1000; i++){
		if((rd32(Present) & (1<<2)) == 0)
			break;
		sleep(1);
	}
	cru = mapnode("clock-controller@fdd20000", 0xfdd20000);
	*(volatile u32 *)(cru + 0x170) = (7u<<12)<<16 | (4u<<12);	/* cpll_50m */
	*(volatile u32 *)(cru + 0x324) = (1u<<8)<<16 | 0;
	__asm__ volatile("dsb sy" ::: "memory");
	sleep(10);
	wr16(Clock, 0);
	wr16(Clock, 1);
	for(i = 0; i < 150; i++){
		if(rd16(Clock) & 2)
			break;
		sleep(1);
	}
	if((rd16(Clock) & 2) == 0)
		return -1;
	dllbypass();
	wr16(Clock, 5);
	if(verbose)
		print("emmc: 50m sel %ux clock %ux\n",
		      *(volatile u32 *)(cru + 0x170), rd16(Clock));
	return (rd16(Clock) & 4) ? 0 : -1;
}

static int
initcard(u32 *ocrp)
{
	u32 resp[4], ocr;
	int i;

	/*
	 * Ported from the vendor kernel, in this order:
	 *   dwcmshc_probe()        -> dwcmshc_rk35xx_init()
	 *   mmc_power_off()        -> sdhci_reinit() -> reset ALL
	 *   mmc_power_up()         -> mmc_set_initial_signal_voltage(1.8V)
	 *   sdhci_set_ios()        -> set_clock, set_power, set_bus_width
	 * The card's VCC is vcc_3v3 (POWER = 3.3V) while its IO is vcc_1v8
	 * (HOST_CONTROL2 VDD_180), which is the one combination never tried.
	 */
	resetall();
	vendor_probe();
	/*
	 * sdhci_set_ios() order, matching the working 6.18 kernel: power,
	 * bus width, 1.8 V signalling, then the interface clock.  Writing
	 * the power register after the clock drops CLOCK_CARD_EN.
	 */
	wr8(Power, 0);
	wr8(Power, 1);			/* sdhci_set_power_reg(): power only */
	setwidth(1);
	wr16(Hc2, 1<<3);		/* 1.8 V signalling (vqmmc = vcc_1v8) */
	cru_emmc_375k();
	if(setclockbase() < 0)
		return -1;
	sleep(20);

	if(command(0, 0, 0, nil) < 0)
		return -1;
	if(verbose)
		print("emmc: cmd0 ok present %ux\n", rd32(Present));
	/*
	 * CMD1: SEND_OP_COND.  This eMMC only answers inside the 1.8 V OCR
	 * window (bit 7), which is what U-Boot's voltage-select retry does;
	 * offering 3.3 V alone times out.  Fall back to the wide window for
	 * cards that want it.
	 */
	ocr = 0;
	for(i = 0; i < 100; i++){
		int quiet = verbose;

		if(i >= 3)
			verbose = 0;
		if(command(1, (1u<<30) | (1u<<7), Rsp48, resp) == 0)
			ocr = resp[0];
		else if(command(1, 0x40ff8000, Rsp48, resp) == 0)
			ocr = resp[0];
		verbose = quiet;
		if(ocr & (1u<<31))
			break;
		sleep(10);
	}
	verbose = 1;
	if(!(ocr & (1u<<31)))
		return -1;
	if(ocrp != nil)
		*ocrp = ocr;

	if(command(2, 0, Rsp136|CmdCrc, resp) < 0)
		return -1;
	if(verbose)
		print("emmc: cid %08ux %08ux %08ux %08ux\n",
		      resp[0], resp[1], resp[2], resp[3]);
	if(command(3, 1<<16, Rsp48|CmdCrc|CmdIdx, resp) < 0)
		return -1;
	if(command(9, 1<<16, Rsp136|CmdCrc, resp) < 0)
		return -1;
	if(verbose)
		print("emmc: csd %08ux %08ux %08ux %08ux\n",
		      resp[0], resp[1], resp[2], resp[3]);
	if(command(7, 1<<16, Rsp48b|CmdCrc|CmdIdx, resp) < 0)
		return -1;
	if(command(16, 512, Rsp48|CmdCrc|CmdIdx, resp) < 0)
		return -1;
	/*
	 * Transfer clock: cclk_emmc back to 200 MHz, SDHCI divider 4, so
	 * 25 MHz.  A 512-byte block at the 702 Hz identify clock takes ~6 s.
	 */
	if(setclock50m() < 0 && setclock24m() < 0)
		return -1;
	return 0;
}

static void
hexdump(u8 *b, int n)
{
	int i;

	for(i = 0; i < n; i += 16){
		print("%04x: %02x %02x %02x %02x %02x %02x %02x %02x  "
		      "%02x %02x %02x %02x %02x %02x %02x %02x\n", i,
		      b[i], b[i+1], b[i+2], b[i+3], b[i+4], b[i+5], b[i+6], b[i+7],
		      b[i+8], b[i+9], b[i+10], b[i+11], b[i+12], b[i+13],
		      b[i+14], b[i+15]);
	}
}

void
main(int argc, char **argv)
{
	static u8 buf[512], back[512];
	u32 ocr, lba, i, resp4[4];
	u8 *cru;
	int r, off;

	verbose = 1;
	mapdev();
	dumpregs();
	if(argc < 2 || strcmp(argv[1], "probe") == 0){
		if(initcard(&ocr) < 0)
			fail("emmc: card init failed");
		print("emmc: ready ocr %08ux ccs %d\n", ocr, (ocr >> 30) & 1);
		if(readblock(0, buf) < 0)
			fail("emmc: read lba 0 failed");
		print("emmc: lba0 signature %02x%02x\n", buf[510], buf[511]);
		hexdump(buf, 64);
		exits(nil);
	}
	if(strcmp(argv[1], "raw13") == 0){
		/*
		 * No reset, no clock setup, no power: use exactly the controller
		 * state U-Boot left behind and ask the card for its status.
		 */
		cmdwait = 2000;
		cru = mapnode("clock-controller@fdd20000", 0xfdd20000);
		print("emmc: raw13 clock %ux hc2 %ux power %ux hostctl %ux misc %ux\n",
		      rd16(Clock), rd16(Hc2), rd8(Power), rd8(Hostctl), rd32(MiscCon));
		print("emmc: cru sel28 %ux gate9 %ux gate35 %ux sel82 %ux softrst %ux\n",
		      *(volatile u32 *)(cru + 0x170), *(volatile u32 *)(cru + 0x324),
		      *(volatile u32 *)(cru + 0x38c), *(volatile u32 *)(cru + 0x248),
		      *(volatile u32 *)(cru + 0x41c));
		print("emmc: emmccon %ux %ux\n",
		      *(volatile u32 *)(cru + 0x598), *(volatile u32 *)(cru + 0x59c));
		{
			u8 *grf, *pmu;

			grf = mapnode("syscon@fdc60000", 0xfdc60000);
			pmu = mapnode("power-management@fdd90000", 0xfdd90000);
			for(off = 0x80; off < 0xD0; off += 16)
				print("emmc: grf%03x %08ux %08ux %08ux %08ux\n", off,
				      *(volatile u32 *)(grf+off),
				      *(volatile u32 *)(grf+off+4),
				      *(volatile u32 *)(grf+off+8),
				      *(volatile u32 *)(grf+off+12));
			for(off = 0x200; off < 0x270; off += 16)
				print("emmc: drv%03x %08ux %08ux %08ux %08ux\n", off,
				      *(volatile u32 *)(grf+off),
				      *(volatile u32 *)(grf+off+4),
				      *(volatile u32 *)(grf+off+8),
				      *(volatile u32 *)(grf+off+12));
			print("emmc: pmu98 %08ux pmua0 %08ux pmu60 %08ux pmu68 %08ux\n",
			      *(volatile u32 *)(pmu+0x98), *(volatile u32 *)(pmu+0xa0),
			      *(volatile u32 *)(pmu+0x60), *(volatile u32 *)(pmu+0x68));
			{
				u8 *g1, *g2;

				g1 = mapnode("gpio@fe740000", 0xfe740000);
				g2 = mapnode("gpio@fe750000", 0xfe750000);
				print("emmc: gpio1 dr %08ux ddr %08ux gpio2 dr %08ux ddr %08ux\n",
				      *(volatile u32 *)g1, *(volatile u32 *)(g1+8),
				      *(volatile u32 *)g2, *(volatile u32 *)(g2+8));
			}
		}
		print("emmc: pll0 %ux %ux %ux %ux pll40 %ux %ux %ux %ux\n",
		      *(volatile u32 *)(cru + 0x00), *(volatile u32 *)(cru + 0x04),
		      *(volatile u32 *)(cru + 0x08), *(volatile u32 *)(cru + 0x0c),
		      *(volatile u32 *)(cru + 0x40), *(volatile u32 *)(cru + 0x44),
		      *(volatile u32 *)(cru + 0x48), *(volatile u32 *)(cru + 0x4c));
		for(off = 0xE0; off < 0x100; off += 16)
			print("emmc: %03x %08ux %08ux %08ux %08ux\n", off,
			      rd32(off), rd32(off+4), rd32(off+8), rd32(off+12));
		for(off = 0x300; off < 0x340; off += 16)
			print("emmc: %03x %08ux %08ux %08ux %08ux\n", off,
			      rd32(off), rd32(off+4), rd32(off+8), rd32(off+12));
		for(off = 0x500; off < 0x560; off += 16)
			print("emmc: %03x %08ux %08ux %08ux %08ux\n", off,
			      rd32(off), rd32(off+4), rd32(off+8), rd32(off+12));
		for(off = 0x800; off < 0x840; off += 16)
			print("emmc: %03x %08ux %08ux %08ux %08ux\n", off,
			      rd32(off), rd32(off+4), rd32(off+8), rd32(off+12));
		for(i = 0; i < 4; i++){
			r = command(13, i<<16, Rsp48|CmdCrc|CmdIdx, resp4);
			print("emmc: raw13 before rca %d -> %d nis %ux eis %ux r0 %08ux\n",
			      i, r, rd16(Nis), rd16(Eis), resp4[0]);
		}
		r = command(2, 0, Rsp136, resp4);
		print("emmc: raw13 cmd2 -> %d nis %ux eis %ux cid %08ux %08ux\n",
		      r, rd16(Nis), rd16(Eis), resp4[0], resp4[1]);
		r = command(1, 0x40000080, Rsp48, resp4);
		print("emmc: raw13 cmd1 -> %d nis %ux eis %ux ocr %08ux\n",
		      r, rd16(Nis), rd16(Eis), resp4[0]);
		rails_on();
		sleep(500);
		print("emmc: raw13 after rails clock %ux hc2 %ux power %ux\n",
		      rd16(Clock), rd16(Hc2), rd8(Power));
		for(i = 0; i < 4; i++){
			r = command(13, i<<16, Rsp48|CmdCrc|CmdIdx, resp4);
			print("emmc: raw13 after rca %d -> %d nis %ux eis %ux r0 %08ux\n",
			      i, r, rd16(Nis), rd16(Eis), resp4[0]);
		}
		exits(nil);
	}
	if(strcmp(argv[1], "read") == 0){
		if(argc < 3)
			fail("usage: emmc read lba");
		lba = strtoul(argv[2], nil, 0);
		if(initcard(&ocr) < 0)
			fail("emmc: card init failed");
		if(readblock(lba, buf) < 0)
			fail("emmc: read %ud failed", lba);
		print("emmc: lba %ud ok\n", lba);
		hexdump(buf, 128);
		exits(nil);
	}
	if(strcmp(argv[1], "write") == 0){
		if(argc < 3)
			fail("usage: emmc write lba");
		lba = strtoul(argv[2], nil, 0);
		if(initcard(&ocr) < 0)
			fail("emmc: card init failed");
		memset(buf, 0, sizeof buf);
		strcpy((char *)buf, "HARVEY EMMC WRITE TEST");
		buf[32] = lba >> 24; buf[33] = lba >> 16;
		buf[34] = lba >> 8; buf[35] = lba;
		for(i = 64; i < 512; i++)
			buf[i] = i ^ 0x5a;
		if(writeblock(lba, buf) < 0)
			fail("emmc: write %ud failed", lba);
		print("emmc: wrote lba %ud\n", lba);
		if(readblock(lba, back) < 0)
			fail("emmc: readback failed");
		r = memcmp(buf, back, 512);
		print("emmc: readback %s\n", r == 0 ? "MATCH" : "MISMATCH");
		if(r != 0)
			hexdump(back, 64);
		exits(r == 0 ? nil : "mismatch");
	}
	if(strcmp(argv[1], "slow5") == 0){
		/*
		 * Faithful 6.18 identify sequence, matching the Armbian
		 * (mainline 6.18.21) driver:
		 *  - no PMIC writes: the rails are already on from U-Boot
		 *  - MISC_CON (0x81c) stays 0, as in the working kernel
		 *  - HC2 is cleared: U-Boot leaves it at 0x93 (SDR104, tuned)
		 *  - cclk_emmc parent 5 = clk_osc0_div_375k = div_750k / 2
		 *  - power is the regulator path value 0x01 (power on only)
		 *  - a long wait, since a command with response must either
		 *    complete or time out before the next one is issued
		 */
		cmdwait = 3000;
		rails_on();
		resetall();
		wr32(MiscCon, rd32(MiscCon) & ~(1<<1));
		vendor_probe();
		/*
		 * The Armbian 6.18 kernel identifies the card with HC2 = 0x08:
		 * 1.8 V signalling already enabled (vqmmc is vcc_1v8), UHS mode
		 * still 0.  With HC2 = 0 (3.3 V signalling) the card never
		 * answers CMD1 even though the clock is running.
		 */
		cru_emmc_375k();
		/*
		 * sdhci_set_ios() order: power, bus width, then clock.  Writing the
		 * power register after enabling the clock makes the controller drop
		 * SDHCI_CLOCK_CARD_EN (bit 2), leaving the card with no clock at all.
		 */
		wr8(Power, 0);
		wr8(Power, 1);
		setwidth(1);
		/* The power write clears the 1.8 V signalling bit, so set it after. */
		wr16(Hc2, 1<<3);
		phyinit();		/* dwcmshc_phy_init(), write-only PHY regs */
		if(setclockbase() < 0)
			fail("emmc: clock failed");
		sleep(2000);
		if(verbose)
			print("emmc: hc2 %ux power %ux clock %ux misc %ux\n",
			      rd16(Hc2), rd8(Power), rd16(Clock), rd32(MiscCon));
		r = command(0, 0, 0, nil);
		print("emmc: cmd0 %d nis %ux present %ux\n", r, rd16(Nis),
		      rd32(Present));
		for(i = 0; i < 3; i++){
			r = command(1, (1u<<30) | (1u<<7), Rsp48, resp4);
			print("emmc: cmd1 %d nis %ux eis %ux ocr %08ux present %ux\n",
			      r, rd16(Nis), rd16(Eis), resp4[0], rd32(Present));
			if(r == 0)
				break;
			sleep(100);
		}
		if(r < 0)
			fail("emmc: no response");
		print("emmc: READY ocr %08ux ccs %d\n", resp4[0], (resp4[0]>>30)&1);
		exits(nil);
	}
	if(strcmp(argv[1], "slow8") == 0){
		/* Full rail power-cycle, then the faithful 6.18 identify. */
		cmdwait = 3000;
		print("emmc: rails off\n");
		rails_set(0x60);		/* disable DCDC5 + SWITCH1 (write masks) */
		sleep(300);
		print("emmc: rails on\n");
		rails_set(0x66);
		sleep(200);
		{
			u8 *grf;

			grf = mapnode("syscon@fdc60000", 0xfdc60000);
			print("emmc: grf20 before %ux\n",
			      *(volatile u32 *)(grf + 0x20));
			*(volatile u32 *)(grf + 0x20) = (0xffffu<<16) | 0x1111;
			*(volatile u32 *)(grf + 0x24) = (0xffffu<<16) | 0x1111;
			*(volatile u32 *)(grf + 0x28) = (0xffffu<<16) | 0x2001;
			*(volatile u32 *)(grf + 0x2c) = (0xffffu<<16) | 0x0222;
			__asm__ volatile("dsb sy" ::: "memory");
			sleep(50);
			print("emmc: grf20 after %ux\n",
			      *(volatile u32 *)(grf + 0x20));
		}
		resetall();
		wr32(MiscCon, rd32(MiscCon) & ~(1<<1));
		vendor_probe();
		cru_emmc_375k();
		wr8(Power, 0);
		wr8(Power, 1);
		setwidth(1);
		wr16(Hc2, 1<<3);
		phyinit();
		if(setclockbase() < 0)
			fail("emmc: clock failed");
		sleep(1000);
		print("emmc: state clock %ux hc2 %ux power %ux\n",
		      rd16(Clock), rd16(Hc2), rd8(Power));
		r = command(0, 0, 0, nil);
		print("emmc: cmd0 %d nis %ux present %ux\n", r, rd16(Nis),
		      rd32(Present));
		r = command(2, 0, Rsp136, resp4);
		print("emmc: cmd2 %d nis %ux eis %ux cid %08ux %08ux %08ux %08ux\n",
		      r, rd16(Nis), rd16(Eis), resp4[0], resp4[1], resp4[2],
		      resp4[3]);
		{
			static u32 ocrs[] = {
				0x40000080, 0x40ff8000, 0x40300000, 0x40ff8080,
				0x00000080, 0xc0ff8080
			};
			int oi;
			for(oi = 0; oi < sizeof ocrs/sizeof ocrs[0]; oi++){
				r = command(1, ocrs[oi], Rsp48, resp4);
				print("emmc: cmd1 %08ux -> %d nis %ux eis %ux ocr %08ux\n",
				      ocrs[oi], r, rd16(Nis), rd16(Eis), resp4[0]);
				if(r == 0)
					break;
			}
		}
		if(r < 0)
			fail("emmc: no response");
		print("emmc: READY ocr %08ux ccs %d\n", resp4[0], (resp4[0]>>30)&1);
		exits(nil);
	}
	if(strcmp(argv[1], "slow6") == 0){
		/*
		 * Probe the CMD/response path before CMD0.  U-Boot left the card
		 * in transfer state with an RCA, so CMD13 (SEND_STATUS) should
		 * answer.  If it does not, the card never sees our commands; if
		 * it does, then CMD0 is not taking effect and CMD1 is illegal.
		 */
		cmdwait = 3000;
		resetall();
		wr32(MiscCon, rd32(MiscCon) & ~(1<<1));
		vendor_probe();
		cru_emmc_375k();
		wr8(Power, 0);
		wr8(Power, 1);
		setwidth(1);
		wr16(Hc2, 1<<3);
		if(setclockbase() < 0)
			fail("emmc: clock failed");
		sleep(100);
		print("emmc: state clock %ux hc2 %ux power %ux\n",
		      rd16(Clock), rd16(Hc2), rd8(Power));
		for(i = 0; i < 16; i++){
			r = command(13, i<<16, Rsp48|CmdCrc|CmdIdx, resp4);
			print("emmc: cmd13 rca %d -> %d nis %ux eis %ux r0 %08ux\n",
			      i, r, rd16(Nis), rd16(Eis), resp4[0]);
		}
		exits(nil);
	}
	if(strcmp(argv[1], "slow3") == 0){
		/* The creator's board DTS gives the eMMC no vmmc/vqmmc and no
		 * mmc-hs200-1_8v, so there is no 1.8 V switch: the card is
		 * identified with 3.3 V signalling. */
		rails_on();
		resetall();
		vendor_probe();
		wr16(Hc2, rd16(Hc2) & ~(1<<3));		/* 3.3 V signalling */
		if(verbose)
			print("emmc: hc2 %ux\n", rd16(Hc2));
		cru_emmc_200m();
		if(clock200() < 0)
			fail("emmc: 200 MHz DLL init failed");
		cru_emmc_375k();
		if(setclockbase() < 0)
			fail("emmc: clock failed");
		setpower(0x0f);
		setwidth(1);
		sleep(20);
		r = command(0, 0, 0, nil);
		print("emmc: cmd0 %d nis %ux present %ux\n", r, rd16(Nis),
		      rd32(Present));
		for(i = 0; i < 5; i++){
			r = command(1, 0x40ff8000, Rsp48, resp4);
			print("emmc: cmd1 %d nis %ux eis %ux ocr %08ux\n",
			      r, rd16(Nis), rd16(Eis), resp4[0]);
			if(r == 0)
				break;
			sleep(20);
		}
		if(r < 0)
			fail("emmc: no response");
		print("emmc: READY ocr %08ux ccs %d\n", resp4[0], (resp4[0]>>30)&1);
		exits(nil);
	}
	if(strcmp(argv[1], "slow") == 0){
		rails_on();
		resetall();
		vendor_probe();
		setvoltage180();
		phyinit();		/* voltage_switch -> dwcmshc_phy_init() */
		/* sdhci_setup_host(): initialise the PHY at max clock. */
		cru_emmc_200m();
		if(clock200() < 0)
			fail("emmc: 200 MHz DLL init failed");
		cru_emmc_375k();
		if(setclockbase() < 0)
			fail("emmc: clock failed");
		setpower(0x0f);
		setwidth(1);
		sleep(20);
		r = command(0, 0, 0, nil);
		print("emmc: cmd0 %d nis %ux present %ux\n", r, rd16(Nis),
		      rd32(Present));
		for(i = 0; i < 5; i++){
			r = command(1, (1u<<30) | (1u<<7), Rsp48, resp4);
			print("emmc: cmd1 %d nis %ux eis %ux ocr %08ux\n",
			      r, rd16(Nis), rd16(Eis), resp4[0]);
			if(r == 0)
				break;
			sleep(20);
		}
		if(r < 0)
			fail("emmc: no response");
		print("emmc: READY ocr %08ux ccs %d\n", resp4[0], (resp4[0]>>30)&1);
		exits(nil);
	}
	if(strcmp(argv[1], "rails") == 0){
		char ctl[64], dat[64], cmd[32], h[64];
		int fd, m, j;
		u8 v;

		if(access("/mnt/i2c", AEXIST) < 0)
			create("/mnt/i2c", OREAD, DMDIR|0755);
		fd = open("/srv/i2c", OREAD);
		if(fd < 0 || mount(fd, -1, "/mnt/i2c", MREPL|MCREATE, "") < 0)
			fail("emmc: cannot mount i2c");
		close(fd);
		snprint(ctl, sizeof ctl, "/mnt/i2c/i2c0/ctl");
		snprint(dat, sizeof dat, "/mnt/i2c/i2c0/data");
		/* RK809 POWER_EN0..3: DCDC1-4 at 0xb1, DCDC5 at 0xb4 bit 1
		 * with write mask bit 5 (ENABLE_MASK(1) = BIT(1)|BIT(5)). */
		for(j = 0; j < 4; j++){
			fd = open(ctl, ORDWR);
			snprint(cmd, sizeof cmd, "r 32 %d 1", 0xb1 + j);
			m = fd < 0 ? -1 : write(fd, cmd, strlen(cmd));
			if(fd >= 0)
				close(fd);
			if(m != strlen(cmd)){
				print("emmc: power_en%d read failed: %r\n", j);
				continue;
			}
			fd = open(dat, OREAD);
			m = fd < 0 ? -1 : read(fd, h, sizeof h - 1);
			if(fd >= 0)
				close(fd);
			if(m <= 0){
				print("emmc: power_en%d data failed\n", j);
				continue;
			}
			h[m] = 0;
			print("emmc: power_en%d (%#x) = 0x%s\n", j, 0xb1 + j, h);
		}
		/* enable DCDC5 (vcc_1v8, the eMMC vqmmc) */
		fd = open(ctl, ORDWR);
		write(fd, "w 32 180 34", 11);
		close(fd);
		sleep(30);
		fd = open(ctl, ORDWR);
		write(fd, "r 32 180 1", 10);
		close(fd);
		fd = open(dat, OREAD);
		m = read(fd, h, sizeof h - 1);
		close(fd);
		h[m > 0 ? m : 0] = 0;
		v = strtoul(h, nil, 16);
		print("emmc: power_en3 now 0x%02ux (DCDC5 %s)\n", v,
		      (v & 2) ? "on" : "off");
		sleep(30);
		if(initcard(&ocr) < 0)
			fail("emmc: card init failed");
		print("emmc: READY ocr %08ux ccs %d\n", ocr, (ocr>>30)&1);
		if(readblock(0, buf) < 0)
			fail("emmc: read lba0 failed");
		print("emmc: lba0 %02x%02x %02x%02x\n", buf[510], buf[511], buf[0], buf[1]);
		hexdump(buf, 32);
		exits(nil);
	}
	if(strcmp(argv[1], "pmicfix") == 0){
		char ctl[64], dat[64], cmd[32], h[64];
		int fd, m;
		u8 v;

		if(access("/mnt/i2c", AEXIST) < 0)
			create("/mnt/i2c", OREAD, DMDIR|0755);
		fd = open("/srv/i2c", OREAD);
		if(fd < 0 || mount(fd, -1, "/mnt/i2c", MREPL|MCREATE, "") < 0)
			fail("emmc: cannot mount i2c");
		close(fd);
		snprint(ctl, sizeof ctl, "/mnt/i2c/i2c0/ctl");
		snprint(dat, sizeof dat, "/mnt/i2c/i2c0/data");
		fd = open(ctl, ORDWR);
		snprint(cmd, sizeof cmd, "r 32 177 1");
		write(fd, cmd, strlen(cmd));
		close(fd);
		fd = open(dat, OREAD);
		m = read(fd, h, sizeof h - 1);
		close(fd);
		if(m < 2)
			fail("emmc: pmic read failed");
		h[m] = 0;
		v = strtoul(h, nil, 16);
		print("emmc: power_en0 %02ux\n", v);
		v |= 1<<4;			/* DCDC5 = vcc_1v8 */
		fd = open(ctl, ORDWR);
		snprint(cmd, sizeof cmd, "w 32 177 %d", v);
		write(fd, cmd, strlen(cmd));
		close(fd);
		sleep(20);
		fd = open(ctl, ORDWR);
		write(fd, "r 32 177 1", 10);
		close(fd);
		fd = open(dat, OREAD);
		m = read(fd, h, sizeof h - 1);
		close(fd);
		if(m > 0){
			h[m] = 0;
			print("emmc: power_en0 now 0x%s\n", h);
		}
		if(initcard(&ocr) < 0)
			fail("emmc: card init failed after pmic fix");
		print("emmc: READY ocr %08ux\n", ocr);
		if(readblock(0, buf) < 0)
			fail("emmc: read failed");
		print("emmc: lba0 sig %02x%02x\n", buf[510], buf[511]);
		exits(nil);
	}
	if(strcmp(argv[1], "pmic") == 0){
		/* Dump RK809 POWER_EN (0xb1..0xb5) over the i2c server so the
		 * rail enables can be compared with the bootloader's. */
		char ctl[64], dat[64], cmd[32], h[64];
		int fd, bus, m, j;

		if(access("/mnt/i2c", AEXIST) < 0)
			create("/mnt/i2c", OREAD, DMDIR|0755);
		fd = open("/srv/i2c", OREAD);
		if(fd < 0 || mount(fd, -1, "/mnt/i2c", MREPL|MCREATE, "") < 0)
			fail("emmc: cannot mount i2c");
		close(fd);
		bus = 0;	/* fdd40000 is the first controller discovered */
		snprint(ctl, sizeof ctl, "/mnt/i2c/i2c%d/ctl", bus);
		snprint(dat, sizeof dat, "/mnt/i2c/i2c%d/data", bus);
		fd = open(ctl, ORDWR);
		if(fd < 0)
			fail("emmc: no pmic bus");
		close(fd);
		fd = open(dat, OREAD);
		if(fd < 0)
			fail("emmc: no pmic data");
		close(fd);
		for(j = 0; j < 5; j++){
			fd = open(ctl, ORDWR);
			snprint(cmd, sizeof cmd, "r 32 %d 1", 0xb1 + j);
			if(write(fd, cmd, strlen(cmd)) != strlen(cmd))
				fail("emmc: i2c write failed");
			close(fd);
			fd = open(dat, OREAD);
			m = read(fd, h, sizeof h - 1);
			close(fd);
			if(m < 2)
				fail("emmc: i2c read failed");
			h[m] = 0;
			print("pmic: 0x%x = 0x%s\n", 0xb1 + j, h);
		}
		exits(nil);
	}
	if(strcmp(argv[1], "pmugrf") == 0){
		u8 *g;
		int off;

		g = mapnode("syscon@fdc20000", 0xfdc20000);
		for(off = 0x120; off < 0x160; off += 16)
			print("%06x: %08ux %08ux %08ux %08ux\n",
			      0xfdc20000 + off,
			      *(volatile u32 *)(g + off),
			      *(volatile u32 *)(g + off + 4),
			      *(volatile u32 *)(g + off + 8),
			      *(volatile u32 *)(g + off + 12));
		exits(nil);
	}
	if(strcmp(argv[1], "grf") == 0){
		/* Dump the main GRF so the pinmux can be diffed against the
		 * bootloader's (md 0xfdc60000 in U-Boot). */
		u8 *grf;
		int off;

		grf = mapnode("syscon@fdc60000", 0xfdc60000);
		for(off = 0; off < 0x60; off += 16)
			print("%06x: %08ux %08ux %08ux %08ux\n",
			      0xfdc60000 + off,
			      *(volatile u32 *)(grf + off),
			      *(volatile u32 *)(grf + off + 4),
			      *(volatile u32 *)(grf + off + 8),
			      *(volatile u32 *)(grf + off + 12));
		exits(nil);
	}
	if(strcmp(argv[1], "rca") == 0){
		/* Sweep every relative card address before disturbing anything.
		 * If the card is still selected from the bootloader this finds it. */
		int found = 0;

		mapdev();
		dumpregs();
		verbose = 0;
		for(i = 1; i <= 255; i++){
			r = command(13, i<<16, Rsp48|CmdCrc|CmdIdx, resp4);
			if(r == 0){
				print("emmc: RCA %d answers, status %08ux\n", i, resp4[0]);
				found++;
			}
			wr8(Swreset, 2);	/* reset the command line after a hang */
			sleep(1);
			wr16(Nis, 0xffff);
		}
		verbose = 1;
		print("emmc: rca sweep done, %d found\n", found);
		exits(nil);
	}
	if(strcmp(argv[1], "matrix") == 0){
		static struct { int io180; u8 pwr; char *name; } t[] = {
			{ 1, 0x0f, "io1.8 vcc3.3" },
			{ 0, 0x0f, "io3.3 vcc3.3" },
			{ 1, 0x0b, "io1.8 vcc1.8" },
			{ 0, 0x0b, "io3.3 vcc1.8" },
		};
		int k;

		mapdev();
		for(k = 0; k < nelem(t); k++){
			resetall();
			vendor_probe();
			if(t[k].io180)
				wr16(Hc2, rd16(Hc2) | (1<<3));
			else
				wr16(Hc2, rd16(Hc2) & ~(1<<3));
			sleep(5);
			setclock(400000);
			setpower(t[k].pwr);
			setwidth(1);
			sleep(20);
			r = command(0, 0, 0, nil);
			print("emmc: %s cmd0 %d nis %ux present %ux\n",
			      t[k].name, r, rd16(Nis), rd32(Present));
			r = command(1, (1u<<30) | (1u<<7), Rsp48, resp4);
			print("emmc: %s cmd1 %d nis %ux eis %ux ocr %08ux present %ux\n",
			      t[k].name, r, rd16(Nis), rd16(Eis), resp4[0],
			      rd32(Present));
			if(r == 0 && (resp4[0] & (1u<<31)))
				print("emmc: %s READY\n", t[k].name);
		}
		exits(nil);
	}
	if(strcmp(argv[1], "pmu") == 0){
		u8 *pmu;
		u32 v;

		mapdev();
		pmu = mapnode("power-management@fdd90000", 0xfdd90000);
		v = *(volatile u32 *)(pmu + 0xa0);
		print("emmc: pmu a0 %ux a4 %ux\n", v,
		      *(volatile u32 *)(pmu + 0xa4));
		/* Try PD_PIPE on with bit 8 asserted, then with it cleared. */
		*(volatile u32 *)(pmu + 0xa0) = (1u<<8)<<16 | (1u<<8);
		__asm__ volatile("dsb sy" ::: "memory");
		sleep(20);
		print("emmc: pmu a0 set %ux a4 %ux\n",
		      *(volatile u32 *)(pmu + 0xa0),
		      *(volatile u32 *)(pmu + 0xa4));
		command(0, 0, 0, nil);
		r = command(1, (1u<<30) | (1u<<7), Rsp48, resp4);
		print("emmc: cmd1 (bit8=1) -> %d nis %ux eis %ux ocr %08ux\n",
		      r, rd16(Nis), rd16(Eis), resp4[0]);
		*(volatile u32 *)(pmu + 0xa0) = (1u<<8)<<16 | 0;
		__asm__ volatile("dsb sy" ::: "memory");
		sleep(20);
		print("emmc: pmu a0 clr %ux a4 %ux\n",
		      *(volatile u32 *)(pmu + 0xa0),
		      *(volatile u32 *)(pmu + 0xa4));
		command(0, 0, 0, nil);
		r = command(1, (1u<<30) | (1u<<7), Rsp48, resp4);
		print("emmc: cmd1 (bit8=0) -> %d nis %ux eis %ux ocr %08ux\n",
		      r, rd16(Nis), rd16(Eis), resp4[0]);
		exits(nil);
	}
	if(strcmp(argv[1], "fast") == 0){
		/* Leave U-Boot's clock and DLL exactly as they are and just
		 * send broadcast commands: does the card answer at all? */
		mapdev();
		dumpregs();
		/* Probe the relative card address first: if the card is still
		 * in transfer state this answers and tells us its RCA. */
		for(i = 1; i <= 4; i++){
			r = command(13, i<<16, Rsp48|CmdCrc|CmdIdx, resp4);
			print("emmc: status rca %ud -> %d nis %ux eis %ux resp %08ux\n",
			      i, r, rd16(Nis), rd16(Eis), resp4[0]);
			sleep(5);
		}
		r = command(0, 0, 0, nil);
		print("emmc: cmd0 -> %d nis %ux eis %ux\n", r, rd16(Nis), rd16(Eis));
		for(i = 0; i < 3; i++){
			r = command(1, (1u<<30) | (1u<<7), Rsp48, resp4);
			print("emmc: cmd1 -> %d nis %ux eis %ux ocr %08ux\n",
			      r, rd16(Nis), rd16(Eis), resp4[0]);
			sleep(10);
		}
		exits(nil);
	}
	if(strcmp(argv[1], "uboot") == 0){
		/* Use whatever state the bootloader left: no reset, no clock
		 * or vendor reconfiguration.  Just try to talk to the card. */
		mapdev();
		dumpregs();
		print("emmc: hostctl %ux hc2 %ux power %ux clock %ux\n",
		      rd8(Hostctl), rd16(Hc2), rd8(Power), rd16(Clock));
		r = command(13, 1<<16, Rsp48|CmdCrc|CmdIdx, resp4);
		print("emmc: cmd13 -> %d nis %ux eis %ux present %ux resp %08ux\n",
		      r, rd16(Nis), rd16(Eis), rd32(Present), resp4[0]);
		if(readblock(0, buf) < 0)
			print("emmc: read 0 failed nis %ux eis %ux\n",
			      rd16(Nis), rd16(Eis));
		else{
			print("emmc: lba0 %02x%02x %02x%02x\n",
			      buf[510], buf[511], buf[0], buf[1]);
			hexdump(buf, 64);
		}
		exits(nil);
	}
	if(strcmp(argv[1], "raw") == 0){
		static struct { int idx; u32 arg; u32 flags; char *name; } t[] = {
			{ 0, 0, 0, "cmd0 idle" },
			{ 1, 0x40ff8000, Rsp48, "cmd1 opcond" },
			{ 1, 0x40ff8000, Rsp48, "cmd1 again" },
			{ 2, 0, Rsp136|CmdCrc, "cmd2 cid" },
			{ 8, 0x1aa, Rsp48|CmdCrc|CmdIdx, "cmd8 ifcond" },
			{ 55, 0, Rsp48|CmdCrc|CmdIdx, "cmd55 app" },
			{ 13, 1<<16, Rsp48|CmdCrc|CmdIdx, "cmd13 status" },
		};
		int i;

		if(reset() < 0)
			fail("emmc: reset failed");
		vendorinit();
		wr8(Timeout, 0x0e);
		wr8(Power, 0x0f);
		sleep(100);
		if(setclock(400000) < 0)
			fail("emmc: clock failed");
		sleep(50);
		for(i = 0; i < nelem(t); i++){
			memset(buf, 0, sizeof buf);
			r = command(t[i].idx, t[i].arg, t[i].flags, resp4);
			print("emmc: %-14s -> %d nis %ux eis %ux resp %08ux %08ux %08ux %08ux\n",
			      t[i].name, r, rd16(Nis), rd16(Eis),
			      resp4[0], resp4[1], resp4[2], resp4[3]);
			sleep(20);
		}
		exits(nil);
	}
	fail("usage: emmc probe|read lba|write lba");
}
