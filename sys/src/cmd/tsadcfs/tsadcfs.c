/* tsadcfs: the RK3566 SoC temperature sensor as a 9P file server.
 *
 * The TSADC has two auto-mode channels (CPU and GPU).  Bring-up follows the
 * vendor rk_tsadcv7_initialize: analog bias through the GRF at fdc60000,
 * auto period and debounce, then AUTO_EN.  A reading is the 12-bit code in
 * DATA(chn), mapped through the RK3568 code table with linear interpolation.
 *
 * The vendor driver applies an eFuse trim; Harvey has no eFuse driver yet,
 * so the raw table is used.  Expect a few degrees of offset until that is
 * wired up.
 *
 *	/temp0, /temp1	read	"code millidegrees"
 *	/status		read	codes, registers
 *	/ctl		write	bring-up experiments (see fswrite)
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"

enum {
	TsUserCon	= 0x00,
	TsAutoCon	= 0x04,
	TsIntEn		= 0x08,
	TsData		= 0x20,		/* + chn*4 */
	TsHighIntDeb	= 0x60,
	TsHighShutDeb	= 0x64,
	TsAutoPeriod	= 0x68,
	TsAutoPeriodHt	= 0x6c,

	TsDataMask	= 0xfff,
	TsUserPdSoc	= 0xfc0,
	TsAutoPeriodVal	= 1622,
	TsDebounce	= 4,
	TsAutoEn	= 1 << 0,
	TsAutoQSel	= 1 << 1,
	TsAdcPower	= 1 << 3,
	TsSrcEn0	= 1 << 4,
	TsSrcEn1	= 1 << 5,
	TsAdcBusy	= 1 << 15,

	TsTimeout	= 100 * 1000 * 1000LL,	/* 100 ms */

	GrfTsadcCon	= 0x0600,
	GrfTsen		= 0x10001 << 8,
	GrfAna0		= 0x10001 << 0,
	GrfAna1		= 0x10001 << 1,
	GrfAna2		= 0x10001 << 2,

	CruGateTsadc	= 0x368,	/* CLKGATE_CON(26): pclk, tsen, tsadc */
	CruSel51	= 0x1cc,	/* CLKSEL_CON(51): tsen/tsadc mux+div */
	CruRstTsadc	= 0x460,	/* SOFTRST_CON(24): pclk, tsadc */
	CruRstTsadcPhy	= 0x474,	/* SOFTRST_CON(29): tsadc phy */

	Nchan		= 2,
};

typedef struct Trip Trip;
struct Trip {
	int	code;
	int	temp;		/* millidegrees */
};

/* rk3568_code_table from the vendor thermal driver. */
static Trip table[] = {
	{0, -60000}, {1448, -60000}, {1584, -40000}, {1620, -35000},
	{1652, -30000}, {1688, -25000}, {1720, -20000}, {1756, -15000},
	{1788, -10000}, {1824, -5000}, {1856, 0}, {1892, 5000},
	{1924, 10000}, {1956, 15000}, {1992, 20000}, {2024, 25000},
	{2060, 30000}, {2092, 35000}, {2128, 40000}, {2160, 45000},
	{2196, 50000}, {2228, 55000}, {2264, 60000}, {2300, 65000},
	{2332, 70000}, {2368, 75000}, {2400, 80000}, {2436, 85000},
	{2468, 90000}, {2500, 95000}, {2536, 100000}, {2572, 105000},
	{2604, 110000}, {2636, 115000}, {2672, 120000}, {2704, 125000},
	{3076, 180000}, {0xfff, 180000},
};

typedef struct Ts Ts;
struct Ts {
	Drv	ts;
	Drv	grf;
	Drv	cru;
	QLock	lk;
	uvlong	reads;
	ulong	errors;
	ulong	ctlwrites;
	char	lastctl[64];
	char	ctlbuf[64];
	int	ctllen;
};

static Ts ts;

static void
delayus(int us)
{
	vlong end;

	end = nsec() + (vlong)us * 1000;
	while(nsec() < end)
		;
}

static int
code2temp(int code)
{
	int i, num, den;

	if(code < table[1].code)
		return table[1].temp;
	for(i = 1; i < nelem(table); i++){
		if(code <= table[i].code && code > table[i-1].code){
			num = (table[i].temp - table[i-1].temp) *
			      abs(table[i-1].code - code);
			den = abs(table[i-1].code - table[i].code);
			return table[i-1].temp + num/den;
		}
	}
	return table[nelem(table)-1].temp;
}

