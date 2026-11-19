/*
 * This file is part of the Harvey operating system. It is subject to the
 * GNU GPL v2 in the top-level LICENSE.gpl file.
 */
#ifndef HARVEY_ARM_DAT_H
#define HARVEY_ARM_DAT_H

#include "context.h"

typedef struct Conf Conf;
typedef struct Confmem Confmem;
typedef struct ISAConf ISAConf;
typedef struct Mach Mach;
typedef struct Page Page;
typedef void KMap;
typedef struct PFPU PFPU;
typedef struct PMMU PMMU;
typedef struct Ptpage Ptpage;
typedef struct PNOTIFY PNOTIFY;
typedef struct Proc Proc;
typedef struct Sys Sys;
typedef struct Active Active;
typedef struct Ureg Ureg;
typedef u32 Mpl;
typedef u32 PTE;
typedef u32 physaddr;

#define MAXSYSARG 5
#define NCOLOR 1
#define AOUT_MAGIC R_MAGIC
#define DBG_MAX_REG_NUM 16
#define GDB_NUMREGBYTES (16 * 4)

typedef struct Lock Lock;
struct Lock {
	u32 key;
	int isilock;
	Mpl pl;
	usize _pc;
	Proc *p;
	Mach *m;
	u64 lockcycles;
};

/* VFP d0-d31 and the status register; unused while the kernel is soft-float. */
struct PFPU {
	u64 d[32];
	u32 fpscr;
};

struct PMMU {
	u32 mmul1pa;
	Ptpage *ptpages;
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
	u32 npage;
	usize upages;
	u32 copymode;
	u32 ialloc;
	u32 nimage;
};

#define NISAOPT 8
struct ISAConf {
	char *type;
	usize port;
	int irq;
	u32 dma;
	usize mem;
	usize size;
	u32 freq;
	int nopt;
	char *opt[NISAOPT];
};

#include "../port/portdat.h"

struct Mach {
	int machno;
	int nixtype;
	int online;
	usize splpc;
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
	int intr;
	int syscall;
	int pfault;
	int tlbfault;
	int tlbpurge;
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
	int load;
	u64 ticks;
	u64 cyclefreq;
	u32 pgsz[NPGSZ];
	int npgsz;
};

struct Active {
	int exiting;
};

extern Sys *sys;
extern Active active;
#define MACHP(n) (sys->machptr[(n)])

#define DBGFLG 0
#define DBG(...) do { } while(0)
#define VA(k) ((usize)(k))
#define kunmap(k) ((void)0)
extern char *rolename[];

#endif
