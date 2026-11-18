/*
 * Kernel virtio-net driver over virtio-mmio (#l) for aarch64.
 *
 * The FDT lists the transports and devvirtio9p.c already parses them;
 * this driver picks the one whose device id is 1 (net) and speaks the
 * same standard virtio-mmio v2 register set as the 9P device.
 *
 * This kernel has no device-interrupt dispatch, so reception is polled
 * from a kernel process and handed to etheriq(), exactly like the GMAC
 * driver.  The host side does not need an interrupt either: it fills
 * the RX ring from a thread and the guest notices on its next poll.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "../port/netif.h"
#include "etherif.h"
#include "fdt.h"

enum {
	TypNet = 1,
	VnetMagic = 0x74726976,

	Vmagic = 0x000, Vversion = 0x004, Vdevice = 0x008,
	Vdevfeatures = 0x010, Vdevselect = 0x014,
	Vdrvfeatures = 0x020, Vdrvselect = 0x024,
	Vqueuesel = 0x030, Vqueuemax = 0x034, Vqueuenum = 0x038,
	Vqueueready = 0x044,
	Vnotify = 0x050, Vinterrupt = 0x060, Vinterruptack = 0x064,
	Vstatus = 0x070,
	Vdescaddr = 0x080, Vavailaddr = 0x090, Vusedaddr = 0x0a0,
	Vconfig = 0x100,

	Acknowledge = 1, Driver = 2, DriverOk = 4, FeaturesOk = 8,

	Fmac = 1 << 5,

	Qsize = 64,
	Framesize = 2048,
	Vhdr = 12,	/* virtio_net_hdr */

	Rxq = 0,
	Txq = 1,

	Write = 2,	/* VRING_DESC_F_WRITE */
	Next = 1,	/* VRING_DESC_F_NEXT */
};

typedef struct Vq Vq;
struct Vq {
	int n;
	u16 size;
	u16 availidx;
	u16 usedidx;
	u64 desc;
	u64 avail;
	u64 used;
	u8 *descva;
	u8 *availva;
	u8 *usedva;
};

typedef struct Ctlr Ctlr;
struct Ctlr {
	u8 *mmio;
	Vq rx, tx;
	u8 mac[6];
	u8 *rxbuf[Qsize];
	u8 *txbuf[Qsize];
	int txnext;
	QLock lk;
	Ether *edev;
	int started;
};

static Ctlr ctlr;
static int lastscan = -1;	/* devether probes once per controller */

extern int arm64virtioapatures(Fdtvirtio *, int);

static u32
mmread(u8 *m, int off)
{
	return *(volatile u32 *)(m + off);
}

static void
mmwrite(u8 *m, int off, u32 v)
{
	*(volatile u32 *)(m + off) = v;
}

static void
mmaddr(u8 *m, int off, u64 a)
{
	mmwrite(m, off, (u32)a);
	mmwrite(m, off + 4, (u32)(a >> 32));
}

static int
vqinit(Ctlr *c, Vq *q, int index, int size)
{
	u64 pa;

	q->n = index;
	q->size = size;
	q->descva = mallocz((usize)size * 16, 1);
	q->availva = mallocz(4 + (usize)size * 2 + 2, 1);
	q->usedva = mallocz(4 + (usize)size * 8 + 2, 1);
	if(q->descva == nil || q->availva == nil || q->usedva == nil)
		return -1;
	q->desc = PADDR(q->descva);
	q->avail = PADDR(q->availva);
	q->used = PADDR(q->usedva);
	q->availidx = 0;
	q->usedidx = 0;

	mmwrite(c->mmio, Vqueuesel, (u32)index);
	mmwrite(c->mmio, Vqueuenum, (u32)size);
	mmaddr(c->mmio, Vdescaddr, q->desc);
	mmaddr(c->mmio, Vavailaddr, q->avail);
	mmaddr(c->mmio, Vusedaddr, q->used);
	mmwrite(c->mmio, Vqueueready, 1);
	return 0;
}

