/*
 * devvirtio9p.c - virtio 1.0 9P transport (#9)
 * Ported from Halatha (extras/9legacy/kernel/devvirtio9p.c).
 * Pure 9P2000 end to end over virtqueue descriptor chains.
 *
 * Mount contract:
 *     bind -a '#9' /dev
 *     mount -c '#9/0' /n/host
 */

#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#ifdef __aarch64__
#include "fdt.h"
#else
#include "io.h"
#endif
#include "../port/error.h"
#include "virtio9p_core.h"
#include "virtio10.h"

typedef struct Vconfig Vconfig;
typedef struct Vring Vring;
typedef struct Vdesc Vdesc;
typedef struct Vused Vused;
typedef struct Vqueue Vqueue;
typedef struct V9cfg V9cfg;
typedef struct Slot Slot;
typedef struct Ctlr Ctlr;

enum {
	Typ9p       = 9,        /* VirtIO device type for 9P */

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

	Fmounttag   = 1,        /* 9P device feature bit 0: config carries mount tag */

	Msize       = IOHDRSZ + 64*1024,
	Maxslot     = 64,       /* cap on outstanding RPCs per device */
};

#ifndef __aarch64__
struct Vconfig {                /* common configuration (cap type 1) */
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
#endif

struct V9cfg {                  /* device configuration (cap type 4) */
	u16 taglen;
	/* u8 tag[taglen] follows */
};

struct Vring {
	u16 flags;
	u16 idx;
};

struct Vdesc {
	u64 addr;
	u32 len;
	u16 flags;
	u16 next;
};

struct Vused {
	u32 id;
	u32 len;
};

struct Vqueue {
	Lock   l;

	int    idx;
	int    size;

	int    free;            /* descriptor free list head */
	int    nfree;

	void   *notify;

	Vdesc  *desc;

	Vring  *avail;
	u16    *availent;
	u16    *availevent;

	Vring  *used;
	Vused  *usedent;
	u16    *usedevent;
	u16    lastused;

	void   *rock[];         /* head index -> Slot* in flight */
};

struct Slot {
	u8     *out;            /* device-readable: T-message */
	u8     *in;             /* device-writable: R-message */
	u32    inlen;           /* bytes written by device */
	u32    rp;              /* read-serve offset within in */
	int    op;
	u32    fid;
	Slot   *next;           /* free list / done FIFO link */
};

struct Ctlr {
#ifdef __aarch64__
	volatile u8 *mmio;
	int version;
#else
	Pcidev  *pci;
	Vio     cfg;
	Vio     isr;
	Vio     notify;
	u32     notifyoffmult;
#endif
	u32     feat[2];

	Vqueue  *vq;            /* single request queue */
	char    tag[64];        /* mount tag */

	Slot    *slot;
	int     nslot;
	Slot    *sfree;
	Slot    *donehd;
	Slot    *donetl;
	Slot    *cur;

	Rendez  rwait;          /* read waits for completed reply */
	Rendez  wwait;          /* write waits for free slot */

	int     started;
	int     failed;
	Ref     inuse;