static void
tsinit(Ts *t)
{
	/*
	 * TSADC gates: PCLK, TSEN and TSADC in CLKGATE_CON(26),
	 * bits 4-6.  GFLAGS is HIWORD_MASK | SET_TO_DISABLE, so the
	 * upper half is the write mask and a zero bit enables.
	 */
	drw(&t->cru, CruGateTsadc, 0x00700000);

	/*
	 * Reset the block: SRST_P_TSADC/SRST_TSADC (con24) and
	 * SRST_TSADCPHY (con29), assert then deassert as Linux does.
	 */
	drw(&t->cru, CruRstTsadc, 0x00060006);
	drw(&t->cru, CruRstTsadcPhy, 0x00800080);
	delayus(20);
	drw(&t->cru, CruRstTsadc, 0x00060000);
	drw(&t->cru, CruRstTsadcPhy, 0x00800000);
	delayus(10);

	drw(&t->ts, TsUserCon, TsUserPdSoc);
	drw(&t->ts, TsAutoPeriod, TsAutoPeriodVal);
	drw(&t->ts, TsAutoPeriodHt, TsAutoPeriodVal);
	drw(&t->ts, TsHighIntDeb, TsDebounce);
	drw(&t->ts, TsHighShutDeb, TsDebounce);
	drw(&t->ts, TsAutoCon, 0);

	/* Analog bias: TSEN, then the three ANA_REG bits after 15 us. */
	drw(&t->grf, GrfTsadcCon, GrfTsen);
	delayus(15);
	drw(&t->grf, GrfTsadcCon, GrfAna0);
	drw(&t->grf, GrfTsadcCon, GrfAna1);
	drw(&t->grf, GrfTsadcCon, GrfAna2);
	delayus(150);

	/*
	 * User-defined mode: one software-triggered conversion per read.
	 * The auto-mode engine never produced data on this block, but the
	 * user-mode path works, and it keeps the part powered down between
	 * reads.  AUTO_CON = 0 selects user mode.
	 */
	/* Clear any latched TSHUT status (write 1 to clear), then idle. */
	drw(&t->ts, TsAutoCon, 1 << 24);
	/*
	 * tsadc_q_sel: the sensor output is negative-coefficient.  With
	 * this bit clear the block reports raw q (4096-q inverted), which
	 * reads ~25 C low against the RK3568 table.  The vendor driver
	 * sets it in auto mode; user mode needs it too.
	 */
	drw(&t->ts, TsAutoCon, TsAutoQSel);
	drw(&t->ts, TsUserCon, TsUserPdSoc);
}

static int
tsread(Ts *t, int chn, int *code)
{
	vlong deadline;
	u32 v;

	if(chn < 0 || chn >= Nchan)
		return -1;

	/*
	 * Power down, then power up and reset with the channel selected.
	 * USER_CON[4] = 0 starts the conversion automatically after
	 * inter_pd_soc cycles; USER_CON[15] clears when it finishes.
	 */
	drw(&t->ts, TsUserCon, TsUserPdSoc);
	delayus(50);
	drw(&t->ts, TsUserCon, TsUserPdSoc | TsAdcPower | (chn & 7));
	delayus(200);

	deadline = nsec() + TsTimeout;
	while((v = drd(&t->ts, TsUserCon)) & TsAdcBusy){
		if(nsec() > deadline){
			drw(&t->ts, TsUserCon, TsUserPdSoc);
			t->errors++;
			return -1;
		}
	}
	*code = drd(&t->ts, TsData + chn*4) & TsDataMask;
	drw(&t->ts, TsUserCon, TsUserPdSoc);
	t->reads++;
	return 0;
}

