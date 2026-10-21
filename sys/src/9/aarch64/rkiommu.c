/*
 * Rockchip IOMMU v2 for RK3568, reduced to the pieces Harvey needs.
 *
 * The hardware walk is a two-level table: 1024 directory entries, each
 * pointing at a 1024-entry page table of 4 KiB pages.  This is enough for
 * a 4 GiB IOVA space; RK3568 PTEs carry a 40-bit physical address so the
 * high DRAM bank can be mapped into that space.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "rkiommu.h"

enum {
	MmuDteaddr	= 0x00,
	MmuStatus	= 0x04,
	MmuCommand	= 0x08,
	MmuFaultaddr	= 0x0c,
	MmuZapone	= 0x10,
	MmuIntraw	= 0x14,
	MmuIntclear	= 0x18,
	MmuIntmask	= 0x1c,
	MmuIntstatus	= 0x20,
	MmuAutogating	= 0x24,

	StatusPaging	= 1<<0,
	StatusFault	= 1<<1,
	StatusStall	= 1<<2,
	StatusIdle	= 1<<3,
	StatusReplay	= 1<<4,
	StatusFaultwrite = 1<<5,
	StatusStallnot	= 1u<<31,

	CmdEnable	= 0,
	CmdDisable	= 1,
	CmdStall	= 2,
	CmdUnstall	= 3,
	CmdZap		= 4,
	CmdFaultdone	= 5,
	CmdReset	= 6,

	IrqFault	= 1,
	IrqBus		= 2,
	IrqMask		= IrqFault|IrqBus,

	PteValid	= 1<<0,
	PteReadable	= 1<<1,
	PteWritable	= 1<<2,
	PtePrivate	= 1<<3,
	AutogateNofetch	= 1u<<31,
};

static Rkiommu iommus[Maxrkiommu];
static int niommu;

static u32
rkread(Rkiommu *i, int n, u32 reg)
{
	return ((volatile u32 *)i->base[n])[reg/4];
}

static void
rkwrite(Rkiommu *i, int n, u32 reg, u32 v)
{
	((volatile u32 *)i->base[n])[reg/4] = v;
}

static void
rkcommand(Rkiommu *i, u32 cmd)
{
	int n;

	for(n = 0; n < i->nbase; n++)
		rkwrite(i, n, MmuCommand, cmd);
}

static int
rkwait(Rkiommu *i, u32 reg, u32 mask, u32 want)
{
	int n, spin;

	for(spin = 0; spin < 1000; spin++){
		int ok = 1;

		for(n = 0; n < i->nbase; n++)
			if((rkread(i, n, reg) & mask) != want)
				ok = 0;
		if(ok)
			return 0;
		microdelay(100);
	}
	return -1;
}

/*
 * v2 PTE/DTE address packing.
 *   31:12 physical address bits 31:0
 *   11:8  physical address bits 35:32
 *    7:4  physical address bits 39:36
 */
static u32
rkaddr(u64 pa)
{
	u64 v;

	v = pa & 0xfffff000ULL;
	v |= (pa & 0xf00000000ULL) >> 24;
	v |= (pa & 0xf000000000ULL) >> 32;
	return (u32)v;
}

static u32
rkdte(u64 ptpa)
{
	return rkaddr(ptpa) | PteValid;
}

static u32
rkpte(u64 pa, int writable)
{
	u32 p;

	p = rkaddr(pa) | PteValid | PteReadable;
	if(writable)
		p |= PteWritable;
	return p;
}

static int
rkpalloc(u64 *pa)
{
	int color;

	color = 0;
	*pa = physalloc(RkiommuPagesz, &color, nil);
	if(*pa == 0)
		return -1;
	memset(KADDR(*pa), 0, RkiommuPagesz);
	return 0;
}

static void
rkptefree(Rkdomain *d)
{
	int i;

	if(d == nil)
		return;
	for(i = 0; i < RkiommuDtes; i++){
		if(d->ptpa[i] != 0)
			physfree(d->ptpa[i], RkiommuPagesz);
		d->pt[i] = nil;
		d->ptpa[i] = 0;
	}
	if(d->dtpa != 0)
		physfree(d->dtpa, RkiommuPagesz);
	d->dt = nil;
	d->dtpa = 0;
}

