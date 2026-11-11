/* rngfs: the RK3566 Crypto V2 TRNG as a 9P file server.
 *
 * The controller is the "rockchip,cryptov2-rng" block at fe388000.  A read
 * of /random generates fresh bytes from the ring oscillator; the kernel's
 * DRBG is not involved.  The vendor sequence is:
 *
 *	RNG_SAMPLE_CNT = 100
 *	RNG_CTL = 256-bit | slower ring 0 | enable | start   (write-masked)
 *	poll RNG_CTL.START until clear
 *	read eight big-endian words from RNG_DOUT_0..7
 *	RNG_CTL = 0
 *
 * One generated batch is 32 bytes (RK_MAX_RNG_BYTE), so larger reads loop.
 *
 *	/random	read	32-byte batches, as many as the read asks for
 *	/status	read	controller, bytes generated, errors
 *	/ctl	write	"flush" stops the generator
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"

enum {
	RngCtl		= 0x400,
	RngSample	= 0x404,
	RngDout		= 0x410,	/* eight 32-bit words */
	RngBatch	= 32,
	RngWords	= RngBatch / 4,

	/* RNG_CTL fields: 256-bit length, slower ring 0, enable, start. */
	CtlLen256	= 3 << 4,
	CtlRing0	= 1 << 2,
	CtlEnable	= 1 << 1,
	CtlStart	= 1 << 0,
	CtlHiMask	= 0xffff0000,

	PollTimeout	= 50 * 1000 * 1000LL,	/* 50 ms */
};

typedef struct Rng Rng;
struct Rng {
	Drv	d;
	QLock	lk;
	uvlong	bytes;
	ulong	errors;
};

static Rng rng;

static u32
be32(u32 v)
{
	return (v >> 24) | ((v >> 8) & 0xff00) |
	       ((v << 8) & 0xff0000) | (v << 24);
}

/* One 32-byte batch.  Returns the number of bytes written, or -1. */
static int
rngbatch(Rng *r, uchar *buf, int n)
{
	Drv *d;
	vlong deadline;
	u32 ctl;
	int i;

	d = &r->d;
	if(n > RngBatch)
		n = RngBatch;

	drw(d, RngSample, 100);
	drw(d, RngCtl, CtlHiMask | CtlLen256 | CtlRing0 | CtlEnable | CtlStart);

	deadline = nsec() + PollTimeout;
	for(;;){
		ctl = drd(d, RngCtl);
		if((ctl & CtlStart) == 0)
			break;
		if(nsec() > deadline){
			drw(d, RngCtl, CtlHiMask);
			r->errors++;
			return -1;
		}
	}
	for(i = 0; i < RngWords; i++){
		u32 v = be32(drd(d, RngDout + i*4));
		int off = i * 4;
		int left = n - off;

		if(left <= 0)
			break;
		if(left > 4)
			left = 4;
		memmove(buf + off, &v, left);
	}
	drw(d, RngCtl, CtlHiMask);
	r->bytes += n;
	return n;
}

static int
rngfill(Rng *r, uchar *buf, int n)
{
	int done, m;

	qlock(&r->lk);
	for(done = 0; done < n; done += m){
		m = rngbatch(r, buf + done, n - done);
		if(m <= 0){
			qunlock(&r->lk);
			return done > 0 ? done : -1;
		}
	}
	qunlock(&r->lk);
	return done;
}

static void
fsread(Req *r, void *aux)
{
	Rng *g;
	char *name;
	char buf[256];
	int n;

	g = aux;
	name = drvfile(r)->name;
	if(strcmp(name, "random") == 0){
		if(r->ifcall.count == 0){
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		n = r->ifcall.count;
		if(n > 4096)
			n = 4096;
		n = rngfill(g, (uchar *)r->ofcall.data, n);
		if(n < 0){
			respond(r, "rng: generate failed");
			return;
		}
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}
	if(strcmp(name, "status") == 0){
		snprint(buf, sizeof buf, "rockchip,cryptov2-rng\nbytes %llud\nerrors %lud\n",
			g->bytes, g->errors);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	respond(r, "read not permitted");
}

static void
fswrite(Req *r, void *aux)
{
	Rng *g;
	char *name;

	g = aux;
	name = drvfile(r)->name;
	if(strcmp(name, "ctl") == 0){
		if(strncmp((char *)r->ifcall.data, "flush", 5) == 0)
			drw(&g->d, RngCtl, CtlHiMask);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	respond(r, "write not permitted");
}

static DrvFile files[] = {
	{ "random",	0444,	&rng,	fsread,	nil },
	{ "status",	0444,	&rng,	fsread,	nil },
	{ "ctl",	0222,	&rng,	nil,	fswrite },
};

void
main(int argc, char **argv)
{
	DrvSrv *s;

	USED(argc);
	USED(argv);
	if(drvfind(&rng.d, "rng", 1) < 0)
		drvfail("rngfs: no RNG in #P");
	if(drvmap(&rng.d) < 0)
		drvfail("rngfs: cannot map %s: %r", rng.d.name);

	s = drvsrv("rng", files, nelem(files));
	if(s == nil)
		drvfail("rngfs: cannot build 9P tree");
	drvsrvpost(s);
	drvhealthrun("rngfs");
}
