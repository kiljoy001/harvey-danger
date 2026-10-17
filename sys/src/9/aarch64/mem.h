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
#define KSTACK (16 * KiB)
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

/* The shared process code uses these boundaries for user segments. */
#define UTZERO (2 * MiB)
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

#endif