static void
postrx(Ctlr *c, int slot)
{
	Vq *q = &c->rx;
	u8 *d = q->descva + (usize)slot * 16;
	u64 pa = PADDR(c->rxbuf[slot]);
	u16 av;

	*(u64 *)d = pa;
	*(u32 *)(d + 8) = Framesize;
	*(u16 *)(d + 12) = Write;
	*(u16 *)(d + 14) = 0;

	av = q->availidx % q->size;
	*(u16 *)(q->availva + 4 + (usize)av * 2) = (u16)slot;
	coherence();
	*(u16 *)(q->availva + 2) = q->availidx + 1;
	q->availidx++;
}

static void
kick(Ctlr *c, int q)
{
	mmwrite(c->mmio, Vnotify, (u32)q);
}

static void
rxpoll(void)
{
	Ctlr *c = &ctlr;
	Vq *q = &c->rx;
	int refilled = 0;

	if(!c->started || !canqlock(&c->lk))
		return;
	for(;;){
		u16 idx = *(u16 *)(q->usedva + 2);
		u16 slot;
		u32 len;
		Block *bp;
		u8 *d;

		if(q->usedidx == idx)
			break;
		slot = q->usedidx % q->size;
		len = *(u32 *)(q->usedva + 4 + (usize)slot * 8 + 4);
		slot = (u16)*(u32 *)(q->usedva + 4 + (usize)slot * 8);
		q->usedidx++;
		if(slot >= q->size){
			refilled = 1;
			continue;
		}
		if(len > Vhdr && len <= Framesize){
			bp = allocb((int)(len - Vhdr));
			if(bp != nil){
				d = c->rxbuf[slot];
				memmove(bp->wp, d + Vhdr, len - Vhdr);
				bp->wp += len - Vhdr;
				etheriq(c->edev, bp, 1);
			}else
				c->edev->Netif.overflows++;
		}else
			c->edev->Netif.misses++;
		postrx(c, slot);
		refilled = 1;
	}
	if(refilled)
		kick(c, Rxq);
	qunlock(&c->lk);
}

static void
rxproc(void *arg)
{
	Rendez r;

	USED(arg);
	memset(&r, 0, sizeof r);
	/* archetherprobe() calls netifinit() after reset returns; etheriq()
	 * needs that Netif, so do not poll until the name is set. */
	while(ctlr.edev != nil && ctlr.edev->Netif.name[0] == 0)
		tsleep(&r, return0, nil, 1);
	for(;;){
		rxpoll();
		tsleep(&r, return0, nil, 1);
	}
}

static void
transmit(Ether *edev)
{
	Ctlr *c = ctlr.edev == edev ? &ctlr : nil;
	Block *bp;

	if(c == nil || !c->started)
		return;
	qlock(&c->lk);
	while((bp = qget(edev->oq)) != nil){
		int slot = c->txnext % Qsize;
		u8 *d = c->tx.descva + (usize)slot * 16;
		u16 av;
		u32 len = (u32)BLEN(bp);

		if(len + Vhdr > Framesize){
			freeb(bp);
			edev->Netif.oerrs++;
			continue;
		}
		memset(c->txbuf[slot], 0, Vhdr);
		memmove(c->txbuf[slot] + Vhdr, bp->rp, len);
		*(u64 *)d = PADDR(c->txbuf[slot]);
		*(u32 *)(d + 8) = len + Vhdr;
		*(u16 *)(d + 12) = 0;
		*(u16 *)(d + 14) = 0;

		av = c->tx.availidx % c->tx.size;
		*(u16 *)(c->tx.availva + 4 + (usize)av * 2) = (u16)slot;
		coherence();
		*(u16 *)(c->tx.availva + 2) = c->tx.availidx + 1;
		c->tx.availidx++;
		kick(c, Txq);

		c->txnext = (c->txnext + 1) % Qsize;
		edev->Netif.outpackets++;
		freeb(bp);
	}
	qunlock(&c->lk);
}