	Ctlr    *next;
};

static Ctlr *ctlrs;
static int  probed;

#ifndef __aarch64__
static void v9interrupt(Ureg*, void*);
#endif

static Vqueue*
mkvqueue(int size)
{
	Vqueue *q;
	u8 *p;
	usize bytes;
	int i;

	bytes = PGROUND(sizeof(Vdesc) * size +
			VringSize +
			sizeof(u16) * size +
			sizeof(u16)) +
		PGROUND(VringSize +
			sizeof(Vused) * size +
			sizeof(u16));
	q = malloc(sizeof(*q) + sizeof(void*) * size);
	p = mallocalign(bytes, PGSZ, 0, 0);
	if(p == nil || q == nil){
		print("virtio9p: no memory for Vqueue\n");
		free(p);
		free(q);
		return nil;
	}
	memset(q, 0, sizeof(*q) + sizeof(void*) * size);
	memset(p, 0, bytes);

	q->desc = (void*)p;
	p += sizeof(Vdesc) * size;
	q->avail = (void*)p;
	p += VringSize;
	q->availent = (void*)p;
	p += sizeof(u16) * size;
	q->availevent = (void*)p;
	p += sizeof(u16);

	p = (u8*)PGROUND((usize)p);
	q->used = (void*)p;
	p += VringSize;
	q->usedent = (void*)p;
	p += sizeof(Vused) * size;
	q->usedevent = (void*)p;

	q->free = -1;
	q->nfree = q->size = size;
	for(i = 0; i < size; i++){
		q->desc[i].next = q->free;
		q->free = i;
	}

	return q;
}

#ifdef __aarch64__
/* VirtIO 1.x MMIO registers on QEMU virt. The FDT supplies every aperture. */
enum {
	Vmagic = 0x000,
	Vversion = 0x004,
	Vdevice = 0x008,
	Vdevfeatures = 0x010,
	Vdevselect = 0x014,
	Vdrvfeatures = 0x020,
	Vdrvselect = 0x024,
	Vguestpagesize = 0x028,
	Vqueuesel = 0x030,
	Vqueuemax = 0x034,
	Vqueuenum = 0x038,
	Vqueuealign = 0x03c,
	Vqueuepfn = 0x040,
	Vqueueready = 0x044,
	Vnotify = 0x050,
	Vinterrupt = 0x060,
	Vinterruptack = 0x064,
	Vstatus = 0x070,
	Vdescaddr = 0x080,
	Vavailaddr = 0x090,
	Vusedaddr = 0x0a0,
	Vconfigbase = 0x100,
};

static Fdtvirtio virtioaperture[Maxfdtvirtio];
static int nvirtioaperture;

void
arm64virtio9pconfigure(Fdtinfo *info)
{
	int i;

	nvirtioaperture = info->nvirtio;
	for(i = 0; i < nvirtioaperture; i++)
		virtioaperture[i] = info->virtio[i];
}

static u32
mmread(Ctlr *c, int off)
{
	return *(volatile u32 *)(c->mmio + off);
}

static void
mmwrite(Ctlr *c, int off, u32 value)
{
	*(volatile u32 *)(c->mmio + off) = value;
}

static void
mmaddr(Ctlr *c, int off, u64 addr)
{
	mmwrite(c, off, (u32)addr);
	mmwrite(c, off+4, (u32)(addr>>32));
}

static void
readtag(Ctlr *c)
{
	u16 n;
	int i;

	c->tag[0] = 0;
	if((c->feat[0] & Fmounttag) == 0)
		return;
	n = c->mmio[Vconfigbase] | c->mmio[Vconfigbase+1]<<8;
	if(n >= sizeof(c->tag))
		n = sizeof(c->tag)-1;
	for(i = 0; i < n; i++)
		c->tag[i] = c->mmio[Vconfigbase+2+i];
	c->tag[n] = 0;
}

static void
v9probe(void)
{
	Ctlr *c, **tail;
	Vqueue *q;
	Slot *s;
	int i, j, n, ndev;
	u64 base;

	if(probed)
		return;
	probed = 1;
	ndev = 0;
	tail = &ctlrs;
	for(i = 0; i < nvirtioaperture; i++){
		base = virtioaperture[i].base;
		if(base == 0 || virtioaperture[i].size < 0x200)
			continue;
		c = malloc(sizeof(*c));
		if(c == nil)
			break;
		memset(c, 0, sizeof(*c));
		c->mmio = (volatile u8 *)(usize)base;
		c->version = mmread(c, Vversion);
		if(mmread(c, Vmagic) != 0x74726976 ||
		   (c->version != 1 && c->version != 2) ||
		   mmread(c, Vdevice) != Typ9p){
			free(c);
			continue;
		}
		mmwrite(c, Vstatus, 0);
		mmwrite(c, Vstatus, Acknowledge | Driver);
		if(c->version == 2){
			mmwrite(c, Vdevselect, 1);
			c->feat[1] = mmread(c, Vdevfeatures);
		}
		mmwrite(c, Vdevselect, 0);
		c->feat[0] = mmread(c, Vdevfeatures);
		if(c->version == 2 && (c->feat[1] & 1) == 0){
			mmwrite(c, Vstatus, Failed);
			free(c);
			continue;
		}
		if(c->version == 2){
			mmwrite(c, Vdrvselect, 1);
			mmwrite(c, Vdrvfeatures, 1);
			mmwrite(c, Vdrvselect, 0);
		}
		mmwrite(c, Vdrvfeatures, c->feat[0] & Fmounttag);
		if(c->version == 2)
			mmwrite(c, Vstatus, Acknowledge | Driver | FeaturesOk);
		if(c->version == 2 && (mmread(c, Vstatus) & FeaturesOk) == 0){
			mmwrite(c, Vstatus, Failed);
			free(c);
			continue;
		}
		mmwrite(c, Vqueuesel, 0);
		if((c->version == 2 && mmread(c, Vqueueready) != 0) ||
		   (c->version == 1 && mmread(c, Vqueuepfn) != 0)){
			mmwrite(c, Vstatus, Failed);
			free(c);
			continue;
		}
		n = v9pqueuesize(mmread(c, Vqueuemax), 64);
		if(n < 2){
			mmwrite(c, Vstatus, Failed);
			free(c);
			continue;
		}
		q = mkvqueue(n);
		if(q == nil){
			mmwrite(c, Vstatus, Failed);
			free(c);
			continue;
		}
		q->idx = 0;
		q->notify = (void *)(c->mmio + Vnotify);
		c->vq = q;
		mmwrite(c, Vqueuenum, n);
		coherence();
		if(c->version == 2){
			mmaddr(c, Vdescaddr, PADDR(q->desc));
			mmaddr(c, Vavailaddr, PADDR(q->avail));
			mmaddr(c, Vusedaddr, PADDR(q->used));
			mmwrite(c, Vqueueready, 1);
		}else{
			mmwrite(c, Vguestpagesize, PGSZ);
			mmwrite(c, Vqueuealign, PGSZ);
			mmwrite(c, Vqueuepfn, PADDR(q->desc)/PGSZ);
		}
		readtag(c);
		c->nslot = n/2;
		if(c->nslot > Maxslot)
			c->nslot = Maxslot;
		c->slot = malloc(sizeof(Slot) * c->nslot);
		if(c->slot == nil)
			panic("virtio9p: no memory for slots");
		memset(c->slot, 0, sizeof(Slot) * c->nslot);
		for(j = 0; j < c->nslot; j++){
			s = &c->slot[j];
			s->out = malloc(Msize+64);
			s->in = malloc(Msize);
			if(s->out == nil || s->in == nil)
				panic("virtio9p: no memory for slot buffers");
			s->next = c->sfree;
			c->sfree = s;
		}
		*tail = c;
		tail = &c->next;
		print("#9/%d: virtio-mmio v%d 9p tag \"%s\" nslot %d msize %d\n",
			ndev++, c->version, c->tag, c->nslot, Msize);
	}
}

static void
v9start(Ctlr *c)
{
	if(c->started)
		return;
	coherence();
	mmwrite(c, Vstatus, Acknowledge | Driver |
		(c->version == 2 ? FeaturesOk : 0) | DriverOk);
	c->started = 1;
}

#else
static Pcicap*
findvirtiocap(Pcidev *p, int typ)
{
	for(Pcicap *c = p->caplist; c != nil; c = c->link){
		if(c->vndr == PciCapVND && c->type == typ)
			return c;
	}
	return nil;
}


static void
readtag(Ctlr *c)
{
	Pcicap *cap;
	Vio dc;
	int n, i;

	c->tag[0] = 0;
	if((c->feat[0] & Fmounttag) == 0)
		return;
	cap = findvirtiocap(c->pci, 4);
	if(cap == nil)
		return;
	if(virtiomapregs(c->pci, cap, 2, &dc) == nil)
		return;
	n = vin16(&dc, 0);
	if(n >= sizeof(c->tag))
		n = sizeof(c->tag) - 1;
	for(i = 0; i < n; i++)
		c->tag[i] = vin8(&dc, 2+i);
	c->tag[n] = 0;
	virtiounmap(&dc, 2 + n);
}

static void
v9probe(void)
{
	Ctlr *c, **tail;
	Pcidev *p;
	Vqueue *q;
	Slot *s;
	Pcicap *cap1, *cap2, *cap3;
	int n, i, ndev, rno;

	if(probed)
		return;
	probed = 1;

	ndev = 0;
	tail = &ctlrs;
	for(p = nil; (p = pcimatch(p, 0x1AF4, 0x1040 + Typ9p)) != nil;){
		if(p->rid == 0)
			continue;

		rno = PciBAR0 - 4;
		for(i = 0; i < nelem(p->mem); i++){
			rno += 4;
			p->mem[i].bar = pcicfgr32(p, rno);
			p->mem[i].size = pcibarsize(p, rno);
		}

		cap1 = findvirtiocap(p, 1);
		if(cap1 == nil){
			print("v9probe: no cap1\n");
			continue;
		}
		Vio cfg;
		if(virtiomapregs(p, cap1, Vconf_sz, &cfg) == nil){
			print("v9probe: no cfg map bar %d len %d bar0 %ux\n", cap1->bar, cap1->length, p->mem[cap1->bar].bar);
			continue;
		}

		cap3 = findvirtiocap(p, 3);
		if(cap3 == nil)
			continue;

		cap2 = findvirtiocap(p, 2);
		if(cap2 == nil)
			continue;

		c = malloc(sizeof(*c));
		if(c == nil){
			print("virtio9p: no memory for Ctlr\n");
			break;
		}
		c->pci = p;
		c->cfg = cfg;
		if(virtiomapregs(p, cap3, 1, &c->isr) == nil){
baddev:
			free(c);
			continue;
		}

		if(virtiomapregs(p, cap2, 2, &c->notify) == nil)
			goto baddev;
		c->notifyoffmult = pcicfgr32(p, cap2->offset + 16);

		/* Reset then acknowledge */
		vout8(&c->cfg, Vconf_status, 0);
		while(vin8(&c->cfg, Vconf_status) != 0)
			delay(1);
		vout8(&c->cfg, Vconf_status, Acknowledge | Driver);

		/* Negotiate features: VERSION_1 (bit 32) and mount tag */
		vout32(&c->cfg, Vconf_devfeatsel, 1);
		c->feat[1] = vin32(&c->cfg, Vconf_devfeat);
		vout32(&c->cfg, Vconf_devfeatsel, 0);
		c->feat[0] = vin32(&c->cfg, Vconf_devfeat);
		vout32(&c->cfg, Vconf_drvfeatsel, 1);
		vout32(&c->cfg, Vconf_drvfeat, c->feat[1] & 1);
		vout32(&c->cfg, Vconf_drvfeatsel, 0);
		vout32(&c->cfg, Vconf_drvfeat, c->feat[0] & Fmounttag);
		vout8(&c->cfg, Vconf_status, vin8(&c->cfg, Vconf_status) | FeaturesOk);
		if((vin8(&c->cfg, Vconf_status) & FeaturesOk) == 0){
			print("virtio9p: device rejected features\n");
			goto baddev;
		}

		/* Request queue 0 */
		vout16(&c->cfg, Vconf_queuesel, 0);
		n = v9pqueuesize(vin16(&c->cfg, Vconf_queuesize), Maxslot * 2);
		if(n == 0){
			print("virtio9p: bad queue size %d\n", n);
			goto baddev;
		}
		q = mkvqueue(n);
		if(q == nil)
			goto baddev;
		q->idx = 0;
		c->vq = q;
		coherence();
		vout64(&c->cfg, Vconf_queuedesc, PADDR(q->desc));
		vout64(&c->cfg, Vconf_queueavail, PADDR(q->avail));
		vout64(&c->cfg, Vconf_queueused, PADDR(q->used));

		readtag(c);

		c->nslot = n / 2;
		if(c->nslot > Maxslot)
			c->nslot = Maxslot;
		c->slot = malloc(sizeof(Slot) * c->nslot);
		if(c->slot == nil)
			goto baddev;
		c->sfree = nil;
		for(i = 0; i < c->nslot; i++){
			s = &c->slot[i];
			s->out = malloc(Msize + 64);
			s->in = malloc(Msize);
			if(s->out == nil || s->in == nil){
				print("virtio9p: no memory for slots\n");
				goto baddev;
			}
			s->next = c->sfree;
			c->sfree = s;
		}

		*tail = c;
		tail = &c->next;
		print("#9/%d: virtio9p tag \"%s\" nslot %d msize %d\n",
			ndev++, c->tag, c->nslot, Msize);
	}
	if(ctlrs == nil)
		probed = 0;
}

static void
v9start(Ctlr *c)
{
	if(c->started)
		return;
	pcisetbme(c->pci);
	intrenable(c->pci->intl, v9interrupt, c, c->pci->tbdf, "virtio9p");
	coherence();
	vout16(&c->cfg, Vconf_queuesel, 0);
	vout16(&c->cfg, Vconf_queueenable, 1);
	vout8(&c->cfg, Vconf_status, vin8(&c->cfg, Vconf_status) | DriverOk);
	c->started = 1;
}
#endif

static void
vqharvest(Ctlr *c)
{
	Vqueue *q;
	Slot *s;
	u32 id;
	int free, m, ix, next;

	q = c->vq;
	m = q->size - 1;
#ifdef __aarch64__
	if(mmread(c, Vinterrupt) != 0)
		mmwrite(c, Vinterruptack, mmread(c, Vinterrupt));
#endif
	ilock(&q->l);
	while((q->lastused ^ q->used->idx) & 0xFFFF){
		ix = q->lastused & m;
		id = q->usedent[ix].id;
		if(!v9pindexvalid(id, q->size) || q->rock[id] == nil){
			print("virtio9p: invalid used descriptor %ud\n", id);
			c->failed = 1;
			q->lastused++;
			break;
		}
		next = q->desc[id].next;
		if(!v9pindexvalid(next, q->size) ||
		   !v9pchain2valid(id, q->desc[id].flags, next,
			q->desc[next].flags, q->size)){
			print("virtio9p: invalid descriptor chain %ud\n", id);
			c->failed = 1;
			q->lastused++;
			break;
		}
		s = q->rock[id];
		q->rock[id] = nil;
		if(s != nil)
			s->inlen = q->usedent[ix].len;
		q->lastused++;
		do {
			free = id;
			id = q->desc[free].next;
			q->desc[free].next = q->free;
			q->free = free;
			q->nfree++;
		} while(q->desc[free].flags & Next);
		if(s != nil){
			s->next = nil;
			if(c->donetl != nil)
				c->donetl->next = s;
			else
				c->donehd = s;
			c->donetl = s;
		}
	}
	iunlock(&q->l);
	wakeup(&c->rwait);
	wakeup(&c->wwait);
}

#ifndef __aarch64__
static void
v9interrupt(Ureg *ur, void *arg)
{
	Ctlr *c = arg;
	USED(ur);
	if(vin8(&c->isr, 0) & 1)
		vqharvest(c);
}
#endif

static int
havereply(void *arg)
{
	Ctlr *c;

	c = arg;
	return c->donehd != nil || c->failed;
}

static int
haveslot(void *arg)
{
	Ctlr *c;

	c = arg;
	return c->sfree != nil || c->failed;
}

static Slot*
getslot(Ctlr *c)
{
	Proc *up = externup();
	Vqueue *q;
	Slot *s;

	q = c->vq;
	ilock(&q->l);
	while(c->sfree == nil && !c->failed){
		iunlock(&q->l);
		while(waserror())
			;
		tsleep(&c->wwait, haveslot, c, 1000);
		poperror();
		vqharvest(c);
		ilock(&q->l);
	}
	if(c->failed){
		iunlock(&q->l);
		error("virtio9p: device queue failed");
	}
	s = c->sfree;
	c->sfree = s->next;
	s->next = nil;
	iunlock(&q->l);
	return s;
}

static void
putslot(Ctlr *c, Slot *s)
{
	Vqueue *q;

	q = c->vq;
	ilock(&q->l);
	s->next = c->sfree;
	c->sfree = s;
	iunlock(&q->l);
	wakeup(&c->wwait);
}

static Slot*
getreply(Ctlr *c)
{
	Proc *up = externup();
	Vqueue *q;
	Slot *s;

	q = c->vq;
	ilock(&q->l);
	while(c->donehd == nil && !c->failed){
		iunlock(&q->l);
		while(waserror())
			;
		tsleep(&c->rwait, havereply, c,
#ifdef __aarch64__
		       1);
#else
		       1000);
#endif
		poperror();
		if(c->donehd == nil)
			vqharvest(c);
		ilock(&q->l);
	}
	if(c->failed){
		iunlock(&q->l);
		error("virtio9p: device queue failed");
	}
	s = c->donehd;
	c->donehd = s->next;
	if(c->donehd == nil)
		c->donetl = nil;
	iunlock(&q->l);
	s->next = nil;
	return s;
}

