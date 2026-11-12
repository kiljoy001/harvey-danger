/* saradcfs: the RK3566 SARADC as a 9P file server.
 *
 * The v1 block is a single 10-bit converter with eight channel selects:
 *
 *	DATA	0x00	conversion result
 *	STAS	0x04	bit 0 = busy
 *	CTRL	0x08	bit 3 power, bits 0-2 channel, bit 5 irq enable
 *	DLY	0x0c	power-up delay in clock periods
 *
 * A conversion writes the power-up delay, writes power + channel, polls
 * busy clear, reads DATA and powers down.  The vendor driver uses the
 * completion interrupt; polling is equivalent for a low-rate ADC.
 *
 *	/chan0 .. /chan7	read	"raw millivolts"
 *	/status			read	reference, conversions, errors
 *	/ctl			write	"vref <millivolts>"
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"

enum {
	AdcData		= 0x00,
	AdcStas		= 0x04,
	AdcCtrl		= 0x08,
	AdcDly		= 0x0c,

	AdcBusy		= 1 << 0,
	AdcPower	= 1 << 3,
	AdcChnMask	= 0x7,
	AdcMaxChan	= 7,
	AdcBits		= 10,
	AdcFull		= (1 << AdcBits) - 1,

	PollTimeout	= 100 * 1000 * 1000LL,	/* 100 ms */
};

typedef struct Adc Adc;
struct Adc {
	Drv	d;
	QLock	lk;
	int	vref;		/* millivolts */
	uvlong	conversions;
	ulong	errors;
	ulong	ctlwrites;
	char	lastctl[32];
	char	ctlbuf[64];
	int	ctllen;
};

static Adc adc;

static int
adcread(Adc *a, int ch, int *raw)
{
	Drv *d;
	vlong deadline;
	u32 stas;

	if(ch < 0 || ch > AdcMaxChan)
		return -1;
	d = &a->d;
	qlock(&a->lk);
	drw(d, AdcDly, 8);
	drw(d, AdcCtrl, AdcPower | (ch & AdcChnMask));

	deadline = nsec() + PollTimeout;
	for(;;){
		stas = drd(d, AdcStas);
		if((stas & AdcBusy) == 0)
			break;
		if(nsec() > deadline){
			drw(d, AdcCtrl, 0);
			a->errors++;
			qunlock(&a->lk);
			return -1;
		}
	}
	*raw = drd(d, AdcData) & AdcFull;
	drw(d, AdcCtrl, 0);
	a->conversions++;
	qunlock(&a->lk);
	return 0;
}

static void
fsread(Req *r, void *aux)
{
	Adc *a;
	char *name;
	char buf[128];
	int ch, raw, mv;

	a = aux;
	name = drvfile(r)->name;
	if(strncmp(name, "chan", 4) == 0){
		ch = atoi(name + 4);
		if(adcread(a, ch, &raw) < 0){
			respond(r, "saradc: conversion failed");
			return;
		}
		mv = (int)((vlong)raw * a->vref / (AdcFull + 1));
		snprint(buf, sizeof buf, "%d %d\n", raw, mv);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	if(strcmp(name, "status") == 0){
		snprint(buf, sizeof buf,
			"rockchip,rk3568-saradc\n"
			"vref %d\n"
			"conversions %llud\n"
			"errors %lud\n"
			"ctlwrites %lud\n"
			"lastctl %s\n",
			a->vref, a->conversions, a->errors,
			a->ctlwrites, a->lastctl);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	respond(r, "read not permitted");
}

static void
fswrite(Req *r, void *aux)
{
	Adc *a;
	char *name, *p;
	char buf[64];
	int i, mv, n, complete, ok;

	a = aux;
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
	mv = 0;
	qlock(&a->lk);
	a->ctlwrites++;
	if(n > 0 && a->ctllen + n < (int)sizeof a->ctlbuf){
		memmove(a->ctlbuf + a->ctllen, buf, n);
		a->ctllen += n;
		a->ctlbuf[a->ctllen] = 0;
		for(i = 0; i < a->ctllen; i++)
			if(a->ctlbuf[i] == '\n')
				break;
		if(i < a->ctllen){
			a->ctlbuf[i] = 0;
			complete = 1;
			snprint(a->lastctl, sizeof a->lastctl, "%s", a->ctlbuf);
			p = a->ctlbuf;
			if(strncmp(p, "vref", 4) == 0){
				p += 4;
				while(*p == ' ')
					p++;
				mv = atoi(p);
				if(mv >= 100 && mv <= 5000){
					a->vref = mv;
					ok = 1;
				}
			}
			a->ctllen = 0;
			a->ctlbuf[0] = 0;
		}
	}
	qunlock(&a->lk);

	if(complete && !ok){
		respond(r, "ctl: vref <millivolts>");
		return;
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static DrvFile files[] = {
	{ "chan0",	0444,	&adc,	fsread,	nil },
	{ "chan1",	0444,	&adc,	fsread,	nil },
	{ "chan2",	0444,	&adc,	fsread,	nil },
	{ "chan3",	0444,	&adc,	fsread,	nil },
	{ "chan4",	0444,	&adc,	fsread,	nil },
	{ "chan5",	0444,	&adc,	fsread,	nil },
	{ "chan6",	0444,	&adc,	fsread,	nil },
	{ "chan7",	0444,	&adc,	fsread,	nil },
	{ "status",	0444,	&adc,	fsread,	nil },
	{ "ctl",	0222,	&adc,	nil,	fswrite },
};

void
main(int argc, char **argv)
{
	DrvSrv *s;

	USED(argc);
	USED(argv);
	if(drvfind(&adc.d, "saradc", 1) < 0)
		drvfail("saradcfs: no SARADC in #P");
	if(drvmap(&adc.d) < 0)
		drvfail("saradcfs: cannot map %s: %r", adc.d.name);
	adc.vref = 1800;	/* vcca_1v8 on the Orange Pi 3B */

	s = drvsrv("saradc", files, nelem(files));
	if(s == nil)
		drvfail("saradcfs: cannot build 9P tree");
	drvsrvpost(s);
	drvhealthrun("saradcfs");
}
