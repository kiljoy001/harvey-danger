/*
 * gmacfs: 9P service for the RK3566 GMAC.
 *
 *   /ctl   link, address and counters (read)
 *   /data  raw Ethernet frames: read returns the next received frame,
 *          write transmits one
 *
 * This is a raw-frame client.  It does not speak ARP or ICMP: the
 * kernel Ether driver (sys/src/9/aarch64/ethergmac.c) owns the card in
 * normal operation and the IP stack lives in /net.  gmacfs is kept for
 * experiments that want whole frames, and for boards booted without
 * the kernel driver.
 *
 * On a v1.1 board the link runs at 10 Mbps: that is the only speed this
 * PHY passes traffic at (U-Boot fails at 100 and 1000 too).  The driver
 * core is ../gmac/gmac.c, built with -DGMACFS_LIB.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include "../gmac/gmac.h"

enum {
	Maxframe	= 2048,
	Nring		= 16,
	Qctl,
	Qdata,
};

static void fsread(Req *);
static void fswrite(Req *);

static Srv fs = {
	.read = fsread,
	.write = fswrite,
};

static QLock cardlk, ringlk;
static u8 ring[Nring][Maxframe];
static int ringlen[Nring], ringhead, ringtail;
static u64 nrx, ntx;
static u64 loops, empties;

static void
destroyed(File *f)
{
	USED(f);
}

static void
queue(u8 *f, int n)
{
	qlock(&ringlk);
	if(ringlen[ringhead] != 0)
		ringtail = (ringtail + 1) % Nring;	/* drop the oldest */
	memmove(ring[ringhead], f, n);
	ringlen[ringhead] = n;
	ringhead = (ringhead + 1) % Nring;
	qunlock(&ringlk);
}

static int
dequeue(u8 *f, int max)
{
	int n;

	qlock(&ringlk);
	if(ringlen[ringtail] == 0){
		qunlock(&ringlk);
		return 0;
	}
	n = ringlen[ringtail];
	if(n > max)
		n = max;
	memmove(f, ring[ringtail], n);
	ringlen[ringtail] = 0;
	ringtail = (ringtail + 1) % Nring;
	qunlock(&ringlk);
	return n;
}

static void
fsread(Req *r)
{
	static u8 f[Maxframe];
	static char buf[512];
	int n;

	if((uintptr)r->fid->file->aux == Qctl){
		uchar *m = gmacaddr();

		qlock(&cardlk);		/* gmacrxdesc does cache maintenance */
		snprint(buf, sizeof buf,
			"gmac rk3566\n"
			"link %d mbps\n"
			"mac %02x:%02x:%02x:%02x:%02x:%02x\n"
			"rx %llud tx %llud\n"
			"dma %08ux %08ux rxcur %08ux txcur %08ux\n"
			"ring %llux idx %d desc %08ux loops %llud empty %llud\n",
			gmaclink(),
			m[0], m[1], m[2], m[3], m[4], m[5],
			nrx, ntx,
			gmacreg(0x1004), gmacreg(0x1160),
			gmacreg(0x114c), gmacreg(0x1144),
			gmacrxcpa(), gmacrxidx(),
			gmacrxdesc(gmacrxidx()), loops, empties);
		qunlock(&cardlk);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	n = dequeue(f, sizeof f);
	if(n > r->ifcall.count)
		n = r->ifcall.count;
	if(n > 0)
		memmove(r->ofcall.data, f, n);
	r->ofcall.count = n;
	respond(r, nil);
}

static void
fswrite(Req *r)
{
	long n;

	if((uintptr)r->fid->file->aux != Qdata){
		respond(r, "write not permitted");
		return;
	}
	n = r->ifcall.count;
	if(n < 14 || n > Maxframe){
		respond(r, "bad frame");
		return;
	}
	qlock(&cardlk);
	if(gmactx(r->ifcall.data, n) < 0){
		qunlock(&cardlk);
		respond(r, "tx failed");
		return;
	}
	ntx++;
	qunlock(&cardlk);
	r->ofcall.count = n;
	respond(r, nil);
}

/* Poll the receiver and queue every frame. */
static void
serve(void)
{
	static u8 buf[Maxframe];
	int n;

	for(;;){
		/* drain everything the ring holds before sleeping */
		loops++;
		for(;;){
			qlock(&cardlk);
			n = gmacrx(buf, sizeof buf);
			qunlock(&cardlk);
			if(n <= 0){
				empties++;
				break;
			}
			nrx++;
			queue(buf, n);
		}
		sleep(1);
	}
}

void
main(int argc, char **argv)
{
	File *f;
	int fd, force, i;

	/*
	 * The kernel Ether driver owns the card in normal operation.
	 * Taking it over would fight the kernel's ring poller, so refuse
	 * unless -f says the kernel driver is not running.
	 */
	force = 0;
	for(i = 1; i < argc; i++)
		if(strcmp(argv[i], "-f") == 0)
			force = 1;
	if(!force && (fd = open("/net/ether0/addr", OREAD)) >= 0){
		close(fd);
		sysfatal("gmacfs: the kernel driver owns gmac (use -f)");
	}
	if(gmacinit() < 0)
		sysfatal("gmacfs: cannot bring up the MAC");

	fs.msize = 128*1024 + IOHDRSZ;
	fs.tree = alloctree(nil, nil, DMDIR|0777, destroyed);
	if(fs.tree == nil)
		sysfatal("gmacfs: alloctree: %r");
	f = createfile(fs.tree->root, "ctl", nil, 0444, (void*)Qctl);
	if(f == nil)
		sysfatal("gmacfs: createfile ctl: %r");
	f = createfile(fs.tree->root, "data", nil, 0666, (void*)Qdata);
	if(f == nil)
		sysfatal("gmacfs: createfile data: %r");

	print("gmacfs: %d mbps, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
	      gmaclink(), gmacaddr()[0], gmacaddr()[1], gmacaddr()[2],
	      gmacaddr()[3], gmacaddr()[4], gmacaddr()[5]);

	/*
	 * The server forks (lib9p needs a process for its blocking
	 * read).  Every card access, including the cache maintenance in
	 * /ctl, holds cardlk so the two processes never write the same
	 * #P/dma channel at once.
	 */
	postmountsrv(&fs, "gmac", nil, MREPL|MCREATE);
	serve();
}
