/* pwmfs: the RK3566 PWM block as a 9P file server.
 *
 * Four channels sit at fe6e0000, 0x10 apart.  Each has a 32-bit counter,
 * period and duty register and a control word:
 *
 *	CNT	0x00	counter
 *	PERIOD	0x04	period in PWM clock ticks
 *	DUTY	0x08	high time in ticks
 *	CTRL	0x0c	bit0 enable, bit1 continuous, bit3 duty positive,
 *			bit6 lock period/duty while updating
 *
 * The block's clock is muxed to xin24m (24 MHz).  Channel 3 is the fan
 * header: GPIO0_C6, PMU_GRF 0x14 bits 8-11 = function 1.
 *
 *	/status		read	clock, per-channel period/duty/counter/state
 *	/fan		read	current fan duty, percent
 *	/ctl		write	enable <ch> <0|1>
 *				period <ch> <nanoseconds>
 *				duty <ch> <nanoseconds>
 *				fan <percent>
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"

enum {
	PwmCnt		= 0x00,
	PwmPeriod	= 0x04,
	PwmDuty		= 0x08,
	PwmCtrl		= 0x0c,

	PwmOn		= 1 << 0,
	PwmContinuous	= 1 << 1,
	PwmDutyPositive	= 1 << 3,
	PwmLock		= 1 << 6,
	PwmCntEn	= 1 << 7,

	Nchan		= 4,
	FanChan		= 3,
	ClockHz		= 24000000,
	FanPeriod	= 1200,		/* 20 kHz at 24 MHz: vendor v1.1 DTS */

	CruGatePwm1	= 0x37c,	/* CLKGATE_CON(31) */
	CruSel72	= 0x220,	/* CLKSEL_CON(72): clk_pwm1 mux */

	GrfGpio0C	= 0x14,		/* PMU_GRF GPIO0C iomux */
	Gpio0C6Shift	= 8,		/* C6 is the upper half of the C group */
	Gpio0C6Mask	= 0xf,
	Gpio0C6Pwm	= 1,
};

typedef struct Pwm Pwm;
struct Pwm {
	Drv	d;
	Drv	grf;
	Drv	cru;
	QLock	lk;
	char	ctlbuf[64];
	int	ctllen;
	char	lastctl[64];
	ulong	cmds;
};

static Pwm pwm;

static void
pwmconfig(Pwm *p, int ch, u32 period, u32 duty)
{
	u32 off, ctrl;

	if(ch < 0 || ch >= Nchan)
		return;
	if(period == 0)
		period = 1;
	if(duty > period)
		duty = period;
	off = ch * 0x10;
	ctrl = drd(&p->d, off + PwmCtrl);
	/* Lock the previous period/duty, load the new pair, then unlock. */
	drw(&p->d, off + PwmCtrl, ctrl | PwmLock);
	drw(&p->d, off + PwmPeriod, period);
	drw(&p->d, off + PwmDuty, duty);
	drw(&p->d, off + PwmCtrl, ctrl & ~PwmLock);
}

static void
pwmenable(Pwm *p, int ch, int on)
{
	u32 off, ctrl;

	if(ch < 0 || ch >= Nchan)
		return;
	off = ch * 0x10;
	ctrl = drd(&p->d, off + PwmCtrl);
	if(on)
		ctrl |= PwmOn | PwmContinuous | PwmDutyPositive | PwmCntEn;
	else
		ctrl &= ~PwmOn;
	drw(&p->d, off + PwmCtrl, ctrl);
}

static u32
nstoticks(u64 ns)
{
	return (u32)(ns * ClockHz / 1000000000ULL);
}

static void
pwmfan(Pwm *p, int pct)
{
	u32 period, duty;

	if(pct < 0)
		pct = 0;
	if(pct > 100)
		pct = 100;
	period = drd(&p->d, FanChan*0x10 + PwmPeriod);
	if(period == 0)
		period = FanPeriod;
	duty = (u32)((u64)period * pct / 100);
	pwmconfig(p, FanChan, period, duty);
	pwmenable(p, FanChan, pct > 0);
}

