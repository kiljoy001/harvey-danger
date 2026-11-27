/*
 * emmclog: an out-of-band boot log on the eMMC itself.
 *
 * The LKL guests own every interesting driver on bengal, so when a
 * guest wedges the SoC there is nobody left to tell us why.  This
 * server is the opposite: a tiny native SDHCI driver (standard
 * register set, PIO, low clock, no DMA, no clocks or regulators of
 * its own — the bootloader leaves the controller powered, and this
 * runs before any LKL family starts) that keeps a circular text log
 * in the unused blocks between the GPT and the first partition.
 *
 *   /log    write appends; read returns the stored log
 *   /stat   controller, capacity, log area
 *   /ctl    write "clear" to reset the log
 *
 * While this runs, no LKL family may probe the same controller: the
 * sdhci node is removed from the bengal_usb family for exactly this
 * reason.  It is a troubleshooting instrument, not the storage plan.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"

/* SDHCI standard registers (byte offsets) */
enum {
	Rarg		= 0x08,
	Rxfermode	= 0x0c,
	Rcmd		= 0x0e,
	Rresp		= 0x10,
	Rdata		= 0x20,
	Rpresent	= 0x24,
	Rhostctl	= 0x28,
	Rpower		= 0x29,
	Rclock		= 0x2c,
	Rtimeout	= 0x2e,
	Rreset		= 0x2f,
	Rintsts		= 0x30,
	Rerrsts		= 0x32,
	Rcaps		= 0x40,

	PresentCmdInhibit	= 1 << 0,
	PresentDatInhibit	= 1 << 1,
	PresentBufWrite		= 1 << 10,
	PresentBufRead		= 1 << 11,

	IntCmdDone	= 1 << 0,
	IntXferDone	= 1 << 1,
	IntBufWrite	= 1 << 4,
	IntBufRead	= 1 << 5,
	IntError	= 1 << 15,

	BlkSize		= 512,
};

enum {
	Qlog, Qstat, Qctl,
};

static Drv d;
static Drv gcc;
static QLock cardlk;
static u32 rca;
static u64 nsectors;
static u32 logbase;	/* first LBA of the log area */
static u32 logblocks;	/* blocks in the area, header included */
static u32 logused;	/* bytes of log data after the header block */
static char *logbuf;	/* RAM copy, write-through to the card */
static int logready;

static u8 *
regs(void)
{
	return d.regs;
}

static u32
rd32(u32 off)
{
	return *(volatile u32 *)(regs() + off);
}

static void
wr32(u32 off, u32 v)
{
	*(volatile u32 *)(regs() + off) = v;
}

static u16
rd16(u32 off)
{
	return *(volatile u16 *)(regs() + off);
}

static void
wr16(u32 off, u16 v)
{
	*(volatile u16 *)(regs() + off) = v;
}

static u8
rd8(u32 off)
{
	return *(volatile u8 *)(regs() + off);
}

static void
wr8(u32 off, u8 v)
{
	*(volatile u8 *)(regs() + off) = v;
}

/* Poll a status bit with a millisecond budget; 0 ok, -1 timeout. */
static int
waitbits(u32 off, u32 mask, u32 want, int ms)
{
	while(ms-- > 0){
		if((rd32(off) & mask) == want)
			return 0;
		sleep(1);
	}
	return -1;
}

/*
 * One command, optionally with one 512-byte data phase (PIO).
 * resp points at 4 words for R2, 1 word otherwise; nil if unwanted.
 * datard selects the data direction when data is present.
 */
static int
docmd(u32 idx, u32 arg, u32 *resp, void *data, int datard)
{
	u16 cmd, xfer;
	int i, n;

	if(waitbits(Rpresent, PresentCmdInhibit | PresentDatInhibit, 0,
	    500) < 0)
		return -1;
	wr32(Rintsts, 0xffffffff);
	wr32(Rresp, 0);	/* keep valgrind of registers honest */
	wr32(Rarg, arg);
	if(data != nil){
		wr16(0x04, BlkSize);	/* block size */
		wr16(0x06, 1);		/* block count */
		xfer = (1 << 1);	/* block count enable */
		if(datard)
			xfer |= (1 << 4);	/* read direction */
		wr16(Rxfermode, xfer);
	}
	switch(idx){
	case 0: cmd = 0; break;			/* no response */
	case 2: case 9: cmd = (idx << 8) | 0x09; break;	/* R2 */
	case 1: cmd = (idx << 8) | 0x02; break;	/* R3: no crc/idx */
	case 7: case 12: cmd = (idx << 8) | 0x0f; break;	/* R1b */
	default: cmd = (idx << 8) | 0x0e; break;	/* R1 */
	}
	if(data != nil)
		cmd |= 0x20;	/* data present (bit 5) */
	wr16(Rcmd, cmd);
	if(waitbits(Rintsts, IntError | IntCmdDone, IntCmdDone, 1000) < 0)
		return -1;
	if(resp != nil){
		resp[0] = rd32(Rresp);
		resp[1] = rd32(Rresp + 4);
		resp[2] = rd32(Rresp + 8);
		resp[3] = rd32(Rresp + 12);
	}
	if(data == nil)
		return 0;
	n = BlkSize / 4;
	if(datard){
		if(waitbits(Rintsts, IntError | IntBufRead, IntBufRead,
		    1000) < 0)
			return -1;
		for(i = 0; i < n; i++)
			((u32 *)data)[i] = rd32(Rdata);
	}else{
		if(waitbits(Rintsts, IntError | IntBufWrite, IntBufWrite,
		    1000) < 0)
			return -1;
		for(i = 0; i < n; i++)
			wr32(Rdata, ((u32 *)data)[i]);
	}
	if(waitbits(Rintsts, IntError | IntXferDone, IntXferDone,
	    2000) < 0)
		return -1;
	return 0;
}