static void
submit(Ctlr *c, Slot *s, long outlen)
{
	Vqueue *q;
	Vdesc *d;
	int head, free;

	q = c->vq;
	ilock(&q->l);
	head = free = q->free;
	d = &q->desc[free]; free = d->next;
	d->addr = PADDR(s->out);
	d->len = outlen;
	d->flags = Next;
	d = &q->desc[free]; free = d->next;
	d->addr = PADDR(s->in);
	d->len = Msize;
	d->flags = Write;
	q->free = free;
	q->nfree -= 2;
	q->rock[head] = s;
	q->availent[q->avail->idx & (q->size - 1)] = head;
	coherence();
	q->avail->idx++;
	iunlock(&q->l);
	if((q->used->flags & 1) == 0){
#ifdef __aarch64__
		mmwrite(c, Vnotify, q->idx);
#else
		{
			int off = c->notifyoffmult * vin16(&c->cfg, Vconf_queuenotifyoff);
			if(c->notify.type == Vio_port)
				outs(c->notify.port + off, q->idx);
			else
				*(u16*)(c->notify.mem + off) = q->idx;
		}
#endif
	}
}

static void
v9flush(Ctlr *c)
{
	Vqueue *q;
	Slot *s, *nx;

	q = c->vq;
	ilock(&q->l);
	if(c->cur != nil){
		c->cur->next = c->sfree;
		c->sfree = c->cur;
		c->cur = nil;
	}
	for(s = c->donehd; s != nil; s = nx){
		nx = s->next;
		s->next = c->sfree;
		c->sfree = s;
	}
	c->donehd = c->donetl = nil;
	iunlock(&q->l);
}

