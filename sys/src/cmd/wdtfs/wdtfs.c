/* wdtfs: the RK3566 DesignWare watchdog as a 9P file server.
 *
 * The kernel disables the watchdog early (CR=0, CRR=0x76) so a stuck boot
 * does not reset the board.  This service lets the system inspect it and
 * choose a policy: report the current state, disable it explicitly, ping it,
 * or enable it with a raw timeout-range index (TORR 0..15).
 *
 * The DesignWare block has sixteen timeout periods derived from its clock,
 * so "enable <torr>" takes the raw index rather than pretending to know the
 * clock rate.  /status reports the decoded fields so the rate can be derived
 * on the board if needed.
 *
 *	/status	read	CR, TORR, CCVR, STAT, version/type/params
 *	/ctl	write	"disable", "ping", "enable [torr]"
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"

enum {
	WdtCr		= 0x00,
	WdtTorr		= 0x04,
	WdtCcvr		= 0x08,
	WdtCrr		= 0x0c,
	WdtStat		= 0x10,
	WdtEoi		= 0x14,
	WdtParams1	= 0xf4,
	WdtVersion	= 0xf8,
	WdtType		= 0xfc,

	CrEn		= 1 << 0,
	CrRmod		= 1 << 1,	/* 1 = IRQ, 0 = reset */
	CrRpl		= 0x3 << 2,	/* reset pulse length */

	CrrMagic	= 0x76,
	TorrMask	= 0xf,
};

typedef struct Wdt Wdt;
struct Wdt {
	Drv	d;
	QLock	lk;
	ulong	pings;
	char	ctlbuf[64];
	int	ctllen;
};

static Wdt wdt;

static void
fsread(Req *r, void *aux)
{
	Wdt *w;
	char buf[512];
	u32 cr, torr, ccvr, stat, p1, ver, type;

	w = aux;
	if(strcmp(drvfile(r)->name, "status") != 0){
		respond(r, "read not permitted");
		return;
	}
	qlock(&w->lk);
	cr = drd(&w->d, WdtCr);
	torr = drd(&w->d, WdtTorr);
	ccvr = drd(&w->d, WdtCcvr);
	stat = drd(&w->d, WdtStat);
	p1 = drd(&w->d, WdtParams1);
	ver = drd(&w->d, WdtVersion);
	type = drd(&w->d, WdtType);
	qunlock(&w->lk);

	snprint(buf, sizeof buf,
		"rockchip,rk3568-wdt\n"
		"cr %#ux\n"
		"enabled %d\n"
		"rmod %s\n"
		"rpl %d\n"
		"torr %d\n"
		"ccvr %ud\n"
		"stat %#ux\n"
		"fix_top %d\n"
		"version %#ux\n"
		"type %#ux\n"
		"pings %lud\n",
		cr, (cr & CrEn) != 0,
		(cr & CrRmod) ? "irq" : "reset",
		(cr >> 2) & 3,
		torr & TorrMask,
		ccvr, stat, (p1 >> 6) & 1, ver, type, w->pings);
	readstr(r, buf);
	respond(r, nil);
}

static void
fswrite(Req *r, void *aux)
{
	Wdt *w;
	char *name, *p;
	char buf[64];
	int i, n, complete, ok, torr;

	w = aux;
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
	qlock(&w->lk);
	if(n > 0 && w->ctllen + n < (int)sizeof w->ctlbuf){
		memmove(w->ctlbuf + w->ctllen, buf, n);
		w->ctllen += n;
		w->ctlbuf[w->ctllen] = 0;
		for(i = 0; i < w->ctllen; i++)
			if(w->ctlbuf[i] == '\n')
				break;
		if(i < w->ctllen){
			w->ctlbuf[i] = 0;
			complete = 1;
			p = w->ctlbuf;
			if(strncmp(p, "disable", 7) == 0){
				drw(&w->d, WdtCr, 0);
				drw(&w->d, WdtCrr, CrrMagic);
				ok = 1;
			}else if(strncmp(p, "ping", 4) == 0){
				drw(&w->d, WdtCrr, CrrMagic);
				w->pings++;
				ok = 1;
			}else if(strncmp(p, "enable", 6) == 0){
				p += 6;
				while(*p == ' ')
					p++;
				torr = atoi(p) & TorrMask;
				drw(&w->d, WdtTorr, torr);
				drw(&w->d, WdtCr, CrEn);
				drw(&w->d, WdtCrr, CrrMagic);
				ok = 1;
			}
			w->ctllen = 0;
			w->ctlbuf[0] = 0;
		}
	}
	qunlock(&w->lk);

	if(complete && !ok){
		respond(r, "ctl: disable, ping, or enable [torr]");
		return;
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static DrvFile files[] = {
	{ "status",	0444,	&wdt,	fsread,	nil },
	{ "ctl",	0222,	&wdt,	nil,	fswrite },
};

void
main(int argc, char **argv)
{
	DrvSrv *s;

	USED(argc);
	USED(argv);
	if(drvfind(&wdt.d, "wdt", 1) < 0)
		drvfail("wdtfs: no watchdog in #P");
	if(drvmap(&wdt.d) < 0)
		drvfail("wdtfs: cannot map %s: %r", wdt.d.name);

	s = drvsrv("wdt", files, nelem(files));
	if(s == nil)
		drvfail("wdtfs: cannot build 9P tree");
	drvsrvpost(s);
	drvhealthrun("wdtfs");
}
