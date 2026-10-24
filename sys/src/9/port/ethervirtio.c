/*
 * ethervirtio.c - virtio 1.0 modern ethernet driver (#l)
 * Clean, minimal implementation for Harvey microkernel under KVM.
 */

#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "../port/netif.h"
#include "virtio10.h"
#include "../amd64/etherif.h"

enum {
	TypNet      = 1,        /* VirtIO device type for network */

	/* Status bits */
	Acknowledge = 1,
	Driver      = 2,
	DriverOk    = 4,
	FeaturesOk  = 8,
	Failed      = 0x80,

	/* Descriptor flags */
	Next        = 1,
	Write       = 2,

	VringSize   = 4,
	VhdrSize    = 12,       /* VirtIO 1.0 net header size */

	Fmac        = 1 << 5,   /* Device has given MAC */
	Fstatus     = 1 << 16,  /* Device status available */

	Qsize       = 16,       /* Virtqueue ring size */
};

typedef struct Vconfig Vconfig;
struct Vconfig {
	u32 devfeatsel;
	u32 devfeat;
	u32 drvfeatsel;
	u32 drvfeat;

	u16 msixcfg;
	u16 nqueues;

	u8  status;
	u8  cfggen;
	u16 queuesel;

	u16 queuesize;
	u16 queuemsixvect;

	u16 queueenable;
	u16 queuenotifyoff;

	u64 queuedesc;
	u64 queueavail;
	u64 queueused;
};

typedef struct Vnetcfg Vnetcfg;
struct Vnetcfg {
	u8  mac[6];
	u16 status;
	u16 max_vq_pairs;
	u16 mtu;
};

typedef struct Vring Vring;
struct Vring {
	u16 flags;
	u16 idx;
};

typedef struct Vdesc Vdesc;
struct Vdesc {
	u64 addr;
	u32 len;
	u16 flags;
	u16 next;
};

typedef struct Vused Vused;
struct Vused {
	u32 id;
	u32 len;
};

typedef struct Vqueue Vqueue;
struct Vqueue {
	Lock   l;
	int    idx;
	int    size;

	struct Ctlr *c;

	Vdesc  *desc;
	Vring  *avail;
	u16    *availent;
	u16    *availevent;

	Vring  *used;
	Vused  *usedent;
	u16    *usedevent;
	u16    lastused;
};

typedef struct Ctlr Ctlr;
struct Ctlr {
	Pcidev  *pci;
	Vio     cfg;
	Vio     isr;
	Vio     notify;
	u32     notifyoffmult;
	u32     feat[2];

	Vqueue  *rxq;
	Vqueue  *txq;

	u8      rx_hdr[Qsize / 2][VhdrSize];
	Block   *rx_blocks[Qsize / 2];

	u8      tx_hdr[Qsize / 2][VhdrSize];
	Block   *tx_blocks[Qsize / 2];

	Ether   *edev;
	int     attached;
};

static Pcicap*
findvirtiocap(Pcidev *p, int typ)
{
	for(Pcicap *c = p->caplist; c != nil; c = c->link){
		if(c->vndr == PciCapVND && c->type == typ)
			return c;
	}
	return nil;
}


static Vqueue*
mkvqueue(int size)
{
	Vqueue *q;
	u8 *p;

	q = malloc(sizeof(*q));
	if(q == nil)
		return nil;

	p = mallocalign(
		PGROUND(sizeof(Vdesc) * size +
			VringSize +
			sizeof(u16) * size +
			sizeof(u16)) +
		PGROUND(VringSize +
			sizeof(Vused) * size +
			sizeof(u16)),
		4096, 0, 0);
	if(p == nil){
		free(q);
		return nil;
	}
	memset(p, 0,
		PGROUND(sizeof(Vdesc) * size +
			VringSize +
			sizeof(u16) * size +
			sizeof(u16)) +
		PGROUND(VringSize +
			sizeof(Vused) * size +
			sizeof(u16)));

	q->desc = (void*)p;
	p += sizeof(Vdesc) * size;
	q->avail = (void*)p;
	p += VringSize;
	q->availent = (void*)p;
	p += sizeof(u16) * size;
	q->availevent = (void*)p;
	p += sizeof(u16);
	p = (u8*)PGROUND((uintptr)p);
	q->used = (void*)p;
	p += VringSize;
	q->usedent = (void*)p;
	p += sizeof(Vused) * size;
	q->usedevent = (void*)p;

	q->size = size;
	q->lastused = 0;
	return q;
}

static void
ethervirtionotify(Vqueue *q)
{
	Ctlr *c = q->c;
	int off;

	if(c == nil || (q->used->flags & 1) != 0)
		return;
	off = c->notifyoffmult * vin16(&c->cfg, Vconf_queuenotifyoff);
	if(c->notify.type == Vio_port)
		outs(c->notify.port + off, q->idx);
	else
		*(u16*)(c->notify.mem + off) = q->idx;
}