static Ctlr*
ctlrindex(int i)
{
	Ctlr *c;
	for(c = ctlrs; c != nil && i > 0; c = c->next, i--)
		;
	return c;
}

static int
v9gen(Chan *c, char *unused, Dirtab *dt, int ndt, int s, Dir *dp)
{
	Qid q;
	char buf[16];
	USED(unused);
	USED(dt);
	USED(ndt);

	if(ctlrs == nil)
		v9probe();
	if(s == DEVDOTDOT){
		mkqid(&q, 0, 0, QTDIR);
		devdir(c, q, "#9", 0, eve, DMDIR | 0555, dp);
		return 1;
	}
	if(ctlrindex(s) == nil)
		return -1;
	snprint(buf, sizeof buf, "%d", s);
	mkqid(&q, s + 1, 0, QTFILE);
	devdir(c, q, buf, 0, eve, 0660, dp);
	return 1;
}

static void
v9reset(void)
{
	v9probe();
}

static Chan*
v9attach(char *spec)
{
	return devattach('9', spec);
}

static Walkqid*
v9walk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, nil, 0, v9gen);
}

static int
v9stat(Chan *c, u8 *dp, int n)
{
	return devstat(c, dp, n, nil, 0, v9gen);
}

static Chan*
v9open(Chan *c, int omode)
{
	Ctlr *cl;

	if(c->qid.type & QTDIR){
		if(ctlrs == nil)
			v9probe();
		return devopen(c, omode, nil, 0, v9gen);
	}
	cl = ctlrindex(c->qid.path - 1);
	if(cl == nil)
		error(Enonexist);
	if(incref(&cl->inuse) > 1){
		decref(&cl->inuse);
		error(Einuse);
	}
	c->aux = cl;
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	c->iounit = Msize - IOHDRSZ;
	v9start(cl);
	return c;
}

