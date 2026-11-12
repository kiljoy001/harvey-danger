/* spifs: the RK3566 SPI3 controller as a 9P file server.
 *
 * SPI3 lives at fe640000 and is the one wired to the 40-pin header (m0:
 * GPIO4 A6 cs0, B0 miso, B2 mosi, B3 clk, all function 4).  The block is
 * a Rockchip-modified DesignWare APB SSI: same control registers, but SER
 * is at 0x0c and TX/RX data are separate 32-bit registers at 0x400/0x800.
 *
 *	/status	read	version, clock, config, fifo state, counters
 *	/ctl	write	speed <hz>
 *			mode <0..3>
 *			cs <0|1>
 *			lsb <0|1>
 *			bits <8>
 *	/xfer	write	clock the bytes out (MOSI)
 *		read	the bytes clocked in by the last transfer (MISO)
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"

enum {
	Ctrlr0		= 0x0000,
	Ctrlr1		= 0x0004,
	SsiEnr		= 0x0008,
	Ser		= 0x000c,
	Baudr		= 0x0010,
	TxFtlr		= 0x0014,
	RxFtlr		= 0x0018,
	TxFlr		= 0x001c,
	RxFlr		= 0x0020,
	Sr		= 0x0024,
	Imr		= 0x002c,
	Isr		= 0x0030,
	Icr		= 0x0038,
	Version		= 0x0048,
	Txdr		= 0x0400,
	Rxdr		= 0x0800,

	SrBusy		= 1 << 0,
	SrTfFull	= 1 << 1,
	SrTfEmpty	= 1 << 2,
	SrRfEmpty	= 1 << 3,
	SrRfFull	= 1 << 4,

	Cr0Dfs8		= 1 << 0,
	Cr0Scph		= 6,
	Cr0Scpol	= 7,
	Cr0SsdOne	= 1 << 10,
	Cr0EmBig	= 1 << 11,
	Cr0Fbm		= 12,
	Cr0Bht8		= 1 << 13,

	Maxbuf		= 4096,
	FifoLen		= 32,
	ClockHz		= 24000000,
	DefSpeed	= 1000000,
	Timeout		= 200 * 1000 * 1000LL,	/* 200 ms */

	CruGateSpi3	= 0x37c,	/* CLKGATE_CON(31) bits 0,1 */
	CruSel72	= 0x220,	/* CLKSEL_CON(72) bits 6,7 */

	GrfGpio4A	= 0x64,		/* PMU? no: SYS_GRF GPIO4A iomux high */
	GrfGpio4B	= 0x68,		/* GPIO4B iomux low */
};

typedef struct Spi Spi;
struct Spi {
	Drv	d;
	Drv	grf;
	Drv	cru;
	Lock	lk;
	int	speed;
	int	mode;
	int	cs;
	int	lsb;
	int	bits;
	u8	tx[Maxbuf];
	u8	rx[Maxbuf];
	int	rxlen;
	uvlong	transfers;
	uvlong	bytes;
	ulong	errors;
	char	lastctl[64];
	char	ctlbuf[64];
	int	ctllen;
};

static Spi spi;

static u32
baudfor(int speed)
{
	u32 v;

	if(speed <= 0)
		speed = DefSpeed;
	/* The controller only divides by an even number. */
	v = 2 * ((ClockHz + 2*speed - 1) / (2*speed));
	if(v < 2)
		v = 2;
	if(v > 65534)
		v = 65534;
	return v;
}