static int
cardread(u32 lba, void *buf)
{
	return docmd(17, lba, nil, buf, 1);
}

static int
cardwrite(u32 lba, void *buf)
{
	return docmd(24, lba, nil, buf, 0);
}

/*
 * The SDHCI register block bus-errors while its clocks are gated,
 * and the bootloader does not leave them on.  Turn the SDCC1 branch
 * clocks on at the GCC (offsets from the sm6115 clock driver) and
 * confirm they are running before touching the controller; failing
 * here is clean, faulting on a gated register block is not.
 */
static char clockdbg[128];

static int
clocks_on(void)
{
	enum {
		AhbCbcr = 0x38008, AppsCbcr = 0x38004,
		RcgCmd = 0x38028, RcgCfg = 0x3802c,
		CmdUpdate = 1 << 0, CmdRootEn = 1 << 1,
		CmdRootOff = 1u << 31, ClkOff = 1u << 31,
	};
	u32 *ahb, *apps, *cmd, *cfg;
	int i;

	if(drvfindreg(&gcc, 0x1400000ULL) < 0 || drvmap(&gcc) < 0)
		return -1;
	ahb = (u32 *)(gcc.regs + AhbCbcr);
	apps = (u32 *)(gcc.regs + AppsCbcr);
	cmd = (u32 *)(gcc.regs + RcgCmd);
	cfg = (u32 *)(gcc.regs + RcgCfg);
	/*
	 * Program a known clock first: BI_TCXO (always running)
	 * through the HID divider only, ~2 MHz, no PLL and no MND
	 * involved.  Then root on, then the two branch clocks.
	 * The guest does the equivalent at probe (clk_set_rate +
	 * enable); merely ungating a parked RCG leaves the block
	 * without edges and its registers bus-error.
	 */
	*cfg = (*cfg & ~0x3f1f) | 5;	/* src div 5, source BI_TCXO */
	*cmd |= CmdRootEn | CmdUpdate;
	for(i = 0; i < 100 && (*cmd & CmdUpdate) != 0; i++)
		sleep(1);
	for(i = 0; i < 100 && (*cmd & CmdRootOff) != 0; i++)
		sleep(1);
	*ahb |= 1;
	*apps |= 1;
	for(i = 0; i < 100; i++){
		if(((*ahb & ClkOff) == 0) && ((*apps & ClkOff) == 0))
			break;
		sleep(1);
	}
	snprint(clockdbg, sizeof clockdbg,
		"gcc cmd %#ux cfg %#ux ahb %#ux apps %#ux",
		*cmd, *cfg, *ahb, *apps);
	if((*cmd & CmdRootOff) != 0 || (*ahb & ClkOff) != 0 ||
	    (*apps & ClkOff) != 0)
		return -1;
	return 0;
}

static int
setclock(u32 div)
{
	u16 v;

	/* SDCLK frequency select is a 10-bit divider field:
	 * bits 15:8 hold the low byte, bits 7:6 the high bits. */
	v = (u16)(((div & 0xff) << 8) | ((div >> 8) << 6) | 0x01);
	wr16(Rclock, v);
	if(waitbits(Rclock, 0x02, 0x02, 100) < 0)
		return -1;
	wr16(Rclock, v | 0x04);	/* SD clock enable */
	return 0;
}