static void
ethervirtioattach(Ether *edev)
{
	Ctlr *c = edev->ctlr;
	Vqueue *rxq = c->rxq;
	int n = rxq->size / 2;

	if(c->attached)
		return;
	c->attached = 1;
	ilock(&rxq->l);
	for(int i = 0; i < n; i++){
		Block *b = allocb(ETHERMAXTU + 64);
		if(b == nil)
			break;
		c->rx_blocks[i] = b;

		int d0 = i * 2;
		int d1 = d0 + 1;

		rxq->desc[d0].addr = PADDR(c->rx_hdr[i]);
		rxq->desc[d0].len = VhdrSize;
		rxq->desc[d0].flags = Next | Write;
		rxq->desc[d0].next = d1;

		rxq->desc[d1].addr = PADDR(b->wp);
		rxq->desc[d1].len = ETHERMAXTU + 64;
		rxq->desc[d1].flags = Write;
		rxq->desc[d1].next = 0;

		rxq->availent[rxq->avail->idx & (rxq->size - 1)] = d0;
		rxq->avail->idx++;
	}
	coherence();
	iunlock(&rxq->l);
	ethervirtionotify(rxq);
}

static void
ethervirtiotransmit(Ether *edev)
{
	Ctlr *c = edev->ctlr;
	Vqueue *txq = c->txq;
	Block *b;

	ilock(&txq->l);
	while((b = qget(edev->oq)) != nil){
		int slot = -1;
		int n = txq->size / 2;
		for(int i = 0; i < n; i++){
			if(c->tx_blocks[i] == nil){
				slot = i;
				break;
			}
		}
		if(slot < 0){
			/* Queue full, put back */
			qputback(edev->oq, b);
			break;
		}

		c->tx_blocks[slot] = b;
		memset(c->tx_hdr[slot], 0, VhdrSize);

		int d0 = slot * 2;
		int d1 = d0 + 1;

		txq->desc[d0].addr = PADDR(c->tx_hdr[slot]);
		txq->desc[d0].len = VhdrSize;
		txq->desc[d0].flags = Next;
		txq->desc[d0].next = d1;

		txq->desc[d1].addr = PADDR(b->rp);
		txq->desc[d1].len = BLEN(b);
		txq->desc[d1].flags = 0;
		txq->desc[d1].next = 0;

		txq->availent[txq->avail->idx & (txq->size - 1)] = d0;
		coherence();
		txq->avail->idx++;
	}
	iunlock(&txq->l);
	ethervirtionotify(txq);
}

static void
ethervirtiointr(Ureg *ur, void *arg)
{
	Ether *edev = arg;
	Ctlr *c = edev->ctlr;
	USED(ur);

	u8 isr = vin8(&c->isr, 0);
	if((isr & 1) == 0)
		return;

	/* 1. Harvest TX completions */
	Vqueue *txq = c->txq;
	ilock(&txq->l);
	while((txq->lastused ^ txq->used->idx) & 0xFFFF){
		int m = txq->size - 1;
		int ix = txq->lastused & m;
		int id = txq->usedent[ix].id / 2;
		if(id >= 0 && id < txq->size / 2 && c->tx_blocks[id] != nil){
			freeb(c->tx_blocks[id]);
			c->tx_blocks[id] = nil;
		}
		txq->lastused++;
	}
	iunlock(&txq->l);

	/* 2. Process RX completions */
	Vqueue *rxq = c->rxq;
	ilock(&rxq->l);
	int replenished = 0;
	while((rxq->lastused ^ rxq->used->idx) & 0xFFFF){
		int m = rxq->size - 1;
		int ix = rxq->lastused & m;
		int id = rxq->usedent[ix].id / 2;
		u32 total_len = rxq->usedent[ix].len;
		rxq->lastused++;

		if(id >= 0 && id < rxq->size / 2){
			Block *b = c->rx_blocks[id];
			c->rx_blocks[id] = nil;

			if(b != nil && total_len > VhdrSize){
				b->wp = b->rp + (total_len - VhdrSize);
				etheriq(edev, b, 1);
			} else if(b != nil){
				freeb(b);
			}

			/* Replenish slot */
			Block *nb = allocb(ETHERMAXTU + 64);
			if(nb != nil){
				c->rx_blocks[id] = nb;
				int d0 = id * 2;
				int d1 = d0 + 1;
				rxq->desc[d1].addr = PADDR(nb->wp);
				rxq->desc[d1].len = ETHERMAXTU + 64;
				rxq->availent[rxq->avail->idx & m] = d0;
				rxq->avail->idx++;
				replenished = 1;
			}
		}
	}
	coherence();
	iunlock(&rxq->l);

	if(replenished)
		ethervirtionotify(rxq);
}

