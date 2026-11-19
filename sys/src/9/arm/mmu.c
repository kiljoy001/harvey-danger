/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * ARMv7-A short-descriptor MMU bring-up.
 *
 * The bootstrap kernel runs on an identity map built from 1 MiB sections:
 * RAM is normal write-back memory, everything below it is device memory.
 * That is enough to enter C with caches enabled, and it keeps the kernel's
 * physical and virtual addresses equal so the early boot code can use
 * literal addresses from the linker script.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "arm.h"

/*
 * Section descriptor bits (ARMv7-A short-descriptor format).  The names
 * follow the ARM ARM: TEX at 14:12, AP[2] at 15, AP[1:0] at 11:10,
 * S at 16, XN at 4, C at 3, B at 2.
 */
enum {
	SecSection	= 2,
	SecB		= 1 << 2,
	SecC		= 1 << 3,
	SecXn		= 1 << 4,
	SecAp2		= 1 << 15,
	SecTexShift	= 12,
	SecS		= 1 << 16,

	/* privileged read/write, no user access */
	ApPrivRW	= 1 << 10,
	/* privileged and user read/write */
	ApFullRW	= 3 << 10,

	/* TEX=001, C=1, B=1: outer and inner write-back, write-allocate */
	AttrNormal	= (1 << SecTexShift) | SecC | SecB | SecS,
	/* TEX=000, C=0, B=1: shareable device, never executed */
	AttrDevice	= SecB | SecXn,
};

u32 arm32l1[4096] __attribute__((aligned(16384)));

static u32
section(u32 pa, u32 attr)
{
	return (pa & 0xfff00000) | ApPrivRW | attr | SecSection;
}

void
arm32mmuinit(void)
{
	int i;
	u32 pa;

	for(i = 0; i < 4096; i++)
		arm32l1[i] = 0;

	/* All of RAM below 0x80000000 is normal memory. */
	for(pa = PHYSDRAM; pa < 0x80000000; pa += MiB)
		arm32l1[pa >> 20] = section(pa, AttrNormal);

	/* Devices (GIC at 0x08000000, PL011 at 0x09000000, ...) below RAM. */
	for(pa = 0; pa < PHYSDRAM; pa += MiB)
		arm32l1[pa >> 20] = section(pa, AttrDevice);

	/*
	 * The high-vector page lives at 0xffff0000.  There is no RAM there,
	 * so point that section at the first MiB of DRAM (below the image).
	 */
	arm32l1[0xfff] = section(PHYSDRAM, AttrNormal);

	arm32setttbr0((u32)arm32l1);
	arm32setdacr(0x55555555);	/* all domains: client */
	arm32tlbflush();
	arm32cacheflush();
	arm32mmuenable();
}

/*
 * Process address spaces.
 *
 * The kernel keeps its identity-mapped sections; a process gets a copy
 * of them (RAM, the vector page and the two device regions the kernel
 * touches from an interrupt) plus coarse L2 tables for user pages below
 * 0x40000000.  Section entries in the user range are never copied: the
 * user owns everything below USTKTOP except the device windows.
 */
enum {
	L1Entries	= 4096,
	L1Bytes		= L1Entries * 4,
	L2Entries	= 256,
	L2Bytes		= L2Entries * 4,

	/* small page descriptor: bits[1:0] = 0b10, bit0 = XN */
	L2Small		= 2,
	L2Xn		= 1 << 0,
	L2B		= 1 << 2,
	L2C		= 1 << 3,
	L2Tex		= 1 << 6,	/* TEX[2] w/ TEX[1:0]=00: write-allocate */
	L2Share		= 1 << 10,

	ApUser		= 3 << 4,	/* AP[1:0] = 11: user read/write */
	ApUserRo	= 2 << 4,

	L2Normal	= L2Small | L2B | L2C | L2Tex | L2Share,
	L2Device	= L2Small | L2B | L2Xn,
};

static void
kernmap(u32 *l1)
{
	int i;

	for(i = 0; i < L1Entries; i++)
		l1[i] = 0;

	/* RAM above the user range, identity mapped. */
	for(i = PHYSDRAM >> 20; i < 0x800; i++)
		l1[i] = arm32l1[i];

	/* The high vector page. */
	l1[0xfff] = arm32l1[0xfff];

	/* Device windows the kernel touches while a process is running. */
	l1[UARTBASE >> 20] = arm32l1[UARTBASE >> 20];
	l1[0x08000000 >> 20] = arm32l1[0x08000000 >> 20];
}

static u32 *
l1of(Proc *p)
{
	u32 pa, *l1;

	if(p == nil)
		return arm32l1;
	if(p->MMU.mmul1pa == 0){
		pa = physalloc(L1Bytes, nil, nil);
		if(pa == 0)
			panic("mmuput: no memory for a level-1 table");
		l1 = KADDR(pa);
		kernmap(l1);
		p->MMU.mmul1pa = pa;
	}
	return KADDR(p->MMU.mmul1pa);
}

void
mmuput(usize va, Page *page, u32 attr)
{
	Proc *p;
	u32 *l1, *l2, pa, *e, flags;
	u32 size;

	p = externup();
	if(p == nil || page == nil || va >= USTKTOP)
		panic("mmuput: invalid process or address");
	size = PGSZ;
	if((va & (size-1)) || (page->pa & (size-1)))
		panic("mmuput: unaligned mapping");

	l1 = l1of(p);
	e = &l1[va >> 20];
	if((*e & 3) != 1){		/* coarse */
		/* physalloc deals in whole pages; a 1 KiB table uses one. */
		pa = physalloc(PGSZ, nil, nil);
		if(pa == 0)
			panic("mmuput: no memory for a level-2 table");
		memset(KADDR(pa), 0, L2Bytes);
		*e = (pa & ~0x3ff) | 1;
	}
	l2 = KADDR(*e & ~0x3ff);

	flags = L2Normal | ApUser;
	if(!(attr & PTEWRITE))
		flags = L2Normal | ApUserRo;
	if(attr & PTENOEXEC)
		flags |= L2Xn;
	if(attr & PTEUNCACHED)
		flags = L2Device | ApUser;

	l2[(va >> 12) & (L2Entries-1)] = (page->pa & ~(size-1)) | flags;
	coherence();
}

void
arm32mmuswitch(u32 l1pa)
{
	arm32setttbr0(l1pa != 0 ? l1pa : (u32)arm32l1);
	arm32tlbflush();
}

void
arm32mmurelease(Proc *p)
{
	u32 *l1;
	int i;

	if(p == nil || p->MMU.mmul1pa == 0)
		return;
	l1 = KADDR(p->MMU.mmul1pa);
	for(i = 0; i < (USTKTOP >> 20); i++)
		if((l1[i] & 3) == 1)
			physfree(l1[i] & ~0x3ff, PGSZ);
	physfree(p->MMU.mmul1pa, L1Bytes);
	p->MMU.mmul1pa = 0;
}
