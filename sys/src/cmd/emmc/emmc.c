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

/* Command register flags. */
enum {
	CmdData	= 1<<6,
	CmdIdx	= 1<<7,
	CmdCrc	= 1<<8,
	Rsp136	= 1<<9,
	Rsp48	= 2<<9,
	Rsp48b	= 3<<9,
};

static u8 *mmio;
static int verbose;

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
	MiscCon		= 0x81C,
	VendorArea1	= 0xE8,
	HostCtrl3	= 0x08,
};

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

	wr16(Nis, 0xffff);
	wr16(Eis, 0xffff);
	wr32(Argument, arg);
	wr16(Cmdreg, idx | flags);
	for(i = 0; i < 500; i++){
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
	if(i == 500){
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
		for(i = 0; i < 2000; i++){
			st = rd16(Nis);
			if(st & IntErr)
				return -1;
			if(st & IntBwr)
				break;
			sleep(1);
		}
		if(i == 2000)
			return -1;
		for(got = 0; got < n; got += 4)
			wr32(Buffer, *(u32 *)(buf + got));
		wr16(Nis, IntBwr);
	}else{
		for(got = 0; got < n; ){
			for(i = 0; i < 2000; i++){
				st = rd16(Nis);
				if(st & IntErr)
					return -1;
				if(st & IntBrr)
					break;
				sleep(1);
			}
			if(i == 2000)
				return -1;
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
	for(i = 0; i < 2000; i++){
		st = rd16(Nis);
		if(st & IntErr)
			return -1;
		if(st & IntTc){
			wr16(Nis, st);
			return 0;
		}
		sleep(1);
	}
	return -1;
}

static int
readblock(u32 lba, u8 *buf)
{
	wr16(Blksize, 512);
	wr16(Blkcount, 1);
	wr16(Xfer, 0x08 | 0x02);	/* read, block count enable */
	if(command(17, lba, CmdData|Rsp48|CmdCrc|CmdIdx, nil) < 0)
		return -1;
	return dataio(buf, 512, 0);
}

static int
writeblock(u32 lba, u8 *buf)
{
	wr16(Blksize, 512);
	wr16(Blkcount, 1);
	wr16(Xfer, 0x02);		/* write, block count enable */
	if(command(24, lba, CmdData|Rsp48|CmdCrc|CmdIdx, nil) < 0)
		return -1;
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
	u32 extra;
	int i;

	extra = rd32(MiscCon);
	wr8(Swreset, 4);
	for(i = 0; i < 100; i++){
		if((rd8(Swreset) & 4) == 0)
			break;
		sleep(1);
	}
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
	setvoltage180();
	if(setclock(400000) < 0)
		return -1;
	setpower(0x0f);			/* 3.3 V, SDHCI_POWER_ON */
	setwidth(1);
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
	int r;

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
