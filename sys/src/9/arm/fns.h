/*
 * This file is part of the Harvey operating system. It is subject to the
 * GNU GPL v2 in the top-level LICENSE.gpl file.
 */
#ifndef HARVEY_ARM_FNS_H
#define HARVEY_ARM_FNS_H

#include "../port/portfns.h"

#define PTR2UINT(p)	((usize)(p))
#define UINT2PTR(i)	((void*)(usize)(i))
#define KADDR(pa)	((void*)(usize)(pa))
#define PADDR(p)	((usize)(p))
#define evenaddr(x)	((void)0)
#define kmapinval()	((void)0)

KMap *kmap(Page *);
Mach *getac(Proc *, int);
int aoutldseg(void *, usize *, Ldseg **, char *, u32);
usize sysexecstack(usize, int);
void sysprocsetup(Proc *);
void *sysexecregs(usize, u32, void *);
void dumpgpr(Ureg *);
int CASW(u32 *, u32, u32);
int userureg(Ureg *);
int fpudevprocio(Proc *, void *, i32, usize, int);
u32 rdtsc(void);
int backtrace_list(usize, usize, usize *, usize);
void archfmtinstall(void);
void hi(char *);
void die(char *);

#define setlabel(label)	slim_setlabel(label)
#define waserror()	setlabel(&up->errlab[up->nerrlab++])
#define poperror()	up->nerrlab--

/* The shared lock implementation expects TAS to return the old value. */
#define TAS(p)		__atomic_exchange_n((p), 1U, __ATOMIC_ACQUIRE)
static inline void
mfence(void)
{
	__asm__ volatile("dmb ish" ::: "memory");
}

/*
 * port/proc.c calls the AArch64 IPI hook by name; keep the shared source
 * untouched and let a single define map it onto this port's version.
 */
#define arm64ipi	arm32ipi

Mach *machp(void);
extern Mach arm32machs[];
void arm32setmach(Mach *);
void arm32cpuirqinit(int);
void arm32ipi(void);
int arm32ipiprobe(void);
static inline Proc *
externup(void)
{
	return machp()->externup;
}

Mpl splhi(void);
Mpl spllo(void);
void splx(Mpl);
void procsave(Proc *);
void procrestore(Proc *);
void mmuflushtlb(void);
void mmuswitch(Proc *);
void mmurelease(Proc *);
void mmuprocinit(Proc *);
void idlehands(void);
void coherence(void);
void dcacheclean(void *, usize);
void dcachecleanall(void);
int corecolor(int);
void cycles(u64 *);
void stopac(void);
void intrac(Proc *);
void DONE(void);
void runacore(void);
void hardhalt(void);
int infected_with_std(void);
void disinfect_std(void);
void ureg2gdb(Ureg *, usize *);
void dumpmmuwalk(void);
u32 cr3get(void);
void stacksnippet(void);
int incref(Ref *);
int decref(Ref *);

#endif
