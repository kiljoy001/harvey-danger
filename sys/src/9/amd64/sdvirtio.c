/*
 * Virtio block device over 9vm's simplified virtio 1.0 MMIO transport.
 *
 * 9vm exposes the disk as PCI 1AF4:1001 whose BAR0 is a flat register
 * block rather than the standard virtio capability layout: one queue,
 * no feature negotiation, and synchronous completion -- the hypervisor
 * processes a kicked request before the notify write returns, so there
 * is nothing to poll.  The interrupt exists only to deassert the line;
 * the ISR is cleared on every request anyway.
 *
 * Ported from purgatorio os/pc/sdvirtio.c onto devsd's SDifc interface.
 * With this driver /dev/sd00/{data,ctl,part,raw} appear, so GEFS can be
 * given a real block device instead of a 9P-exported image file.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "../port/sd.h"

enum {
	QSZ		= 16,		/* matches 9vm's queue size */

	/* simplified virtio MMIO registers (byte offsets) */
	VDevFeatSel	= 0x000,
	VNumQueues	= 0x012,
	VDevStatus	= 0x014,
	VQueueSel	= 0x016,
	VQueueSize	= 0x018,
	VQueueReady	= 0x01C,
	VQueueDesc	= 0x020,
	VQueueAvail	= 0x028,
	VQueueUsed	= 0x030,
	VQueueNotify	= 0x100,
	VISR		= 0x200,
	VDevConfig	= 0x300,

	/* device status */
	VSAck		= 1,
	VSDriver	= 2,
	VSDriverOk	= 4,

	/* virtio-blk request types */
	VBlkIn		= 0,		/* read */
	VBlkOut		= 1,		/* write */

	/* descriptor flags */
	VDescNext	= 1,
	VDescWrite	= 2,
};

typedef struct Vdesc Vdesc;
struct Vdesc {
	u64	addr;
	u32	len;
	u16	flags;
	u16	next;
};

typedef struct Vring Vring;
struct Vring {
	Vdesc	desc[QSZ];
	struct {
		u16	flags;
		u16	idx;
		u16	ring[QSZ];
	} avail;
	struct {
		u16	flags;
		u16	idx;
		struct {
			u32	id;
			u32	len;
		} ring[QSZ];
	} used;
};

typedef struct Vbdev Vbdev;
struct Vbdev {
	Pcidev	*pci;
	u64	pa;		/* BAR0 physical address */
	u8	*mmio;		/* BAR0 kernel virtual address */
	Vring	*ring;
	QLock	lk;
	u64	capacity;	/* 512-byte sectors */
	u64	reqhdr[2];	/* 16-byte request header */
	u8	reqstatus;
};

SDifc sdvirtioifc;

static u32
vbr32(Vbdev *vb, int off)
{
	return *(volatile u32*)(vb->mmio + off);
}

static void
vbw32(Vbdev *vb, int off, u32 v)
{
	*(volatile u32*)(vb->mmio + off) = v;
}

static void
vbw16(Vbdev *vb, int off, u16 v)
{
	*(volatile u16*)(vb->mmio + off) = v;
}

static void
vbw8(Vbdev *vb, int off, u8 v)
{
	*(volatile u8*)(vb->mmio + off) = v;
}

static void
sdvirtiointerrupt(Ureg *u, void *a)
{
	Vbdev *vb;

	USED(u);
	vb = a;
	(void)vbr32(vb, VISR);	/* read clears the ISR and deasserts the line */
}

static i32
sdvirtiobio(SDunit *unit, int lun, int write, void *data, i32 nb, u64 bno)
{
	Vbdev *vb;
	Vring *r;
	u8 *hdr;
	u64 n;

	USED(lun);
	if(nb <= 0)
		return 0;
	vb = unit->dev->ctlr;
	r = vb->ring;
	n = (u64)nb * unit->secsize;

	qlock(&vb->lk);
	hdr = (u8*)vb->reqhdr;
	*(u32*)(hdr + 0) = write ? VBlkOut : VBlkIn;
	*(u32*)(hdr + 4) = 0;
	*(u64*)(hdr + 8) = bno;

	/* descriptor 0: request header (device reads) */
	r->desc[0].addr = PADDR(hdr);
	r->desc[0].len = 16;
	r->desc[0].flags = VDescNext;
	r->desc[0].next = 1;

	/* descriptor 1: data (device writes on read, reads on write) */
	r->desc[1].addr = PADDR(data);
	r->desc[1].len = n;
	r->desc[1].flags = (write ? 0 : VDescWrite) | VDescNext;
	r->desc[1].next = 2;

	/* descriptor 2: status byte (device writes) */
	r->desc[2].addr = PADDR(&vb->reqstatus);
	r->desc[2].len = 1;
	r->desc[2].flags = VDescWrite;
	r->desc[2].next = 0;

	/* publish the head and kick; 9vm completes the request inline */
	vb->reqstatus = 0xFF;
	r->avail.ring[r->avail.idx % QSZ] = 0;
	r->avail.idx++;
	vbw16(vb, VQueueNotify, 0);
	(void)vbr32(vb, VISR);

	if(vb->reqstatus != 0){
		qunlock(&vb->lk);
		return -1;
	}
	qunlock(&vb->lk);
	return n;
}

