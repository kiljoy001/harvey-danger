/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in the top-level LICENSE.gpl file.
 *
 * Physical page allocator.  RAM is described by one free list of
 * extents starting above the kernel image; there is no firmware memory
 * map yet, so the caller passes the end of DRAM.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

enum {
	Pagesz = 4096,
	Maxextent = 256,
};

typedef struct Extent Extent;
struct Extent {
	u32 base;
	u32 size;
};

static Extent freeext[Maxextent];
static int nfreeext;
static u32 managedstart;
static u32 ramend;
static Lock physlock;

extern char __image_end[];

static u32
pageup(u32 n)
{
	return (n + Pagesz - 1) & ~(Pagesz - 1);
}

static void
addextent(u32 base, u32 size)
{
	int i;

	if(size == 0)
		return;
	if(nfreeext >= Maxextent)
		panic("physical free list exhausted");
	for(i = nfreeext; i > 0 && freeext[i-1].base > base; i--)
		freeext[i] = freeext[i-1];
	freeext[i].base = base;
	freeext[i].size = size;
	nfreeext++;
	if(i > 0 && freeext[i-1].base + freeext[i-1].size == base){
		freeext[i-1].size += size;
		for(; i < nfreeext-1; i++)
			freeext[i] = freeext[i+1];
		nfreeext--;
		i--;
	}
	if(i+1 < nfreeext && freeext[i].base + freeext[i].size == freeext[i+1].base){
		freeext[i].size += freeext[i+1].size;
		for(; i+1 < nfreeext-1; i++)
			freeext[i+1] = freeext[i+2];
		nfreeext--;
	}
}

void
arm32physinit(u32 end)
{
	u32 start;

	ramend = end;
	start = pageup((u32)__image_end);
	if(start >= ramend)
		panic("no memory above the image");
	managedstart = start;
	nfreeext = 0;
	addextent(start, ramend - start);
}

u64
physalloc(u64 size, int *color, void *tag)
{
	u32 align, base, b, end, oldbase, oldend;
	int i, j;

	USED(tag);
	if(size == 0 || size > ramend || (size & (Pagesz-1)))
		return 0;
	ilock(&physlock);
	align = (size & (size-1)) == 0 ? (u32)size : Pagesz;
	for(i = 0; i < nfreeext; i++){
		oldbase = freeext[i].base;
		oldend = oldbase + freeext[i].size;
		b = (oldbase + align - 1) & ~(align - 1);
		end = b + size;
		if(b < oldbase || end < b || end > oldend)
			continue;
		if(b == oldbase && end == oldend){
			for(j = i; j < nfreeext-1; j++)
				freeext[j] = freeext[j+1];
			nfreeext--;
		}else if(b == oldbase){
			freeext[i].base = end;
			freeext[i].size = oldend - end;
		}else if(end == oldend){
			freeext[i].size = b - oldbase;
		}else{
			freeext[i].size = b - oldbase;
			addextent(end, oldend - end);
		}
		if(color != nil)
			*color = 0;
		base = b;
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
	   base < managedstart || base + size < base || base + size > ramend)
		panic("physfree: invalid range");
	ilock(&physlock);
	for(i = 0; i < nfreeext; i++)
		if(base < freeext[i].base + freeext[i].size &&
		   base + size > freeext[i].base)
			panic("physfree: overlapping free range");
	addextent(base, size);
	iunlock(&physlock);
}

u32
arm32physfreepages(void)
{
	u32 pages, i;

	ilock(&physlock);
	pages = 0;
	for(i = 0; i < nfreeext; i++)
		pages += freeext[i].size / Pagesz;
	iunlock(&physlock);
	return pages;
}

char *
seprintphysstats(char *s, char *e)
{
	u32 bytes, i;

	ilock(&physlock);
	bytes = 0;
	for(i = 0; i < nfreeext; i++)
		bytes += freeext[i].size;
	iunlock(&physlock);
	return seprint(s, e, "%ludK physical memory free %ludK physical memory total\n",
		bytes / KiB, (ramend - managedstart) / KiB);
}
