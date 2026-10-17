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
#define NPGSZ 1
#define STACKALIGN(sp) ((sp) & ~15ULL)

#endif