Rkdomain *
rkiommudomain(void)
{
	Rkdomain *d;

	d = mallocz(sizeof *d, 1);
	if(d == nil)
		return nil;
	if(rkpalloc(&d->dtpa) < 0){
		free(d);
		return nil;
	}
	d->dt = KADDR(d->dtpa);
	return d;
}

void
rkiommudomainfree(Rkdomain *d)
{
	if(d == nil)
		return;
	rkptefree(d);
	free(d);
}

static u32 *
rkgetpt(Rkdomain *d, u32 dtei)
{
	u64 pa;

	if(d->pt[dtei] != nil)
		return d->pt[dtei];
	if(rkpalloc(&pa) < 0)
		return nil;
	d->ptpa[dtei] = pa;
	d->pt[dtei] = KADDR(pa);
	d->dt[dtei] = rkdte(pa);
	return d->pt[dtei];
}

int
rkiommuattach(Rkiommu *i, Rkdomain *d)
{
	if(i == nil || d == nil)
		return -1;
	ilock(&d->l);
	i->dom = d;
	iunlock(&d->l);
	if(i->enabled){
		int n;

		for(n = 0; n < i->nbase; n++)
			rkwrite(i, n, MmuDteaddr, rkdte(d->dtpa));
		rkcommand(i, CmdZap);
	}
	return 0;
}

int
rkiommumap(Rkiommu *i, u64 iova, u64 pa, usize size, int writable)
{
	Rkdomain *d;
	u32 dtei, ptei, *pt;
	usize n;

	if(i == nil || i->dom == nil)
		return -1;
	if((iova|pa|size) & (RkiommuPagesz-1) || size == 0)
		return -1;
	if(iova+size < iova || iova+size > (1ULL<<32))
		return -1;
	d = i->dom;
	ilock(&d->l);
	for(n = 0; n < size; n += RkiommuPagesz){
		dtei = (iova+n) >> RkiommuDteshift & RkiommuDtemask;
		ptei = (iova+n) >> RkiommuPteshift & RkiommuPtemask;
		pt = rkgetpt(d, dtei);
		if(pt == nil){
			iunlock(&d->l);
			return -1;
		}
		if((pt[ptei] & PteValid) != 0){
			iunlock(&d->l);
			return -1;
		}
		pt[ptei] = rkpte(pa+n, writable);
	}
	coherence();
	iunlock(&d->l);
	if(i->enabled)
		rkiommuzap(i, iova, size);
	return 0;
}

int
rkiommuunmap(Rkiommu *i, u64 iova, usize size)
{
	Rkdomain *d;
	u32 dtei, ptei, *pt;
	usize n;

	if(i == nil || i->dom == nil)
		return -1;
	if((iova|size) & (RkiommuPagesz-1) || size == 0 ||
	   iova+size < iova || iova+size > (1ULL<<32))
		return -1;
	d = i->dom;
	ilock(&d->l);
	for(n = 0; n < size; n += RkiommuPagesz){
		dtei = (iova+n) >> RkiommuDteshift & RkiommuDtemask;
		ptei = (iova+n) >> RkiommuPteshift & RkiommuPtemask;
		pt = d->pt[dtei];
		if(pt == nil)
			continue;
		pt[ptei] = 0;
	}
	coherence();
	iunlock(&d->l);
	if(i->enabled)
		rkiommuzap(i, iova, size);
	return 0;
}

void
rkiommuzap(Rkiommu *i, u64 iova, usize size)
{
	u64 a;

	USED(size);
	for(a = iova; a < iova+size; a += RkiommuPagesz)
		for(int n = 0; n < i->nbase; n++)
			rkwrite(i, n, MmuZapone, (u32)a);
	rkcommand(i, CmdZap);
}