static i32
vnetifstat(Ether *edev, void *a, i32 n, u32 offset)
{
	char *p;
	int len;

	USED(offset);
	p = malloc(READSTR);
	if(p == nil)
		return 0;
	len = snprint(p, READSTR,
	              "inpackets: %lud\noutpackets: %lud\n"
	              "oerrs: %lud\nmisses: %lud\noverflows: %lud\n",
	              edev->Netif.inpackets, edev->Netif.outpackets,
	              edev->Netif.oerrs, edev->Netif.misses,
	              edev->Netif.overflows);
	len = readstr(offset, a, n, p);
	free(p);
	return len;
}

static int
vnetreset(Ether *edev)
{
	Ctlr *c = &ctlr;
	Fdtvirtio ap[Maxfdtvirtio];
	int n, i;
	u32 feat;

	n = arm64virtioapatures(ap, Maxfdtvirtio);
	for(i = lastscan + 1; i < n; i++){
		u8 *m;
		u64 base = ap[i].base;

		if(base == 0 || ap[i].size < 0x200)
			continue;
		m = (u8 *)(usize)base;
		if(mmread(m, Vmagic) != VnetMagic)
			continue;
		if((mmread(m, Vversion) & 0xff) != 2)
			continue;
		if((mmread(m, Vdevice) & 0xffff) != TypNet)
			continue;
		lastscan = i;
		memset(c, 0, sizeof *c);
		c->mmio = m;
		c->edev = edev;

		mmwrite(m, Vstatus, 0);
		mmwrite(m, Vstatus, Acknowledge | Driver);
		/* VERSION_1 lives in feature word 1 */
		mmwrite(m, Vdevselect, 1);
		feat = mmread(m, Vdevfeatures);
		if((feat & 1) == 0)
			return -1;
		mmwrite(m, Vdrvselect, 1);
		mmwrite(m, Vdrvfeatures, 1);
		mmwrite(m, Vdevselect, 0);
		feat = mmread(m, Vdevfeatures);
		mmwrite(m, Vdrvselect, 0);
		mmwrite(m, Vdrvfeatures, feat & Fmac);
		mmwrite(m, Vstatus, Acknowledge | Driver | FeaturesOk);

		for(int b = 0; b < 6; b++)
			c->mac[b] = m[Vconfig + b];
		memmove(edev->ea, c->mac, 6);

		if(vqinit(c, &c->rx, Rxq, Qsize) < 0)
			return -1;
		if(vqinit(c, &c->tx, Txq, Qsize) < 0)
			return -1;
		for(int s = 0; s < Qsize; s++){
			c->rxbuf[s] = mallocz(Framesize, 1);
			c->txbuf[s] = mallocz(Framesize, 1);
			if(c->rxbuf[s] == nil || c->txbuf[s] == nil)
				return -1;
			postrx(c, s);
		}
		kick(c, Rxq);
		coherence();
		mmwrite(m, Vstatus, Acknowledge | Driver | FeaturesOk | DriverOk);

		edev->Netif.mbps = 1000;
		edev->Netif.link = 1;
		/* minmtu/maxmtu/mtu come from archetherprobe(); lowering
		 * minmtu here made ARP frames fail with Etoosmall. */
		edev->transmit = transmit;
		edev->interrupt = nil;
		edev->ifstat = vnetifstat;
		edev->ctlr = c;
		c->started = 1;
		print("#l%d: virtio-mmio net %2.2ux%2.2ux%2.2ux%2.2ux%2.2ux%2.2ux\n",
		      edev->ctlrno, c->mac[0], c->mac[1], c->mac[2],
		      c->mac[3], c->mac[4], c->mac[5]);
		kproc("virtionet", rxproc, nil);
		return 0;
	}
	return -1;
}

void
ethervmmioinit(void)
{
	addethercard("virtio", vnetreset);
}