static void
fsread(Req *r, void *aux)
{
	Pwm *p;
	char *name, *s;
	char buf[1024];
	int ch, n;
	u32 period, duty, cnt, ctrl;

	p = aux;
	name = drvfile(r)->name;
	if(strcmp(name, "status") == 0){
		n = snprint(buf, sizeof buf,
			"rockchip,rk3568-pwm fe6e0000\n"
			"clock %d sel72 %ux gate %ux grf %ux\n"
			"grfx %ux %ux %ux %ux %ux %ux %ux %ux\n",
			ClockHz, drd(&p->cru, CruSel72),
			drd(&p->cru, CruGatePwm1), drd(&p->grf, GrfGpio0C),
			drd(&p->grf, 0x00), drd(&p->grf, 0x04),
			drd(&p->grf, 0x08), drd(&p->grf, 0x0c),
			drd(&p->grf, 0x10), drd(&p->grf, 0x14),
			drd(&p->grf, 0x18), drd(&p->grf, 0x1c));
		for(ch = 0; ch < Nchan; ch++){
			period = drd(&p->d, ch*0x10 + PwmPeriod);
			duty = drd(&p->d, ch*0x10 + PwmDuty);
			cnt = drd(&p->d, ch*0x10 + PwmCnt);
			ctrl = drd(&p->d, ch*0x10 + PwmCtrl);
			n += snprint(buf+n, sizeof buf-n,
				"ch%d period %ud duty %ud cnt %ud ctrl %ux %s\n",
				ch, period, duty, cnt, ctrl,
				(ctrl & PwmOn) ? "on" : "off");
		}
		n += snprint(buf+n, sizeof buf-n,
			"cmds %lud lastctl %s\n", p->cmds, p->lastctl);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	if(strcmp(name, "fan") == 0){
		period = drd(&p->d, FanChan*0x10 + PwmPeriod);
		duty = drd(&p->d, FanChan*0x10 + PwmDuty);
		ctrl = drd(&p->d, FanChan*0x10 + PwmCtrl);
		if(period == 0)
			ch = 0;
		else
			ch = (int)((u64)duty * 100 / period);
		snprint(buf, sizeof buf, "%d\n", ch);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	USED(s);
	respond(r, "read not permitted");
}

static void
fswrite(Req *r, void *aux)
{
	Pwm *p;
	char *name, *s, *f[5];
	char buf[64];
	int i, n, nf, complete, ok, ch, val;
	u32 ns;

	p = aux;
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
	qlock(&p->lk);
	p->cmds++;
	if(n > 0 && p->ctllen + n < (int)sizeof p->ctlbuf){
		memmove(p->ctlbuf + p->ctllen, buf, n);
		p->ctllen += n;
		p->ctlbuf[p->ctllen] = 0;
		for(i = 0; i < p->ctllen; i++)
			if(p->ctlbuf[i] == '\n')
				break;
		if(i < p->ctllen){
			p->ctlbuf[i] = 0;
			complete = 1;
			snprint(p->lastctl, sizeof p->lastctl, "%s", p->ctlbuf);
			s = p->ctlbuf;
			nf = tokenize(s, f, nelem(f));
			if(nf == 3 && strcmp(f[0], "enable") == 0){
				ch = atoi(f[1]);
				val = atoi(f[2]);
				pwmenable(p, ch, val != 0);
				ok = 1;
			}else if(nf == 3 && strcmp(f[0], "period") == 0){
				ch = atoi(f[1]);
				ns = strtoul(f[2], nil, 0);
				if(ch >= 0 && ch < Nchan){
					pwmconfig(p, ch, nstoticks(ns),
						drd(&p->d, ch*0x10 + PwmDuty));
					ok = 1;
				}
			}else if(nf == 3 && strcmp(f[0], "duty") == 0){
				ch = atoi(f[1]);
				ns = strtoul(f[2], nil, 0);
				if(ch >= 0 && ch < Nchan){
					pwmconfig(p, ch,
						drd(&p->d, ch*0x10 + PwmPeriod),
						nstoticks(ns));
					ok = 1;
				}
			}else if(nf == 3 && strcmp(f[0], "ctrl") == 0){
				ch = atoi(f[1]);
				if(ch >= 0 && ch < Nchan){
					drw(&p->d, ch*0x10 + PwmCtrl,
						strtoul(f[2], nil, 0));
					ok = 1;
				}
			}else if(nf == 3 && strcmp(f[0], "grf") == 0){
				drw(&p->grf, strtoul(f[1], nil, 0),
					strtoul(f[2], nil, 0));
				ok = 1;
			}else if(nf == 2 && strcmp(f[0], "fan") == 0){
				pwmfan(p, atoi(f[1]));
				ok = 1;
			}
			p->ctllen = 0;
			p->ctlbuf[0] = 0;
		}
	}
	qunlock(&p->lk);

	if(complete && !ok){
		respond(r, "ctl: enable/period/duty/fan");
		return;
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static DrvFile files[] = {
	{ "status",	0444,	&pwm,	fsread,	nil },
	{ "fan",	0444,	&pwm,	fsread,	nil },
	{ "ctl",	0222,	&pwm,	nil,	fswrite },
};

void
main(int argc, char **argv)
{
	DrvSrv *s;
	u32 v;

	USED(argc);
	USED(argv);
	if(drvfindreg(&pwm.d, 0xfe6e0000ULL) < 0){
		if(drvfind(&pwm.d, "pwm", 0) < 0)
			drvfail("pwmfs: no PWM in #P");
	}
	if(drvmap(&pwm.d) < 0)
		drvfail("pwmfs: cannot map %s: %r", pwm.d.name);
	if(drvfindreg(&pwm.cru, 0xfdd20000ULL) < 0 || drvmap(&pwm.cru) < 0)
		drvfail("pwmfs: no CRU at fdd20000");
	if(drvfindreg(&pwm.grf, 0xfdc20000ULL) < 0 || drvmap(&pwm.grf) < 0)
		drvfail("pwmfs: no PMU GRF at fdc20000");

	/* pclk_pwm1 and clk_pwm1 in CLKGATE_CON(31), bits 10 and 11. */
	drw(&pwm.cru, CruGatePwm1, 0x0C000000);
	/* clk_pwm1 mux = xin24m (1 of gpll100/xin24m/cpll100). */
	drw(&pwm.cru, CruSel72, 0x03000100);

	/*
	 * GPIO0_C6 to PWM7 (function 1).  The RK3568 PMU_GRF iomux
	 * registers take the write mask in the upper half and the value in
	 * the lower half; a plain read-modify-write is silently ignored.
	 */
	v = (Gpio0C6Mask << (Gpio0C6Shift + 16)) |
	    (Gpio0C6Pwm << Gpio0C6Shift);
	drw(&pwm.grf, GrfGpio0C, v);

	s = drvsrv("pwm", files, nelem(files));
	if(s == nil)
		drvfail("pwmfs: cannot build 9P tree");
	drvsrvpost(s);
	drvhealthrun("pwmfs");
}