int
ethervirtioreset(Ether *edev)
{
	static Pcidev *lastp = nil;
	Pcidev *p = lastp;
	while((p = pcimatch(p, 0x1AF4, 0x1040 + TypNet)) != nil){
		if(p->rid != 0)
			break;
	}
	if(p == nil)
		return -1;
	lastp = p;

	/* Setup BAR0 */
	int rno = PciBAR0 - 4;
	for(int i = 0; i < nelem(p->mem); i++){
		rno += 4;
		p->mem[i].bar = pcicfgr32(p, rno);
		p->mem[i].size = pcibarsize(p, rno);
	}

	Pcicap *cap1 = findvirtiocap(p, 1);
	Pcicap *cap2 = findvirtiocap(p, 2);
	Pcicap *cap3 = findvirtiocap(p, 3);
	Pcicap *cap4 = findvirtiocap(p, 4);

	if(cap1 == nil || cap2 == nil || cap3 == nil || cap4 == nil)
		return -1;

	Vio cfg, notify, isr, netcfg;
	if(virtiomapregs(p, cap1, Vconf_sz, &cfg) == nil ||
	   virtiomapregs(p, cap2, 2, &notify) == nil ||
	   virtiomapregs(p, cap3, 1, &isr) == nil ||
	   virtiomapregs(p, cap4, 12, &netcfg) == nil)
		return -1;

	Ctlr *c = mallocz(sizeof(*c), 1);
	if(c == nil)
		return -1;

	c->pci = p;
	c->cfg = cfg;
	c->notify = notify;
	c->isr = isr;
	c->notifyoffmult = pcicfgr32(p, cap2->offset + 16);
	c->edev = edev;

	/* Reset then acknowledge */
	vout8(&c->cfg, Vconf_status, 0);
	while(vin8(&c->cfg, Vconf_status) != 0)
		delay(1);
	vout8(&c->cfg, Vconf_status, Acknowledge | Driver);

	/* Negotiate features: VERSION_1, MAC, STATUS */
	vout32(&c->cfg, Vconf_devfeatsel, 1);
	c->feat[1] = vin32(&c->cfg, Vconf_devfeat);
	vout32(&c->cfg, Vconf_devfeatsel, 0);
	c->feat[0] = vin32(&c->cfg, Vconf_devfeat);

	vout32(&c->cfg, Vconf_drvfeatsel, 1);
	vout32(&c->cfg, Vconf_drvfeat, c->feat[1] & 1);
	vout32(&c->cfg, Vconf_drvfeatsel, 0);
	vout32(&c->cfg, Vconf_drvfeat, c->feat[0] & (Fmac | Fstatus));
	vout8(&c->cfg, Vconf_status, vin8(&c->cfg, Vconf_status) | FeaturesOk);

	/* Read MAC address */
	for(int i = 0; i < 6; i++)
		edev->ea[i] = vin8(&netcfg, i);

	/* Initialize Queue 0 (RX) */
	vout16(&c->cfg, Vconf_queuesel, 0);
	int rx_size = vin16(&c->cfg, Vconf_queuesize);
	if(rx_size == 0 || rx_size > Qsize)
		rx_size = Qsize;
	c->rxq = mkvqueue(rx_size);
	if(c->rxq == nil){
		free(c);
		return -1;
	}
	c->rxq->idx = 0;
	c->rxq->c = c;
	vout64(&c->cfg, Vconf_queuedesc, PADDR(c->rxq->desc));
	vout64(&c->cfg, Vconf_queueavail, PADDR(c->rxq->avail));
	vout64(&c->cfg, Vconf_queueused, PADDR(c->rxq->used));
	vout16(&c->cfg, Vconf_queueenable, 1);

	/* Initialize Queue 1 (TX) */
	vout16(&c->cfg, Vconf_queuesel, 1);
	int tx_size = vin16(&c->cfg, Vconf_queuesize);
	if(tx_size == 0 || tx_size > Qsize)
		tx_size = Qsize;
	c->txq = mkvqueue(tx_size);
	if(c->txq == nil){
		free(c->rxq);
		free(c);
		return -1;
	}
	c->txq->idx = 1;
	c->txq->c = c;
	vout64(&c->cfg, Vconf_queuedesc, PADDR(c->txq->desc));
	vout64(&c->cfg, Vconf_queueavail, PADDR(c->txq->avail));
	vout64(&c->cfg, Vconf_queueused, PADDR(c->txq->used));
	vout16(&c->cfg, Vconf_queueenable, 1);

	vout8(&c->cfg, Vconf_status, vin8(&c->cfg, Vconf_status) | DriverOk);

	pcisetbme(p);

	edev->ctlr = c;
	edev->tbdf = p->tbdf;
	edev->attach = ethervirtioattach;
	edev->transmit = ethervirtiotransmit;
	edev->interrupt = ethervirtiointr;
	edev->ISAConf.irq = p->intl;
	edev->ISAConf.port = p->mem[0].bar & ~0xFULL;
	edev->Netif.mbps = 1000;
	edev->Netif.mtu = 1500;

	return 0;
}

void
ethervirtiolink(void)
{
	addethercard("virtio", ethervirtioreset);
}
