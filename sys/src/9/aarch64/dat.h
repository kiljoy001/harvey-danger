/*
 * This file is part of the Harvey operating system. It is subject to the
 * GNU GPL v2 in the top-level LICENSE.gpl file.
 */
#ifndef HARVEY_AARCH64_DAT_H
#define HARVEY_AARCH64_DAT_H

#include "context.h"

typedef struct Conf Conf;
typedef struct Confmem Confmem;
typedef struct Lock Lock;
typedef struct Mach Mach;
typedef struct Page Page;
typedef struct PFPU PFPU;
typedef struct PMMU PMMU;
typedef struct PNOTIFY PNOTIFY;
typedef struct Proc Proc;
typedef struct Sys Sys;
typedef struct Active Active;
typedef struct Ureg Ureg;
typedef u64 Mpl;
typedef u64 PTE;
typedef u64 physaddr;

#define MAXSYSARG 5
#define NCOLOR 1

struct Lock {
	u32 key;
	int isilock;
	Mpl pl;
	usize _pc;
	Proc *p;
	Mach *m;
	u64 lockcycles;
};

struct PFPU {
	u64 reserved;
};

struct PMMU {
	u64 rootpa;
};

struct PNOTIFY {
	char emptiness;
};

struct Confmem {
	usize base;
	usize npage;
	usize kbase;
	usize klimit;
};

struct Conf {
	u32 nproc;
	Confmem mem[4];
	u64 npage;
	usize upages;
	u32 copymode;
	u32 ialloc;
	u32 nimage;
};

#include "../port/portdat.h"

struct Mach {
	int machno;
	int nixtype;
	Proc *proc;
	Proc *externup;
	u64 ticks;
	Label sched;
	Lock alarmlock;
	void *alarm;
	int inclockintr;
	Proc *readied;
	u64 schedticks;
	u64 qstart;
	int qexpired;
	int cs;
	int mmuflush;
	int ilockdepth;
	int inidle;
	Perf perf;
	int load;
	struct {
		int nixtype;
	} NIX;
	Sched *sch;
};

struct Sys {
	int nmach;
	Mach *machptr[MACHMAX];
};

struct Active {
	int exiting;
};

extern Sys *sys;
extern Active active;
#define MACHP(n) (sys->machptr[(n)])

#endif