static int
cardinit(void)
{
	u32 resp[4];
	u32 basekhz, div, iddiv;
	int i;
	u8 extcsd[BlkSize];

	/* Software-reset the host, leaving the card as the bootloader
	 * left it until CMD0 below. */
	wr8(Rreset, 0x01);
	{
		/* Rreset is a byte register: a word read at 0x2f is
		 * unaligned and this bus aborts on those. */
		int i;

		for(i = 0; i < 100 && (rd8(Rreset) & 0x01) != 0; i++)
			sleep(1);
		if(i >= 100)
			return -1;
	}
	/* Identification (CMD0..CMD7) must run at no more than
	 * 400 kHz.  The faster divider is only selected after the
	 * card has been selected with CMD7. */
	basekhz = ((rd32(Rcaps) >> 8) & 0xff) * 1000;
	if(basekhz == 0)
		basekhz = 200000;
	iddiv = 1;
	while(basekhz / (2 * iddiv) > 400 && iddiv < 512)
		iddiv <<= 1;
	div = 1;
	while(basekhz / (2 * div) > 3000 && div < 256)
		div <<= 1;
	if(setclock(iddiv) < 0)
		return -1;
	wr8(Rtimeout, 0x0e);
	wr8(Rpower, 0x0f);	/* bus power on, 3.3 V (as left) */
	wr8(Rhostctl, 0);	/* 1-bit, no DMA */
	sleep(10);

	if(docmd(0, 0, nil, nil, 0) < 0)
		return -1;
	sleep(2);
	/* MMC SEND_OP_COND until ready, sector addressing. */
	for(i = 0; i < 200; i++){
		if(docmd(1, 0x40300000, resp, nil, 0) < 0)
			return -1;
		if(resp[0] & 0x80000000)
			break;
		sleep(10);
	}
	if(i >= 200)
		return -1;
	if(docmd(2, 0, resp, nil, 0) < 0)	/* CID */
		return -1;
	rca = 1;
	if(docmd(3, rca << 16, resp, nil, 0) < 0)
		return -1;
	if(docmd(9, rca << 16, resp, nil, 0) < 0)	/* CSD */
		return -1;
	if(docmd(7, rca << 16, resp, nil, 0) < 0)	/* select */
		return -1;
	if(setclock(div) < 0)
		return -1;
	if(docmd(16, BlkSize, resp, nil, 0) < 0)
		return -1;
	/* EXT_CSD for the sector count (offset 212, little endian). */
	memset(extcsd, 0, sizeof extcsd);
	if(docmd(8, 0, resp, extcsd, 1) < 0)
		return -1;
	nsectors = (u32)extcsd[212] | (u32)extcsd[213] << 8 |
		(u32)extcsd[214] << 16 | (u32)extcsd[215] << 24;
	if(nsectors == 0)
		return -1;
	return 0;
}

