/* sfcs: the RK3566 serial flash controller as a read-only 9P service.
 *
 * The SFC drives the SPI NOR that holds U-Boot.  This driver only reads
 * it: no write or erase path exists, so a bug cannot brick the boot
 * flash.  Reads use the plain 1-1-1 READ (0x03) opcode at the xin24m
 * rate; the JEDEC ID (0x9F) and SFDP (0x5A) identify the part.
 *
 *	/status	read	version, JEDEC ID, capacity, counters
 *	/data	read	flash contents at the file offset
 *	/ctl	write	rescan (re-read ID and SFDP)
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"

enum {
	Ctrl		= 0x00,
	Imr		= 0x04,
	Iclr		= 0x08,
	Ftlr		= 0x0c,
	Rcvr		= 0x10,
	Ax		= 0x14,
	Abit		= 0x18,
	Isr		= 0x1c,
	Fsr		= 0x20,
	Sr		= 0x24,
	Risr		= 0x28,
	Ver		= 0x2c,
	LenCtrl		= 0x88,
	LenExt		= 0x8c,
	Cmd		= 0x100,
	Addr		= 0x104,
	Data		= 0x108,

	CtrlPhaseSel	= 1 << 1,
	CtrlWpen	= 1 << 29,
	CmdDummyShift	= 8,
	CmdDirShift	= 12,
	CmdAddrShift	= 14,
	CmdTranShift	= 16,
	CmdCsShift	= 30,
	Addr24		= 1,
	Addr32		= 2,

	RcvrReset	= 1 << 0,
	SrBusy		= 1 << 0,
	FsrRxLevel	= 0x1f << 16,

	LenCtrlTrbSel	= 1,
	Maxchunk	= 4096,
	Timeout		= 200 * 1000 * 1000LL,

	CruGateSfc	= 0x324,	/* CLKGATE_CON(9) bits 2,4 */
	CruSel28	= 0x170,	/* CLKSEL_CON(28) bits 4-6 */
};

typedef struct Sfc Sfc;
struct Sfc {
	Drv	d;
	Drv	grf;
	Drv	cru;
	Lock	lk;
	int	version;
	int	capacity;
	int	phase;
	int	fast;
	u8	id[3];
	uvlong	reads;
	uvlong	bytes;
	char	hex[1024];
	ulong	errors;
	char	lastctl[64];
	char	ctlbuf[64];
	int	ctllen;
};

static Sfc sfc;

static void
delayus(int us)
{
	vlong end;

	end = nsec() + (vlong)us * 1000;
	while(nsec() < end)
		;
}

static void
sfcreset(Sfc *s)
{
	vlong deadline;

	drw(&s->d, Rcvr, RcvrReset);
	deadline = nsec() + 10*1000*1000LL;
	while(drd(&s->d, Rcvr) & RcvrReset)
		if(nsec() > deadline)
			break;
	drw(&s->d, Iclr, 0xffffffff);
}

static int
waitrx(Sfc *s, int minwords)
{
	vlong deadline;
	u32 fsr;

	deadline = nsec() + Timeout;
	for(;;){
		fsr = drd(&s->d, Fsr);
		if(((fsr & FsrRxLevel) >> 16) >= minwords)
			return (fsr & FsrRxLevel) >> 16;
		if(nsec() > deadline)
			return -1;
	}
}

static int
readfifo(Sfc *s, u8 *buf, int len)
{
	int dwords, bytes, i, level;
	u32 v;

	dwords = len >> 2;
	bytes = len & 3;
	while(dwords > 0){
		level = waitrx(s, 1);
		if(level < 0)
			return -1;
		if(level > dwords)
			level = dwords;
		for(i = 0; i < level; i++){
			v = drd(&s->d, Data);
			*buf++ = v;
			*buf++ = v >> 8;
			*buf++ = v >> 16;
			*buf++ = v >> 24;
		}
		dwords -= level;
	}
	if(bytes){
		if(waitrx(s, 1) < 0)
			return -1;
		v = drd(&s->d, Data);
		for(i = 0; i < bytes; i++)
			*buf++ = v >> (8*i);
	}
	return 0;
}

