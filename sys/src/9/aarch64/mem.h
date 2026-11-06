/*
 * This file is part of the Harvey operating system. It is subject to the
 * GNU GPL v2 in the top-level LICENSE.gpl file.
 */
#ifndef HARVEY_AARCH64_MEM_H
#define HARVEY_AARCH64_MEM_H

#define KiB 1024ULL
#define MiB (1024ULL * KiB)
#define GiB (1024ULL * MiB)
#define PGSZ (4 * KiB)
#define BIGPGSZ (2 * MiB)
#define PGSHFT 12
#define KSTACK (32 * KiB)
#define BY2SE 8
#define HZ 100
#define MACHMAX 16
#define NPGSZ 2
#define STACKALIGN(sp) ((sp) & ~15ULL)

#define HOWMANY(x, y) (((x) + (y) - 1) / (y))
#define ROUNDUP(x, y) (HOWMANY((x), (y)) * (y))
#define ROUNDDN(x, y) (((x) / (y)) * (y))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define BLOCKALIGN 8
#define ALIGNED(p, a) (!(((usize)(p)) & ((a) - 1)))
#define PGROUND(x) ROUNDUP((x), PGSZ)
#define BIGPGROUND(x) ROUNDUP((x), BIGPGSZ)

/*
 * The shared process code uses these boundaries for user segments.
 *
 * User text must start above every identity mapping the kernel installs.
 * This port runs the kernel and the user process in one TTBR0 address
 * space, with RAM and device registers mapped 1:1, and Maxidentity in
 * mmu.c rejects anything at or above 16 GiB.  A user segment that lands
 * in that window replaces the kernel's mapping of those physical pages.
 * Clang is ~120 MB, so at the old 2 MiB base its data segment reached
 * 0x08000000 and unmapped the GIC: every timer tick then read a spurious
 * interrupt ID and re-entered the vector forever.  Keep user text at or
 * above 16 GiB until the kernel moves to TTBR1.
 */
#define UTZERO (16ULL * 1024 * 1024 * 1024)
#define UTROUND(x) BIGPGROUND(x)
#define USTKTOP (0x0000007fffe00000ULL)
#define USTKSIZE (16 * MiB)
#define TSTKTOP (USTKTOP - USTKSIZE)
#define NIXCALL (TSTKTOP - USTKSIZE)
#define BIGBSSTOP ROUNDDN((NIXCALL - BIGPGSZ), GiB)
#define BIGBSSSIZE (32 * GiB)
#define HEAPTOP (BIGBSSTOP - BIGBSSSIZE)

/* TODO: migrate the identity mapped bootstrap kernel to TTBR1. */
#define KZERO 0xffffff8000000000ULL
#define KTZERO 0x40080000ULL

/*
 * The bootstrap kernel runs on an identity map, so kernel pointers are
 * low addresses rather than high-half ones.  User programs are linked
 * below 0x40000000; the kernel image, heap and device mappings are above.
 */
#define KPTR(p) ((usize)(p) >= 0x40000000ULL)

#endif
