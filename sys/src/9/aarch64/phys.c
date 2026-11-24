/* Monotonic early physical-page allocator, before the MMU and page tables. */
#include "phys.h"
#ifdef ARM64_FULL_KERNEL
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#endif

enum { Pagesz = 4096 };

extern char _start[], __image_end[];
extern u64 arm64dmabase, arm64dmasize;

static u64 nextpage, ramend;
static Fdtmemory banks[Maxfdtmemory];
static u32 nbanks, curb;
static u64 totalram, maxbank;
#ifdef ARM64_FULL_KERNEL
static u64 managedstart;
#endif
static Fdtreserve excluded[Maxfdtreserve+1];
static u32 nexcluded;

#ifdef ARM64_FULL_KERNEL
static u64 pageup(u64);
typedef struct Extent Extent;
struct Extent {
	u64 base;
	u64 size;
};
enum { Maxextent = 1024 };
static Extent freeext[Maxextent];
static int nfreeext, physready;

static void
addextent(u64 base, u64 size)
{
	int i;

	if(size == 0)
		return;
	if(nfreeext >= Maxextent)
		panic("ARM64 physical free list exhausted");
	for(i = nfreeext; i > 0 && freeext[i-1].base > base; i--)
		freeext[i] = freeext[i-1];
	freeext[i].base = base;
	freeext[i].size = size;
	nfreeext++;
	if(i > 0 && freeext[i-1].base+freeext[i-1].size == base){
		freeext[i-1].size += size;
		for(; i < nfreeext-1; i++)
			freeext[i] = freeext[i+1];
		nfreeext--;
		i--;
	}
	if(i+1 < nfreeext && freeext[i].base+freeext[i].size == freeext[i+1].base){
		freeext[i].size += freeext[i+1].size;
		for(; i+1 < nfreeext-1; i++)
			freeext[i+1] = freeext[i+2];
		nfreeext--;
	}
}

static void
startfree(void)
{
	u64 p, bend, stop, skipto, low, high, end;
	u32 i, b;

	if(physready)
		return;
	managedstart = nextpage;
	for(b = curb; b < nbanks; b++){
		p = b == curb ? managedstart : banks[b].base;
		bend = banks[b].base + banks[b].size;
		while(p < bend){
			stop = bend;
			skipto = 0;
			for(i = 0; i < nexcluded; i++){
				if(excluded[i].size == 0)
					continue;
				end = excluded[i].base + excluded[i].size;
				if(end < excluded[i].base)
					panic("ARM64 physical reservation overflow");
				low = excluded[i].base & ~(u64)(Pagesz-1);
				high = pageup(end);
				if(high <= p)
					continue;
				if(low <= p){
					if(high > skipto)
						skipto = high;
				}else if(low < stop)
					stop = low;
			}
			if(skipto > p){
				p = skipto;
				continue;
			}
			if(stop > bend)
				stop = bend;
			if(stop > p)
				addextent(p, stop-p);
			p = stop;
		}
	}
	/* The DMA arena is published to user space; it must never be free. */
	if(arm64dmabase != 0 && arm64dmasize != 0){
		u64 abase = arm64dmabase, aend = arm64dmabase + arm64dmasize;
		for(i = 0; i < (u32)nfreeext; i++){
			if(freeext[i].base < aend &&
			   abase < freeext[i].base + freeext[i].size){
				print("ARM64 DMA-ARENA FREE %#llux+%#llux overlaps %#llux+%#llux\n",
				      abase, arm64dmasize, freeext[i].base, freeext[i].size);
				break;
			}
		}
	}
	physready = 1;
}
#endif

static u64
pageup(u64 n)
{
	if(n > ~(u64)0 - (Pagesz-1))
		return 0;
	return (n + Pagesz-1) & ~(u64)(Pagesz-1);
}

static int
inbanks(u64 base, u64 size)
{
	u32 i;
	u64 end;

	for(i = 0; i < nbanks; i++){
		end = banks[i].base + banks[i].size;
		if(end < banks[i].base)
			continue;
		if(base >= banks[i].base && base + size <= end)
			return 1;
	}
	return 0;
}

int
arm64physinit(Fdtinfo *info, usize dtb, usize dtbsize)
{
	u32 i;
	u64 start, end;

	if(info == nil || info->nusable == 0 ||
	   info->nusable > Maxfdtmemory || info->usable[0].size == 0 ||
	   dtb+dtbsize < dtb || info->nreserve > Maxfdtreserve)
		return -1;
	nbanks = info->nusable;
	totalram = 0;
	maxbank = 0;
	for(i = 0; i < nbanks; i++){
		banks[i] = info->usable[i];
		end = banks[i].base + banks[i].size;
		if(end < banks[i].base)
			return -1;
		totalram += banks[i].size;
		if(banks[i].size > maxbank)
			maxbank = banks[i].size;
	}
	ramend = banks[0].base + banks[0].size;
	start = (u64)__image_end;
	if((u64)_start < banks[0].base || start > ramend ||
	   (nextpage = pageup(start)) == 0 || nextpage > ramend)
		return -1;
	curb = 0;
	nexcluded = info->nreserve+1;
	for(i = 0; i < info->nreserve; i++)
		excluded[i] = info->reserve[i];
	excluded[info->nreserve].base = dtb;
	excluded[info->nreserve].size = dtbsize;
	return 0;
}

