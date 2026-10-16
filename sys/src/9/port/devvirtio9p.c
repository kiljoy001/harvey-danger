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
#include "io.h"
#include "../port/error.h"

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
	Pcidev  *pci;
	Vconfig *cfg;
	u8      *isr;
	u8      *notify;
	u32     notifyoffmult;
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
	Ref     inuse;

	Ctlr    *next;
};

static Ctlr *ctlrs;
static int  probed;

static void v9interrupt(Ureg*, void*);

static Vqueue*
mkvqueue(int size)
{
	Vqueue *q;
	u8 *p;
	int i;

	q = malloc(sizeof(*q) + sizeof(void*) * size);
	p = mallocalign(
		PGROUND(sizeof(Vdesc) * size +
			VringSize +
			sizeof(u16) * size +
			sizeof(u16)) +
		PGROUND(VringSize +
			sizeof(Vused) * size +
			sizeof(u16)),
		PGSZ, 0, 0);
	if(p == nil || q == nil){
		print("virtio9p: no memory for Vqueue\n");
		free(p);
		free(q);
		return nil;
	}

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

static Pcicap*
findvirtiocap(Pcidev *p, int typ)
{
	for(Pcicap *c = p->caplist; c != nil; c = c->link){
		if(c->vndr == PciCapVND && c->type == typ)
			return c;
	}
	return nil;
}

static void*
virtiomapregs(Pcidev *p, Pcicap *cap)
{
	if(cap == nil || cap->bar >= nelem(p->mem))
		return nil;
	u64 base = p->mem[cap->bar].bar & ~0xFULL;
	u64 addr = base + cap->offset;
	return vmap(addr, cap->length);
}

static void
readtag(Ctlr *c)
{
	Pcicap *cap;
	V9cfg *dc;
	int n;

	c->tag[0] = 0;
	if((c->feat[0] & Fmounttag) == 0)
		return;
	cap = findvirtiocap(c->pci, 4);
	if(cap == nil)
		return;
	dc = virtiomapregs(c->pci, cap);
	if(dc == nil)
		return;
	n = dc->taglen;
	if(n >= sizeof(c->tag))
		n = sizeof(c->tag) - 1;
	memmove(c->tag, (u8*)dc + sizeof(dc->taglen), n);
	c->tag[n] = 0;
}

static void
v9probe(void)
{
	Ctlr *c, **tail;
	Vconfig *cfg;
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
		if(cap1 == nil)
			continue;
		cfg = virtiomapregs(p, cap1);
		if(cfg == nil)
			continue;

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
		c->isr = virtiomapregs(p, cap3);
		if(c->isr == nil){
baddev:
			free(c);
			continue;
		}

		c->notify = virtiomapregs(p, cap2);
		if(c->notify == nil)
			goto baddev;
		c->notifyoffmult = pcicfgr32(p, cap2->offset + 16);

		/* Reset then acknowledge */
		cfg->status = 0;
		while(cfg->status != 0)
			delay(1);
		cfg->status = Acknowledge | Driver;

		/* Negotiate features: VERSION_1 (bit 32) and mount tag */
		cfg->devfeatsel = 1;
		c->feat[1] = cfg->devfeat;
		cfg->devfeatsel = 0;
		c->feat[0] = cfg->devfeat;
		cfg->drvfeatsel = 1;
		cfg->drvfeat = c->feat[1] & 1;
		cfg->drvfeatsel = 0;
		cfg->drvfeat = c->feat[0] & Fmounttag;
		cfg->status |= FeaturesOk;
		if((cfg->status & FeaturesOk) == 0){
			print("virtio9p: device rejected features\n");
			goto baddev;
		}

		/* Request queue 0 */
		cfg->queuesel = 0;
		n = cfg->queuesize;
		if(n == 0 || (n & (n - 1)) != 0){
			print("virtio9p: bad queue size %d\n", n);
			goto baddev;
		}
		q = mkvqueue(n);
		if(q == nil)
			goto baddev;
		q->idx = 0;
		q->notify = (u8*)c->notify + c->notifyoffmult * cfg->queuenotifyoff;
		c->vq = q;
		coherence();
		cfg->queuedesc = PADDR(q->desc);
		cfg->queueavail = PADDR(q->avail);
		cfg->queueused = PADDR(q->used);

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
}

static void
v9start(Ctlr *c)
{
	if(c->started)
		return;
	pcisetbme(c->pci);
	intrenable(c->pci->intl, v9interrupt, c, c->pci->tbdf, "virtio9p");
	coherence();
	c->cfg->queuesel = 0;
	c->cfg->queueenable = 1;
	c->cfg->status |= DriverOk;
	c->started = 1;
}

static void
vqharvest(Ctlr *c)
{
	Vqueue *q;
	Slot *s;
	int id, free, m, ix;

	q = c->vq;
	m = q->size - 1;
	ilock(&q->l);
	while((q->lastused ^ q->used->idx) & 0xFFFF){
		ix = q->lastused & m;
		id = q->usedent[ix].id;
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
}

static void
v9interrupt(Ureg *ur, void *arg)
{
	Ctlr *c = arg;
	USED(ur);
	if(c->isr[0] & 1)
		vqharvest(c);
}

static int
havereply(void *arg)
{
	return ((Ctlr*)arg)->donehd != nil;
}

static int
haveslot(void *arg)
{
	return ((Ctlr*)arg)->sfree != nil;
}

static Slot*
getslot(Ctlr *c)
{
	Proc *up = externup();
	Vqueue *q;
	Slot *s;

	q = c->vq;
	ilock(&q->l);
	while(c->sfree == nil){
		iunlock(&q->l);
		while(waserror())
			;
		tsleep(&c->wwait, haveslot, c, 1000);
		poperror();
		vqharvest(c);
		ilock(&q->l);
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
	while(c->donehd == nil){
		iunlock(&q->l);
		while(waserror())
			;
		tsleep(&c->rwait, havereply, c, 1000);
		poperror();
		if(c->donehd == nil)
			vqharvest(c);
		ilock(&q->l);
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
	if((q->used->flags & 1) == 0)
		*((u16*)q->notify) = q->idx;
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

	if(c->qid.type & QTDIR)
		return devopen(c, omode, nil, 0, v9gen);
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
	if(n < BIT32SZ + BIT8SZ + BIT16SZ)
		error("invalid 9P message");
	if(n > Msize + 64)
		error("9P message exceeds buffer size");

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