/*
 * One command.  addrbits is 0, 24 or 32; dummybytes is the number of
 * dummy clocks divided by 8; len is the number of bytes read back.
 */
static int
sfop(Sfc *s, u32 opcode, u32 addr, int addrbits, int dummybytes,
     u8 *buf, int len)
{
	u32 ctrl, cmd;
	vlong deadline;

	ctrl = CtrlWpen | (s->phase ? CtrlPhaseSel : 0);
	cmd = opcode & 0xff;
	if(addrbits == 24)
		cmd |= Addr24 << CmdAddrShift;
	else if(addrbits == 32)
		cmd |= Addr32 << CmdAddrShift;
	if(dummybytes)
		cmd |= (u32)dummybytes * 8 << CmdDummyShift;
	if(s->version >= 4)
		drw(&s->d, LenExt, len);
	else if(len > 0)
		cmd |= (u32)len << CmdTranShift;
	cmd |= 0 << CmdCsShift;

	drw(&s->d, Ctrl, ctrl);
	drw(&s->d, Cmd, cmd);
	if(addrbits)
		drw(&s->d, Addr, addr);
	if(len > 0 && readfifo(s, buf, len) < 0)
		return -1;

	deadline = nsec() + Timeout;
	while(drd(&s->d, Sr) & SrBusy)
		if(nsec() > deadline){
			sfcreset(s);
			return -1;
		}
	return 0;
}

static int
flashread(Sfc *s, u32 addr, u8 *buf, int len)
{
	int n;

	while(len > 0){
		n = len > Maxchunk ? Maxchunk : len;
		if(s->fast ? sfop(s, 0x0b, addr, 24, 1, buf, n) < 0
		           : sfop(s, 0x03, addr, 24, 0, buf, n) < 0)
			return -1;
		addr += n;
		buf += n;
		len -= n;
	}
	return 0;
}

static ulong
le4(u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
}

/* SFDP gives the density in bits; fall back to the ID table. */
static int
capacity(Sfc *s)
{
	u8 hdr[16], bfpt[12];
	u32 ptr, density;
	int cap;

	if(sfop(s, 0x5a, 0, 24, 1, hdr, sizeof hdr) == 0){
		ptr = hdr[0x0d] | hdr[0x0e] << 8 | hdr[0x0f] << 16;
		if(ptr >= 0x40 && ptr < 0x100000 &&
		   sfop(s, 0x5a, ptr, 24, 1, bfpt, sizeof bfpt) == 0){
			density = le4(bfpt + 8) & 0x7fffffff;
			if(density == 0)
				density = 0x80000000;
			cap = density / 8;
			if(cap >= 1024*1024 && cap <= 256*1024*1024)
				return cap;
		}
	}
	/* Common 3-byte JEDEC IDs: memory type 0x40, capacity 0x17..0x19. */
	if(s->id[1] == 0x40){
		switch(s->id[2]){
		case 0x14: return 1*1024*1024;
		case 0x15: return 2*1024*1024;
		case 0x16: return 4*1024*1024;
		case 0x17: return 8*1024*1024;
		case 0x18: return 16*1024*1024;
		case 0x19: return 32*1024*1024;
		}
	}
	return 16*1024*1024;
}

static void
flashwake(Sfc *s)
{
	/* Release from deep power-down, then a full software reset. */
	sfop(s, 0xab, 0, 0, 0, nil, 0);
	delayus(500);
	sfop(s, 0x66, 0, 0, 0, nil, 0);
	sfop(s, 0x99, 0, 0, 0, nil, 0);
	delayus(1000);
}

static void
identify(Sfc *s)
{
	flashwake(s);
	if(sfop(s, 0x9f, 0, 0, 0, s->id, 3) < 0)
		s->errors++;
	s->version = drd(&s->d, Ver) & 0xffff;
	s->capacity = capacity(s);
}

