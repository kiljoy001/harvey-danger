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
#include "mem.h"
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

	arm32setttbr0((u32)arm32l1);
	arm32setdacr(0x55555555);	/* all domains: client */
	arm32tlbflush();
	arm32cacheflush();
	arm32mmuenable();
}

void
arm32mmuoff(void)
{
	/* Not used yet; kept for the fault path and future SMP bring-up. */
}