u64
arm64physpage(void)
{
	u64 p, end;
	u32 i, j;
	u64 *word;

#ifdef ARM64_FULL_KERNEL
	if(physready){
		int color = 0;
		p = physalloc(Pagesz, &color, nil);
		if(p == 0)
			return 0;
		word = (u64 *)p;
		for(j = 0; j < Pagesz/sizeof(u64); j++)
			word[j] = 0;
		return p;
	}
#endif
	p = nextpage;
	for(;;){
		if(curb >= nbanks)
			return 0;
		end = banks[curb].base + banks[curb].size;
		if(p < banks[curb].base)
			p = banks[curb].base;
		if(p > end || end - p < Pagesz){
			curb++;
			if(curb < nbanks)
				p = nextpage = banks[curb].base;
			continue;
		}
		for(i = 0; i < nexcluded; i++){
			end = excluded[i].base + excluded[i].size;
			if(p < end && p+Pagesz > excluded[i].base){
				p = pageup(end);
				break;
			}
		}
		if(i != nexcluded){
			if(p >= banks[curb].base + banks[curb].size){
				curb++;
				if(curb < nbanks)
					p = nextpage = banks[curb].base;
			}
			continue;
		}
		break;
	}
	nextpage = p+Pagesz;
	word = (u64 *)p;
	for(j = 0; j < Pagesz/sizeof(u64); j++)
		word[j] = 0;
	return p;
}

#ifdef ARM64_FULL_KERNEL
/*
 * physalloc/physfree run on every CPU: page faults, mmurelease from an IPI,
 * and the kernel heap all allocate or return pages concurrently.  splhi()
 * only masks interrupts on the local CPU, so the extent list needs a real
 * cross-CPU lock.
 */
static Lock physlock;

u64
physalloc(u64 size, int *color, void *tag)
{
	u64 align, base, end, oldend, oldbase;
	int i, j;

	USED(tag);
	if(size == 0 || size > maxbank || (size & (Pagesz-1)))
		return 0;
	ilock(&physlock);
	startfree();
	align = (size & (size-1)) == 0 ? size : Pagesz;
	for(i = 0; i < nfreeext; i++){
		oldbase = freeext[i].base;
		oldend = oldbase + freeext[i].size;
		base = (oldbase+align-1) & ~(align-1);
		end = base + size;
		if(base < oldbase || end < base || end > oldend)
			continue;
		if(base == oldbase && end == oldend){
			for(j = i; j < nfreeext-1; j++)
				freeext[j] = freeext[j+1];
			nfreeext--;
		}else if(base == oldbase){
			freeext[i].base = end;
			freeext[i].size = oldend-end;
		}else if(end == oldend){
			freeext[i].size = base-oldbase;
		}else{
			freeext[i].size = base-oldbase;
			addextent(end, oldend-end);
		}
		if(color != nil)
			*color = 0;
		iunlock(&physlock);
		return base;
	}
	iunlock(&physlock);
	return 0;
}

void
physfree(u64 base, u64 size)
{
	int i;

	if((base | size) & (Pagesz-1) || size == 0 ||
	   base < managedstart || base+size < base ||
	   !inbanks(base, size))
		panic("ARM64 physfree: invalid range");
	for(i = 0; i < nexcluded; i++)
		if(excluded[i].size != 0 &&
		   base < excluded[i].base+excluded[i].size &&
		   base+size > excluded[i].base)
			panic("ARM64 physfree: reserved range");
	ilock(&physlock);
	for(i = 0; i < nfreeext; i++)
		if(base < freeext[i].base+freeext[i].size &&
		   base+size > freeext[i].base)
			panic("ARM64 physfree: overlapping free range");
	addextent(base, size);
	iunlock(&physlock);
}

char *
seprintphysstats(char *s, char *e)
{
	u64 bytes;
	int i;

	ilock(&physlock);
	startfree();
	bytes = 0;
	for(i = 0; i < nfreeext; i++)
		bytes += freeext[i].size;
	iunlock(&physlock);
	/*
	 * The guest namespace is RAM-backed, so statvfs reports the managed
	 * physical range.  Print both numbers for userspace.
	 */
	return seprint(s, e, "%lluK physical memory free %lluK physical memory total\n",
	    bytes/KiB, totalram/KiB);
}

u64
arm64physfreepages(void)
{
	u64 pages;
	int i;

	ilock(&physlock);
	startfree();
	pages = 0;
	for(i = 0; i < nfreeext; i++)
		pages += freeext[i].size / Pagesz;
	iunlock(&physlock);
	return pages;
}

int
arm64physprobe(void)
{
	u64 small, large, again;
	int color;

	color = 0;
	small = physalloc(Pagesz, &color, nil);
	large = physalloc(2 * MiB, &color, nil);
	if(small == 0 || large == 0 || (large & (2 * MiB - 1)) != 0 ||
	   (small >= large && small < large + 2 * MiB))
		return -1;
	physfree(small, Pagesz);
	again = physalloc(Pagesz, &color, nil);
	if(again != small)
		return -1;
	physfree(again, Pagesz);
	physfree(large, 2 * MiB);
	return 0;
}
#endif