static void
fsread(Req *r, void *aux)
{
	Sfc *s;
	char *name;
	char buf[512];
	u8 tmp[Maxchunk];
	int n, len;

	s = aux;
	name = drvfile(r)->name;
	if(strcmp(name, "status") == 0){
		snprint(buf, sizeof buf,
			"rockchip,sfc fe300000\n"
			"version %d\n"
			"jedec %02ux %02ux %02ux\n"
			"capacity %d\n"
			"clock 24000000\n"
			"grf %ux %ux %ux phase %d fast %d\n"
			"reads %llud bytes %llud errors %lud\n"
			"lastctl %s\n",
			s->version, s->id[0], s->id[1], s->id[2],
			s->capacity,
			drd(&s->grf, 0x18), drd(&s->grf, 0x1c), drd(&s->grf, 0x0c),
			s->phase, s->fast,
			s->reads, s->bytes, s->errors, s->lastctl);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	if(strcmp(name, "hex") == 0){
		lock(&s->lk);
		snprint(buf, sizeof buf, "%s", s->hex);
		unlock(&s->lk);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	if(strcmp(name, "data") == 0){
		if(r->ifcall.offset >= s->capacity || s->capacity == 0){
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		len = r->ifcall.count;
		if(len > Maxchunk)
			len = Maxchunk;
		if(r->ifcall.offset + len > s->capacity)
			len = s->capacity - r->ifcall.offset;
		lock(&s->lk);
		if(flashread(s, r->ifcall.offset, tmp, len) < 0){
			s->errors++;
			unlock(&s->lk);
			respond(r, "sfc: read failed");
			return;
		}
		s->reads++;
		s->bytes += len;
		unlock(&s->lk);
		memmove(r->ofcall.data, tmp, len);
		r->ofcall.count = len;
		respond(r, nil);
		return;
	}
	USED(n);
	respond(r, "read not permitted");
}

static void
fswrite(Req *r, void *aux)
{
	Sfc *s;
	char *name;
	char buf[64];
	int i, n, complete, ok;

	s = aux;
	name = drvfile(r)->name;
	if(strcmp(name, "ctl") != 0){
		respond(r, "write not permitted");
		return;
	}
	n = r->ifcall.count;
	if(n >= (int)sizeof buf)
		n = sizeof buf - 1;
	memmove(buf, r->ifcall.data, n);
	buf[n] = 0;

	complete = 0;
	ok = 0;
	lock(&s->lk);
	if(n > 0 && s->ctllen + n < (int)sizeof s->ctlbuf){
		memmove(s->ctlbuf + s->ctllen, buf, n);
		s->ctllen += n;
		s->ctlbuf[s->ctllen] = 0;
		for(i = 0; i < s->ctllen; i++)
			if(s->ctlbuf[i] == '\n')
				break;
		if(i < s->ctllen){
			s->ctlbuf[i] = 0;
			complete = 1;
			snprint(s->lastctl, sizeof s->lastctl, "%s", s->ctlbuf);
			if(strcmp(s->ctlbuf, "rescan") == 0){
				identify(s);
				ok = 1;
			}else if(strncmp(s->ctlbuf, "fast ", 5) == 0){
				s->fast = atoi(s->ctlbuf + 5) != 0;
				ok = 1;
			}else if(strncmp(s->ctlbuf, "phase ", 6) == 0){
				s->phase = atoi(s->ctlbuf + 6) != 0;
				ok = 1;
			}else if(strcmp(s->ctlbuf, "flashreset") == 0){
				flashwake(s);
				identify(s);
				ok = 1;
			}else if(strcmp(s->ctlbuf, "wake") == 0){
				u8 tmp[1];
				if(sfop(s, 0xab, 0, 0, 0, tmp, 0) == 0)
					ok = 1;
			}else if(strcmp(s->ctlbuf, "idraw") == 0){
				u8 tmp[8];
				int j, k;

				if(sfop(s, 0x9f, 0, 0, 0, tmp, sizeof tmp) == 0){
					k = snprint(s->hex, sizeof s->hex, "\nidraw:");
					for(j = 0; j < (int)sizeof tmp; j++)
						k += snprint(s->hex+k, sizeof s->hex-k, " %02ux", tmp[j]);
					snprint(s->hex+k, sizeof s->hex-k, "\n");
					ok = 1;
				}
			}else if(strcmp(s->ctlbuf, "sr") == 0){
				u8 tmp[1];
				if(sfop(s, 0x05, 0, 0, 0, tmp, 1) == 0){
					snprint(s->hex, sizeof s->hex, "\nsr: %02ux\n", tmp[0]);
					ok = 1;
				}
			}else if(strncmp(s->ctlbuf, "dump ", 5) == 0){
				char *f[4];
				u8 tmp[256];
				u32 addr;
				int len, j, k;

				if(tokenize(s->ctlbuf, f, nelem(f)) == 3){
					addr = strtoul(f[1], nil, 0);
					len = atoi(f[2]);
					if(len > 0 && len <= (int)sizeof tmp &&
					   flashread(s, addr, tmp, len) == 0){
						k = 0;
						for(j = 0; j < len && k < (int)sizeof s->hex - 4; j++){
							if(j % 16 == 0)
								k += snprint(s->hex+k, sizeof s->hex-k, "\n%04ux:", addr+j);
							k += snprint(s->hex+k, sizeof s->hex-k,
								" %02ux", tmp[j]);
						}
						snprint(s->hex+k, sizeof s->hex-k, "\n");
						s->reads++;
						s->bytes += len;
						ok = 1;
					}
				}
			}
			s->ctllen = 0;
			s->ctlbuf[0] = 0;
		}
	}
	unlock(&s->lk);

	if(complete && !ok){
		respond(r, "ctl: rescan");
		return;
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static DrvFile files[] = {
	{ "status",	0444,	&sfc,	fsread,	nil },
	{ "data",	0444,	&sfc,	fsread,	nil },
	{ "hex",	0444,	&sfc,	fsread,	nil },
	{ "ctl",	0222,	&sfc,	nil,	fswrite },
};

void
main(int argc, char **argv)
{
	DrvSrv *s;
	u32 v;

	USED(argc);
	USED(argv);
	if(drvfindreg(&sfc.d, 0xfe300000ULL) < 0){
		if(drvfind(&sfc.d, "sfc", 0) < 0)
			drvfail("sfcs: no SFC in #P");
	}
	if(drvmap(&sfc.d) < 0)
		drvfail("sfcs: cannot map %s: %r", sfc.d.name);
	if(drvfindreg(&sfc.cru, 0xfdd20000ULL) < 0 || drvmap(&sfc.cru) < 0)
		drvfail("sfcs: no CRU at fdd20000");
	if(drvfindreg(&sfc.grf, 0xfdc60000ULL) < 0 || drvmap(&sfc.grf) < 0)
		drvfail("sfcs: no SYS GRF at fdc60000");

	/* hclk_sfc and sclk_sfc in CLKGATE_CON(9), bits 2 and 4. */
	drw(&sfc.cru, CruGateSfc, 0x00140000);
	/* sclk_sfc mux = xin24m (0 of xin24m/cpll50/gpll75/...). */
	drw(&sfc.cru, CruSel28, 0x00070000);

	/*
	 * FSPI pins, SYS_GRF hiword write masks (full 4-bit fields):
	 * D0-D3 -> 0x18 bits 0,4,8,12 = 1; D4 -> 0x1c bit 0 = 1;
	 * B7 (d2) -> 0x0c bits 12-15 = 2.
	 */
	drw(&sfc.grf, 0x18, 0xffff1111);
	drw(&sfc.grf, 0x1c, 0x000f0001);
	drw(&sfc.grf, 0x14, 0x000f2000);	/* d2 = GPIO1_C7, function 2 */

	drw(&sfc.d, Ctrl, 0);
	drw(&sfc.d, Iclr, 0xffffffff);
	drw(&sfc.d, Imr, 0xffffffff);
	sfc.phase = 1;
	sfc.fast = 1;
	sfc.version = drd(&sfc.d, Ver) & 0xffff;
	if(sfc.version >= 4)
		drw(&sfc.d, LenCtrl, LenCtrlTrbSel);
	sfcreset(&sfc);
	identify(&sfc);

	s = drvsrv("sfc", files, nelem(files));
	if(s == nil)
		drvfail("sfcs: cannot build 9P tree");
	drvsrvpost(s);
	drvhealthrun("sfcs");
}