static void
fsread(Req *r, void *aux)
{
	Ts *t;
	char *name;
	char buf[1024];
	int chn, code, temp, i, n, rv;

	t = aux;
	name = drvfile(r)->name;
	if(strncmp(name, "temp", 4) == 0){
		chn = atoi(name + 4);
		if(chn < 0 || chn >= Nchan){
			respond(r, "tsadc: bad channel");
			return;
		}
		qlock(&t->lk);
		rv = tsread(t, chn, &code);
		qunlock(&t->lk);
		if(rv < 0){
			respond(r, "tsadc: conversion timed out");
			return;
		}
		temp = code2temp(code);
		snprint(buf, sizeof buf, "%d %d\n", code, temp);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	if(strcmp(name, "status") == 0){
		n = 0;
		qlock(&t->lk);
		for(i = 0; i < Nchan; i++){
			rv = tsread(t, i, &code);
			if(rv < 0)
				n += snprint(buf+n, sizeof buf-n,
					"chan%d error\n", i);
			else
				n += snprint(buf+n, sizeof buf-n,
					"chan%d %d %d\n",
					i, code, code2temp(code));
		}
		qunlock(&t->lk);
		n += snprint(buf+n, sizeof buf-n,
			"user %ux auto %ux period %ux\n"
			"gate %ux sel51 %ux rst %ux phy %ux\n"
			"grf %ux\n"
			"reads %llud errors %lud ctlwrites %lud\n"
			"lastctl %s\n",
			drd(&t->ts, TsUserCon),
			drd(&t->ts, TsAutoCon),
			drd(&t->ts, TsAutoPeriod),
			drd(&t->cru, CruGateTsadc),
			drd(&t->cru, CruSel51),
			drd(&t->cru, CruRstTsadc),
			drd(&t->cru, CruRstTsadcPhy),
			drd(&t->grf, GrfTsadcCon),
			t->reads, t->errors, t->ctlwrites, t->lastctl);
		for(i = 0; i < 0x20; i++){
			if(i % 8 == 0)
				n += snprint(buf+n, sizeof buf-n, "\n%02x:", i*4);
			n += snprint(buf+n, sizeof buf-n, " %08ux",
				drd(&t->ts, i*4));
		}
		n += snprint(buf+n, sizeof buf-n, "\n");
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	respond(r, "read not permitted");
}

static void
fswrite(Req *r, void *aux)
{
	Ts *t;
	char *name, *p, *f[6];
	char buf[64];
	int i, n, nf, complete, ok;

	t = aux;
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
	qlock(&t->lk);
	t->ctlwrites++;
	if(n > 0 && t->ctllen + n < (int)sizeof t->ctlbuf){
		memmove(t->ctlbuf + t->ctllen, buf, n);
		t->ctllen += n;
		t->ctlbuf[t->ctllen] = 0;
		for(i = 0; i < t->ctllen; i++)
			if(t->ctlbuf[i] == '\n')
				break;
		if(i < t->ctllen){
			t->ctlbuf[i] = 0;
			complete = 1;
			snprint(t->lastctl, sizeof t->lastctl, "%s", t->ctlbuf);
			p = t->ctlbuf;
			nf = tokenize(p, f, nelem(f));
			if(nf >= 1 && strcmp(f[0], "clk") == 0 && nf == 4){
				u32 mux, dtsen, dtadc, m, v;

				mux = strtoul(f[1], nil, 0);
				dtsen = strtoul(f[2], nil, 0);
				dtadc = strtoul(f[3], nil, 0);
				m = (3 << 4) | 7 | (0x7f << 8);
				v = ((mux & 3) << 4) | (dtsen & 7) |
				    ((dtadc & 0x7f) << 8);
				drw(&t->cru, CruSel51, (m << 16) | v);
				ok = 1;
			}else if(nf == 2 && strcmp(f[0], "ac") == 0){
				drw(&t->ts, TsAutoCon, strtoul(f[1], nil, 0));
				ok = 1;
			}else if(nf == 2 && strcmp(f[0], "uc") == 0){
				drw(&t->ts, TsUserCon, strtoul(f[1], nil, 0));
				ok = 1;
			}else if(nf == 3 && strcmp(f[0], "reg") == 0){
				drw(&t->ts, strtoul(f[1], nil, 0),
					strtoul(f[2], nil, 0));
				ok = 1;
			}else if(nf == 3 && strcmp(f[0], "cru") == 0){
				drw(&t->cru, strtoul(f[1], nil, 0),
					strtoul(f[2], nil, 0));
				ok = 1;
			}else if(nf == 3 && strcmp(f[0], "grf") == 0){
				drw(&t->grf, strtoul(f[1], nil, 0),
					strtoul(f[2], nil, 0));
				ok = 1;
			}
			t->ctllen = 0;
			t->ctlbuf[0] = 0;
		}
	}
	qunlock(&t->lk);

	if(complete && !ok){
		respond(r, "ctl: clk/ac/uc/reg/cru/grf");
		return;
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static DrvFile files[] = {
	{ "temp0",	0444,	&ts,	fsread,	nil },
	{ "temp1",	0444,	&ts,	fsread,	nil },
	{ "status",	0444,	&ts,	fsread,	nil },
	{ "ctl",	0222,	&ts,	nil,	fswrite },
};

void
main(int argc, char **argv)
{
	DrvSrv *s;

	USED(argc);
	USED(argv);
	if(drvfind(&ts.ts, "tsadc", 1) < 0)
		drvfail("tsadcfs: no TSADC in #P");
	if(drvmap(&ts.ts) < 0)
		drvfail("tsadcfs: cannot map %s: %r", ts.ts.name);
	if(drvfindreg(&ts.grf, 0xfdc60000ULL) < 0 || drvmap(&ts.grf) < 0)
		drvfail("tsadcfs: no GRF at fdc60000");
	if(drvfindreg(&ts.cru, 0xfdd20000ULL) < 0 || drvmap(&ts.cru) < 0)
		drvfail("tsadcfs: no CRU at fdd20000");
	tsinit(&ts);

	s = drvsrv("tsadc", files, nelem(files));
	if(s == nil)
		drvfail("tsadcfs: cannot build 9P tree");
	drvsrvpost(s);
	drvhealthrun("tsadcfs");
}