static u64
get64(u8 *p)
{
	u64 v = 0;
	int i;

	for(i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static u32
get32le(u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 |
		(u32)p[3] << 24;
}

/*
 * Find the log area: unused blocks between the end of the GPT
 * entries and the first partition.  At least 256 blocks must be
 * free, and the first area block must be empty or already carry
 * our header — anything else means the gap is not ours to use.
 */
enum { Logmagic = 0x31474f4c };	/* "LOG1" */

static int
logareainit(void)
{
	u8 buf[BlkSize];
	u64 entrieslba, firststart, hdrend;
	u32 nentries, entrysz, i;
	u32 *hdr;

	if(cardread(1, buf) < 0)
		return -1;
	if(memcmp(buf, "EFI PART", 8) != 0)
		return -1;
	entrieslba = get64(buf + 72);
	nentries = get32le(buf + 80);
	entrysz = get32le(buf + 84);
	hdrend = entrieslba + (nentries * entrysz + BlkSize - 1) / BlkSize;
	firststart = 0;
	for(i = 0; i < nentries; i++){
		u32 l = (u32)(entrieslba + (i * entrysz) / BlkSize);
		u32 off = (i * entrysz) % BlkSize;

		if(cardread(l, buf) < 0)
			return -1;
		if(get64(buf + off) == 0 && get64(buf + off + 8) == 0)
			continue;	/* unused entry (type GUID zero) */
		{
			u64 s = get64(buf + off + 32);

			if(firststart == 0 || s < firststart)
				firststart = s;
		}
	}
	if(firststart < hdrend + 256)
		return -1;
	logblocks = (u32)(firststart - hdrend);
	if(logblocks > 2048)
		logblocks = 2048;
	logbase = (u32)(firststart - logblocks);
	/* First block: our header, or zeros. */
	if(cardread(logbase, buf) < 0)
		return -1;
	hdr = (u32 *)buf;
	if(hdr[0] != Logmagic){
		/*
		 * Foreign data at the conventional tail position must
		 * not be overwritten.  Fall back to the longest run of
		 * genuinely zero blocks in the GPT gap; only if at
		 * least 256 consecutive blocks are zero do we claim the
		 * end of that run for the log.
		 */
		u32 lba, runstart, runlen, beststart, bestlen;
		int j, allzero;

		runstart = runlen = beststart = bestlen = 0;
		for(lba = (u32)hdrend; lba < (u32)firststart; lba++){
			if(cardread(lba, buf) < 0)
				return -1;
			allzero = 1;
			for(j = 0; j < BlkSize; j++){
				if(buf[j] != 0){
					allzero = 0;
					break;
				}
			}
			if(allzero){
				if(runlen == 0)
					runstart = lba;
				runlen++;
				if(runlen > bestlen){
					beststart = runstart;
					bestlen = runlen;
				}
			}else
				runlen = 0;
		}
		if(bestlen < 256)
			return -1;	/* no safe area: hands off */
		logblocks = bestlen;
		if(logblocks > 2048)
			logblocks = 2048;
		logbase = beststart + bestlen - logblocks;
		if(cardread(logbase, buf) < 0)
			return -1;
		hdr = (u32 *)buf;
	}
	logbuf = mallocz((logblocks - 1) * BlkSize, 1);
	if(logbuf == nil)
		return -1;
	if(hdr[0] == Logmagic){
		logused = hdr[1];
		if(logused > (logblocks - 1) * BlkSize)
			logused = 0;
		for(i = 0; i < (logused + BlkSize - 1) / BlkSize; i++)
			if(cardread(logbase + 1 + i,
			    logbuf + i * BlkSize) < 0)
				return -1;
	}else
		logused = 0;
	logready = 1;
	return 0;
}

static void
logflushhdr(void)
{
	u8 buf[BlkSize];
	u32 *hdr = (u32 *)buf;

	memset(buf, 0, sizeof buf);
	hdr[0] = Logmagic;
	hdr[1] = logused;
	cardwrite(logbase, buf);
}

/* Append bytes to the on-card log (caller holds cardlk). */
static int
logappend(void *data, long n)
{
	u8 *p = data;
	u32 cap, blk;
	long m;

	if(!logready)
		return -1;
	cap = (logblocks - 1) * BlkSize;
	while(n > 0){
		if(logused >= cap){
			/* Full: restart from the top. */
			logused = 0;
		}
		blk = logused / BlkSize;
		m = BlkSize - (logused % BlkSize);
		if(m > n)
			m = n;
		memmove(logbuf + logused, p, m);
		if(cardwrite(logbase + 1 + blk,
		    logbuf + blk * BlkSize) < 0)
			return -1;
		logused += m;
		p += m;
		n -= m;
	}
	logflushhdr();
	return 0;
}

static void
fsread(Req *r, void *aux)
{
	switch((int)(uintptr)aux){
	case Qlog:
		qlock(&cardlk);
		if(logready)
			readbuf(r, logbuf, logused);
		else
			readbuf(r, "", 0);
		qunlock(&cardlk);
		respond(r, nil);
		return;
	case Qstat: {
		char buf[256];

		snprint(buf, sizeof buf,
			"sdhci %#llux caps %#ux sectors %llud logbase %ud blocks %ud used %ud\n",
			d.base, rd32(Rcaps), nsectors, logbase, logblocks,
			logused);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	}
	respond(r, "read not permitted");
}

static void
fswrite(Req *r, void *aux)
{
	switch((int)(uintptr)aux){
	case Qlog:
		qlock(&cardlk);
		if(logappend(r->ifcall.data, r->ifcall.count) < 0){
			qunlock(&cardlk);
			respond(r, "emmc write failed");
			return;
		}
		qunlock(&cardlk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	case Qctl:
		if(strncmp((char *)r->ifcall.data, "clear", 5) == 0){
			qlock(&cardlk);
			logused = 0;
			if(logready)
				logflushhdr();
			qunlock(&cardlk);
		}
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	respond(r, "write not permitted");
}

void
main(int argc, char **argv)
{
	static DrvSrv *s;
	static File *rootfiles;

	USED(argc);
	USED(argv);
	USED(rootfiles);
	if(drvfindreg(&d, 0x4744000ULL) < 0)
		drvfail("emmclog: no sdhci@4744000 in #P");
	if(drvmap(&d) < 0)
		drvfail("emmclog: cannot map %s: %r", d.name);
	if(clocks_on() < 0)
		drvfail("emmclog: SDCC1 clocks did not start (%s)", clockdbg);
	if(cardinit() < 0)
		drvfail("emmclog: card init failed");
	if(logareainit() < 0)
		drvfail("emmclog: no safe log area in GPT gap");
	{
		static DrvFile files[] = {
			{ "log", 0664, (void *)Qlog, fsread, fswrite },
			{ "stat", 0444, (void *)Qstat, fsread, nil },
			{ "ctl", 0222, (void *)Qctl, nil, fswrite },
		};

		s = drvsrv("emmclog", files, nelem(files));
		if(s == nil)
			drvfail("emmclog: cannot build 9P tree");
		drvsrvpost(s);
	}
	drvhealthrun("emmclog");
}