static void
v9close(Chan *c)
{
	Ctlr *cl;

	if((c->flag & COPEN) == 0 || (c->qid.type & QTDIR))
		return;
	cl = c->aux;
	if(cl != nil){
		v9flush(cl);
		decref(&cl->inuse);
	}
}

static i32
v9read(Chan *c, void *a, i32 n, i64 off)
{
	Ctlr *cl;
	Slot *s;
	i32 k;
	USED(off);

	if(c->qid.type & QTDIR)
		return devdirread(c, a, n, nil, 0, v9gen);
	cl = c->aux;
	s = cl->cur;
	if(s == nil){
		s = getreply(cl);
		s->rp = 0;
		cl->cur = s;
	}
	k = s->inlen - s->rp;
	if(k > n)
		k = n;
	memmove(a, s->in + s->rp, k);
	s->rp += k;
	if(s->rp >= s->inlen){
		cl->cur = nil;
		putslot(cl, s);
	}
	return k;
}

static i32
v9write(Chan *c, void *a, i32 n, i64 off)
{
	Ctlr *cl;
	Slot *s;
	u8 *m;
	USED(off);

	if(c->qid.type & QTDIR)
		error(Eperm);
	if(!v9pmessagevalid(a, n, Msize + 64))
		error("invalid 9P message framing");

	cl = c->aux;
	m = a;
	s = getslot(cl);

	s->op = m[BIT32SZ];
	switch(s->op){
	case Topen:
	case Tcreate:
	case Tread:
	case Tclunk:
	case Tremove:
		s->fid = GBIT32(m + BIT32SZ + BIT8SZ + BIT16SZ);
		break;
	}

	memmove(s->out, m, n);
	submit(cl, s, n);
	return n;
}

Dev virtio9pdevtab = {
	.dc = '9',
	.name = "virtio9p",

	.reset = v9reset,
	.init = devinit,
	.shutdown = devshutdown,
	.attach = v9attach,
	.walk = v9walk,
	.stat = v9stat,
	.open = v9open,
	.create = devcreate,
	.close = v9close,
	.read = v9read,
	.bread = devbread,
	.write = v9write,
	.bwrite = devbwrite,
	.remove = devremove,
	.wstat = devwstat,
};
