/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * ARMv7 memory and machine definitions.  The first target is QEMU's
 * 32-bit "virt" board: RAM at 0x40000000, PL011 at 0x09000000 and a
 * GIC-400 at 0x08000000.
 */
#ifndef HARVEY_ARM_MEM_H
#define HARVEY_ARM_MEM_H

#define KiB	1024u
#define MiB	1048576u
#define GiB	1073741824u

#define PGSZ		(4*KiB)
#define BIGPGSZ		(2*MiB)
#define PGSHIFT		12
#define NPGSZ		2
#define HZ		100
#define KGUARD		(4*KiB)
#define PGROUND(s)	ROUND(s, BY2PG)
#define ROUND(s, sz)	(((s)+(sz-1))&~(sz-1))
#define BY2SE		4
#define BY2WD		4
#define BY2V		8
#define BI2BY		8

#define MAXMACH		4
#define MACHMAX		MAXMACH
#define MACHSIZE	BY2PG
#define L1SIZE		(4*BY2PG)	/* 4096 section entries */

#define KSTACK		(8*KiB)
#define STACKALIGN(sp)	((sp) & ~7)

/*
 * The kernel runs on an identity map: KZERO is the physical base of DRAM,
 * so KADDR/PADDR are no-ops and device registers are reachable at their
 * documented addresses.  User processes get the low 1 GiB.
 */
#define PHYSDRAM	0x40000000
#define KSEG0		0x40000000
#define KZERO		KSEG0
#define KTZERO		0x40010000
#define CONFADDR	(KZERO+0x100)
#define MACHADDR	(KZERO+0x2000)
#define L1		(KZERO+0x4000)
#define L2		(KZERO+0x8000)

#define BY2PG		PGSZ
#define PGSHFT		12

/*
 * User address space for 32-bit processes: text starts above the zero
 * page, the stack and the NIX call pages sit just under the kernel's
 * identity-mapped RAM at 0x40000000.
 */
#define UZERO		0
#define UTZERO		(UZERO+BY2PG)
#define UTROUND(x)	PGROUND(x)
#define USTKTOP		0x40000000
#define USTKSIZE	(8*1024*1024)
#define TSTKTOP		(USTKTOP-USTKSIZE)
#define NIXCALL		(TSTKTOP-USTKSIZE)
#define HEAPTOP		(NIXCALL-32*MiB)

#define HVECTORS	0xffff0000

#define BLOCKALIGN	64

#define PTEMAPMEM	(1024*1024)
#define PTEPERTAB	(PTEMAPMEM/BY2PG)
#define SEGMAPSIZE	1984
#define SSEGMAPSIZE	16
#define PPN(x)		((x)&~(BY2PG-1))

/* Artificial bits used by the port's segment code. */
#define PTEVALID	(1<<0)
#define PTERONLY	0
#define PTEWRITE	(1<<1)
#define PTECACHED	0
#define PTEUNCACHED	(1<<2)
#define PTENOEXEC	(1<<4)

#define HOWMANY(x, y)	(((x) + (y) - 1) / (y))
#define ROUNDUP(x, y)	(HOWMANY((x), (y)) * (y))
#define ROUNDDN(x, y)	(((x) / (y)) * (y))
#define MIN(a, b)	((a) < (b)? (a): (b))
#define MAX(a, b)	((a) > (b)? (a): (b))
#define ALIGNED(p, a)	(!(((usize)(p)) & ((a) - 1)))
#define BIGPGROUND(x)	ROUNDUP((x), BIGPGSZ)

/*
 * QEMU virt device map.
 */
#define UARTBASE	0x09000000
#define UARTIRQ		1
#define GICD		((u32*)0x08000000)
#define GICC		((u32*)0x08010000)

#endif
