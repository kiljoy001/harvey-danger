/*
 * gmacfs: 9P service for the RK3566 GMAC.
 *
 *   /ctl   link, address and counters (read)
 *   /data  raw Ethernet frames: read returns the next received frame,
 *          write transmits one
 *
 * The service also answers ARP and ICMP echo for its IPv4 address, so
 * the board is pingable without any IP stack on the client side.  On a
 * v1.1 board the link runs at 10 Mbps: that is the only speed this PHY
 * passes traffic at (U-Boot fails at 100 and 1000 too).
 *
 * The driver core is ../gmac/gmac.c, built with -DGMACFS_LIB.
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
static u32 ipaddr = 0xc0a802f8;		/* 192.168.2.248 */
static u64 nrx, ntx, narp, nicmp;
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

static u32
get32(u8 *p)
{
	return (p[0]<<24) | (p[1]<<16) | (p[2]<<8) | p[3];
}

static void
put32(u8 *p, u32 v)
{
	p[0] = v>>24; p[1] = v>>16; p[2] = v>>8; p[3] = v;
}

static void
put16(u8 *p, u16 v)
{
	p[0] = v>>8; p[1] = v;
}

static u16
cksum(u8 *p, int n)
{
	u32 s = 0;
	int i;

	for(i = 0; i + 1 < n; i += 2)
		s += (p[i]<<8) | p[i+1];
	if(n & 1)
		s += p[n-1] << 8;
	while(s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return ~s & 0xffff;
}

static void
framesend(u8 *f, int n)
{
	qlock(&cardlk);
	if(gmactx(f, n) < 0)
		print("gmacfs: tx failed\n");
	else
		ntx++;
	qunlock(&cardlk);
}

/* Announce ipaddr/mac so switches and hosts replace stale bindings. */
static void
garp(void)
{
	u8 f[64];
	uchar *m = gmacaddr();

	memset(f, 0, sizeof f);
	memset(f, 0xff, 6);
	memmove(f+6, m, 6);
	f[12] = 0x08; f[13] = 0x06;
	f[14] = 0; f[15] = 1;
	f[16] = 0x08; f[17] = 0;
	f[18] = 6; f[19] = 4;
	f[20] = 0; f[21] = 1;			/* request, sender = target */
	memmove(f+22, m, 6);
	put32(f+28, ipaddr);
	memset(f+32, 0, 6);
	put32(f+38, ipaddr);
	framesend(f, 42);
	narp++;
}

static void
arphandler(u8 *f, int n)
{
	u8 r[64];
	uchar *mac = gmacaddr();

	if(n < 42 || f[20] != 0 || f[21] != 1)		/* request only */
		return;
	if(get32(f+38) != ipaddr)			/* target address */
		return;
	memmove(r, f+6, 6);				/* dst = asker */
	memmove(r+6, mac, 6);
	r[12] = 0x08; r[13] = 0x06;
	r[14] = 0; r[15] = 1;				/* ethernet */
	r[16] = 0x08; r[17] = 0;			/* IPv4 */
	r[18] = 6; r[19] = 4;
	r[20] = 0; r[21] = 2;				/* reply */
	memmove(r+22, mac, 6);
	put32(r+28, ipaddr);
	memmove(r+32, f+22, 6);
	put32(r+38, get32(f+28));
	framesend(r, 42);
	narp++;
}

static void
icmphandler(u8 *f, int n)
{
	u8 r[Maxframe];
	u8 *ip, *ic;
	int ihl, iplen, iclen;

	if(n < 34 || f[12] != 0x08 || f[13] != 0x00)
		return;
	ip = f + 14;
	if(get32(ip+16) != ipaddr)			/* destination */
		return;
	ihl = (ip[0] & 0xf) * 4;
	if(ihl < 20 || n < 14 + ihl + 8 || ip[9] != 1)	/* ICMP */
		return;
	ic = ip + ihl;
	if(ic[0] != 8)					/* echo request */
		return;
	iplen = (ip[2]<<8) | ip[3];
	if(iplen < ihl + 8 || 14 + iplen > n)
		return;

	memmove(r, f, 14 + iplen);
	memmove(r, f+6, 6);				/* swap MACs */
	memmove(r+6, f, 6);
	memmove(r+26, f+30, 4);				/* swap IPs */
	memmove(r+30, f+26, 4);
	r[14+ihl] = 0;					/* echo reply */
	put16(r+24, 0);					/* IP checksum */
	put16(r+14+ihl+2, 0);				/* ICMP checksum */
	put16(r+24, cksum(r+14, ihl));
	iclen = iplen - ihl;
	put16(r+14+ihl+2, cksum(r+14+ihl, iclen));
	framesend(r, 14 + iplen);
	nicmp++;
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
			"ip %d.%d.%d.%d\n"
			"rx %llud tx %llud arp %llud icmp %llud\n"
			"dma %08ux %08ux rxcur %08ux txcur %08ux\n"
			"ring %llux idx %d desc %08ux loops %llud empty %llud\n",
			gmaclink(),
			m[0], m[1], m[2], m[3], m[4], m[5],
			ipaddr>>24, (ipaddr>>16)&0xff, (ipaddr>>8)&0xff,
			ipaddr&0xff, nrx, ntx, narp, nicmp,
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

/* Poll the receiver and answer ARP/ICMP; everything else is queued. */
static void
serve(void)
{
	static u8 buf[Maxframe];
	int n;

	for(;;){
		/* drain everything the ring holds before sleeping */
		loops++;
		if((loops % 3000) == 1)
			garp();
		for(;;){
			qlock(&cardlk);
			n = gmacrx(buf, sizeof buf);
			qunlock(&cardlk);
			if(n <= 0){
				empties++;
				break;
			}
			nrx++;
			if(n >= 42 && buf[12] == 0x08 && buf[13] == 0x06)
				arphandler(buf, n);
			else if(n >= 34 && buf[12] == 0x08 && buf[13] == 0x00){
				int before = nicmp;

				icmphandler(buf, n);
				if(nicmp == before)
					queue(buf, n);
			}else
				queue(buf, n);
		}
		sleep(1);
	}
}

void
main(int argc, char **argv)
{
	File *f;

	USED(argc);
	USED(argv);
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

	garp();
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