static int
sdvirtioenable(SDev *sdev)
{
	Vbdev *vb;
	u64 pa;

	vb = sdev->ctlr;
	if(vb->mmio != nil)
		return 1;
	if((vb->mmio = vmap(vb->pa, 4096)) == nil)
		return 0;

	/* reset (also deasserts the interrupt line) */
	vbw8(vb, VDevStatus, 0);

	vb->ring = mallocz(sizeof(Vring), 1);
	if(vb->ring == nil)
		return 0;
	memset(vb->ring, 0, sizeof(Vring));

	/* no feature negotiation */
	vbw32(vb, VDevFeatSel, 0);
	vbw8(vb, VDevStatus, VSAck | VSDriver);

	/* set up queue 0 */
	vbw16(vb, VQueueSel, 0);
	pa = PADDR(&vb->ring->desc[0]);
	vbw32(vb, VQueueDesc, (u32)pa);
	vbw32(vb, VQueueDesc + 4, (u32)(pa >> 32));
	pa = PADDR(&vb->ring->avail);
	vbw32(vb, VQueueAvail, (u32)pa);
	vbw32(vb, VQueueAvail + 4, (u32)(pa >> 32));
	pa = PADDR(&vb->ring->used);
	vbw32(vb, VQueueUsed, (u32)pa);
	vbw32(vb, VQueueUsed + 4, (u32)(pa >> 32));
	vbw16(vb, VQueueReady, 1);

	vbw8(vb, VDevStatus, VSAck | VSDriver | VSDriverOk);

	/* capacity in 512-byte sectors */
	vb->capacity = vbr32(vb, VDevConfig);
	vb->capacity |= (u64)vbr32(vb, VDevConfig + 4) << 32;

	print("sdvirtio: %llud sectors (%llud MB)\n",
	      vb->capacity, vb->capacity / 2048);

	if(vb->pci != nil && vb->pci->intl != 0 && vb->pci->intl != 0xFF)
		intrenable(vb->pci->intl, sdvirtiointerrupt, vb,
			   vb->pci->tbdf, "virtio");

	return 1;
}

static int
sdvirtioverify(SDunit *unit)
{
	Vbdev *vb;

	vb = unit->dev->ctlr;
	if(vb->capacity == 0)
		return 0;
	memset(unit->inquiry, 0, sizeof(unit->inquiry));
	unit->inquiry[0] = 0x00;	/* direct-access device */
	unit->sectors = 0;		/* force devsd to re-read geometry */
	unit->secsize = 0;
	return 1;
}

static int
sdvirtioonline(SDunit *unit)
{
	Vbdev *vb;

	vb = unit->dev->ctlr;
	if(vb->capacity == 0)
		return 0;
	unit->sectors = vb->capacity;
	unit->secsize = 512;
	return 0;
}

static SDev*
sdvirtiopnp(void)
{
	Pcidev *p;
	Vbdev *vb;
	SDev *sdev;

	p = nil;
	while((p = pcimatch(p, 0x1AF4, 0x1001)) != nil){
		vb = mallocz(sizeof(Vbdev), 1);
		if(vb == nil)
			break;
		vb->pci = p;
		vb->pa = p->mem[0].bar;

		sdev = mallocz(sizeof(SDev), 1);
		if(sdev == nil){
			free(vb);
			break;
		}
		sdev->ifc = &sdvirtioifc;
		sdev->ctlr = vb;
		sdev->nunit = 1;
		sdev->idno = 'V';	/* /dev/sdV0 */
		print("sdvirtio: 1AF4:1001 at BAR %x, irq %d\n",
		      p->mem[0].bar, p->intl);
		return sdev;
	}
	return nil;
}

SDifc sdvirtioifc = {
	"virtio",		/* name */

	sdvirtiopnp,		/* pnp */
	nil,			/* legacy */
	sdvirtioenable,		/* enable */
	nil,			/* disable */

	sdvirtioverify,		/* verify */
	sdvirtioonline,		/* online */
	nil,			/* rio */
	nil,			/* rctl */
	nil,			/* wctl */

	sdvirtiobio,		/* bio */
	nil,			/* probe */
	nil,			/* clear */
	nil,			/* rtopctl */
	nil,			/* wtopctl */
};