static int
xfer(u8 *tx, u8 *rx, int len)
{
	u32 cr0, baudr;
	vlong deadline;
	int i, n;
	u32 sr;

	if(len <= 0 || len > Maxbuf)
		return -1;
	cr0 = Cr0Dfs8 | Cr0SsdOne | Cr0EmBig | Cr0Bht8;
	cr0 |= (spi.mode & 3) << Cr0Scph;
	if(spi.lsb)
		cr0 |= 1 << Cr0Fbm;
	baudr = baudfor(spi.speed);

	drw(&spi.d, SsiEnr, 0);
	drw(&spi.d, Ctrlr0, cr0);
	drw(&spi.d, Ctrlr1, len - 1);
	drw(&spi.d, TxFtlr, 0);
	drw(&spi.d, RxFtlr, 0);
	drw(&spi.d, Baudr, baudr);
	drw(&spi.d, Icr, 0xffffffff);
	drw(&spi.d, SsiEnr, 1);
	drw(&spi.d, Ser, 1 << spi.cs);

	deadline = nsec() + Timeout;
	n = 0;
	for(i = 0; i < len; i++){
		while((sr = drd(&spi.d, Sr)) & SrTfFull){
			if(nsec() > deadline)
				goto timeout;
		}
		drw(&spi.d, Txdr, tx[i]);
		while(!(drd(&spi.d, Sr) & SrRfEmpty)){
			if(n < len)
				rx[n++] = drd(&spi.d, Rxdr);
			else
				drd(&spi.d, Rxdr);
		}
	}
	while(n < len){
		if(nsec() > deadline)
			goto timeout;
		if(!(drd(&spi.d, Sr) & SrRfEmpty))
			rx[n++] = drd(&spi.d, Rxdr);
	}
	while(drd(&spi.d, Sr) & SrBusy){
		if(nsec() > deadline)
			goto timeout;
	}
	drw(&spi.d, Ser, 0);
	drw(&spi.d, SsiEnr, 0);
	return len;

timeout:
	drw(&spi.d, Ser, 0);
	drw(&spi.d, SsiEnr, 0);
	return -1;
}