void
rkiommufault(Rkiommu *i)
{
	int n;
	u32 st, addr;

	for(n = 0; n < i->nbase; n++){
		st = rkread(i, n, MmuIntstatus);
		if(st == 0)
			continue;
		addr = rkread(i, n, MmuFaultaddr);
		print("rkiommu %s: fault status %#ux iova %#ux%s\n",
			i->name, st, addr,
			(rkread(i, n, MmuStatus) & StatusFaultwrite) ? " write" : "");
		rkwrite(i, n, MmuIntclear, st);
		rkcommand(i, CmdFaultdone);
	}
}

int
rkiommuenable(Rkiommu *i)
{
	int n;
	u32 v;

	if(i == nil || i->dom == nil || i->nbase == 0)
		return -1;
	if(i->enabled)
		return 0;

	rkcommand(i, CmdReset);
	if(rkwait(i, MmuDteaddr, 0xffffffff, 0) < 0)
		return -1;

	for(n = 0; n < i->nbase; n++){
		rkwrite(i, n, MmuDteaddr, rkdte(i->dom->dtpa));
		rkcommand(i, CmdZap);
		rkwrite(i, n, MmuIntmask, IrqMask);
		v = rkread(i, n, MmuAutogating);
		rkwrite(i, n, MmuAutogating, v | AutogateNofetch);
	}
	rkcommand(i, CmdEnable);
	if(rkwait(i, MmuStatus, StatusPaging, StatusPaging) < 0){
		print("rkiommu %s: paging enable timed out\n", i->name);
		return -1;
	}
	i->enabled = 1;
	return 0;
}

int
rkiommudisable(Rkiommu *i)
{
	if(i == nil || !i->enabled)
		return 0;
	rkcommand(i, CmdDisable);
	if(rkwait(i, MmuStatus, StatusPaging, 0) < 0)
		return -1;
	i->enabled = 0;
	return 0;
}

Rkiommu *
rkiommubybase(u64 base)
{
	int n;

	for(n = 0; n < niommu; n++)
		if(iommus[n].base[0] == base ||
		   (iommus[n].nbase > 1 && iommus[n].base[1] == base))
			return &iommus[n];
	return nil;
}

Rkiommu *
rkiommuinit(Fdtinfo *info)
{
	int n, j;
	Rkiommu *i;

	niommu = 0;
	for(n = 0; n < info->ndev && niommu < Maxrkiommu; n++){
		Fdtdev *d;

		d = &info->dev[n];
		if(d->clas != Fdtclassiommu || d->base == 0)
			continue;
		i = &iommus[niommu++];
		memset(i, 0, sizeof *i);
		strncpy(i->name, d->name, sizeof i->name - 1);
		i->base[0] = d->base;
		i->size[0] = d->size;
		i->nbase = 1;
		if(d->base2 != 0){
			i->base[1] = d->base2;
			i->size[1] = d->size2;
			i->nbase = 2;
		}
		i->irq = d->irq;
		print("rkiommu: %s base %#llux", i->name, i->base[0]);
		for(j = 1; j < i->nbase; j++)
			print("/%#llux", i->base[j]);
		print(" irq %ud\n", i->irq);
	}
	return niommu != 0 ? iommus : nil;
}

int
rkiommuptest(void)
{
	Rkiommu t;
	Rkdomain *d;
	u64 pa;
	u32 dtei, ptei;
	int r;

	memset(&t, 0, sizeof t);
	d = rkiommudomain();
	if(d == nil)
		return -1;
	t.dom = d;
	if(rkpalloc(&pa) < 0){
		rkiommudomainfree(d);
		return -1;
	}
	r = rkiommumap(&t, 0x10000000ULL, pa, RkiommuPagesz, 1);
	if(r == 0){
		dtei = 0x10000000ULL >> RkiommuDteshift & RkiommuDtemask;
		ptei = 0x10000000ULL >> RkiommuPteshift & RkiommuPtemask;
		if(d->pt[dtei] == nil ||
		   (d->pt[dtei][ptei] & PteValid) == 0 ||
		   (d->pt[dtei][ptei] & PteWritable) == 0)
			r = -1;
	}
	if(r == 0)
		r = rkiommuunmap(&t, 0x10000000ULL, RkiommuPagesz);
	physfree(pa, RkiommuPagesz);
	rkiommudomainfree(d);
	return r;
}