static void
fsread(Req *r, void *aux)
{
	Spi *s;
	char *name;
	char buf[1024];
	int i, n;

	s = aux;
	name = drvfile(r)->name;
	if(strcmp(name, "status") == 0){
		n = snprint(buf, sizeof buf,
			"rockchip,rk3568-spi fe640000\n"
			"version %ux\n"
			"clock %d speed %d baudr %ud\n"
			"mode %d cs %d lsb %d bits %d\n"
			"sr %ux txflr %ud rxflr %ud\n"
			"grf 4a %ux 4b %ux\n"
			"grfx %ux %ux %ux %ux %ux %ux %ux %ux\n"
			"transfers %llud bytes %llud errors %lud\n"
			"lastctl %s\n",
			drd(&s->d, Version),
			ClockHz, s->speed, drd(&s->d, Baudr),
			s->mode, s->cs, s->lsb, s->bits,
			drd(&s->d, Sr), drd(&s->d, TxFlr), drd(&s->d, RxFlr),
			drd(&s->grf, GrfGpio4A), drd(&s->grf, GrfGpio4B),
			drd(&s->grf, 0x60), drd(&s->grf, 0x64),
			drd(&s->grf, 0x68), drd(&s->grf, 0x6c),
			drd(&s->grf, 0x70), drd(&s->grf, 0x74),
			drd(&s->grf, 0x78), drd(&s->grf, 0x7c),
			s->transfers, s->bytes, s->errors, s->lastctl);
		n += snprint(buf+n, sizeof buf-n, "lasttx ");
		for(i = 0; i < s->rxlen && i < 16; i++)
			n += snprint(buf+n, sizeof buf-n, "%02ux ", s->tx[i]);
		n += snprint(buf+n, sizeof buf-n, "\nlastrx ");
		for(i = 0; i < s->rxlen && i < 16; i++)
			n += snprint(buf+n, sizeof buf-n, "%02ux ", s->rx[i]);
		n += snprint(buf+n, sizeof buf-n, "\n");
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	if(strcmp(name, "xfer") == 0){
		readbuf(r, s->rx, s->rxlen);
		respond(r, nil);
		return;
	}
	respond(r, "read not permitted");
}

static void
fswrite(Req *r, void *aux)
{
	Spi *s;
	char *name, *f[4];
	char buf[64];
	int i, n, nf, complete, ok, val;

	s = aux;
	name = drvfile(r)->name;
	if(strcmp(name, "xfer") == 0){
		n = r->ifcall.count;
		if(n > Maxbuf)
			n = Maxbuf;
		if(n <= 0){
			respond(r, "xfer: empty");
			return;
		}
		memmove(s->tx, r->ifcall.data, n);
		lock(&s->lk);
		if(xfer(s->tx, s->rx, n) < 0){
			s->errors++;
			s->rxlen = 0;
			unlock(&s->lk);
			respond(r, "spi: transfer timed out");
			return;
		}
		s->rxlen = n;
		s->transfers++;
		s->bytes += n;
		unlock(&s->lk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
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
			nf = tokenize(s->ctlbuf, f, nelem(f));
			if(nf == 2 && strcmp(f[0], "speed") == 0){
				val = atoi(f[1]);
				if(val >= 1000 && val <= ClockHz)
					s->speed = val, ok = 1;
			}else if(nf == 2 && strcmp(f[0], "mode") == 0){
				val = atoi(f[1]);
				if(val >= 0 && val <= 3)
					s->mode = val, ok = 1;
			}else if(nf == 2 && strcmp(f[0], "cs") == 0){
				val = atoi(f[1]);
				if(val >= 0 && val <= 1)
					s->cs = val, ok = 1;
			}else if(nf == 2 && strcmp(f[0], "lsb") == 0){
				val = atoi(f[1]);
				if(val >= 0 && val <= 1)
					s->lsb = val, ok = 1;
			}else if(nf == 3 && strcmp(f[0], "grf") == 0){
				drw(&s->grf, strtoul(f[1], nil, 0),
					strtoul(f[2], nil, 0));
				ok = 1;
			}else if(nf == 2 && strcmp(f[0], "bits") == 0){
				val = atoi(f[1]);
				if(val == 8)
					s->bits = val, ok = 1;
			}
			s->ctllen = 0;
			s->ctlbuf[0] = 0;
		}
	}
	unlock(&s->lk);

	if(complete && !ok){
		respond(r, "ctl: speed/mode/cs/lsb/bits");
		return;
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static DrvFile files[] = {
	{ "status",	0444,	&spi,	fsread,	nil },
	{ "xfer",	0666,	&spi,	fsread,	fswrite },
	{ "ctl",	0222,	&spi,	nil,	fswrite },
};

void
main(int argc, char **argv)
{
	DrvSrv *s;
	u32 v;

	USED(argc);
	USED(argv);
	if(drvfindreg(&spi.d, 0xfe640000ULL) < 0){
		if(drvfind(&spi.d, "spi", 0) < 0)
			drvfail("spifs: no SPI in #P");
	}
	if(drvmap(&spi.d) < 0)
		drvfail("spifs: cannot map %s: %r", spi.d.name);
	if(drvfindreg(&spi.cru, 0xfdd20000ULL) < 0 || drvmap(&spi.cru) < 0)
		drvfail("spifs: no CRU at fdd20000");
	if(drvfindreg(&spi.grf, 0xfdc60000ULL) < 0 || drvmap(&spi.grf) < 0)
		drvfail("spifs: no SYS GRF at fdc60000");

	spi.speed = DefSpeed;
	spi.mode = 0;
	spi.cs = 0;
	spi.lsb = 0;
	spi.bits = 8;

	/* pclk_spi3 and clk_spi3 in CLKGATE_CON(31), bits 0 and 1. */
	drw(&spi.cru, CruGateSpi3, 0x00030000);
	/* clk_spi3 mux = xin24m (1 of gpll200/xin24m/cpll100). */
	drw(&spi.cru, CruSel72, 0x000C0040);

	/*
	 * SPI3 m0 pins, SYS_GRF hiword write masks: cs0 A6 -> 0x64 bit 8,
	 * miso B0 -> 0x68 bit 0, mosi B2 -> 0x68 bit 8, clk B3 -> 0x68
	 * bit 12, all function 4.
	 */
	v = (0xf << (8 + 16)) | (4 << 8);
	drw(&spi.grf, GrfGpio4A, v);
	v = ((0xf << 0) | (0xf << 8) | (0xf << 12)) << 16;
	v |= (4 << 0) | (4 << 8) | (4 << 12);
	drw(&spi.grf, GrfGpio4B, v);

	drw(&spi.d, SsiEnr, 0);

	s = drvsrv("spi", files, nelem(files));
	if(s == nil)
		drvfail("spifs: cannot build 9P tree");
	drvsrvpost(s);
	drvhealthrun("spifs");
}
