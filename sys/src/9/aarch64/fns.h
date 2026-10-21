/*
 * This file is part of the Harvey operating system. It is subject to the
 * GNU GPL v2 in the top-level LICENSE.gpl file.
 */
#ifndef HARVEY_AARCH64_FNS_H
#define HARVEY_AARCH64_FNS_H

#include "../port/portfns.h"

#define PTR2UINT(p) ((usize)(p))
#define UINT2PTR(i) ((void *)(usize)(i))
#define KADDR(pa) ((void *)(usize)(pa))
#define evenaddr(x) ((void)0)
#define kmapinval() ((void)0)
u64 PADDR(void *);

KMap *kmap(Page *);
Mach *getac(Proc *, int);
int aoutldseg(void *, usize *, Ldseg **, char *, u32);
usize sysexecstack(usize, int);
void sysprocsetup(Proc *);
void *sysexecregs(usize, u32, void *);
void dumpgpr(Ureg *);
int CASW(u32 *, u32, u32);
int userureg(Ureg *);
void ureg2gdb(Ureg *, usize *);
int fpudevprocio(Proc *, void *, i32, usize, int);
u64 rdtsc(void);
int backtrace_list(usize, usize, usize *, usize);
void archfmtinstall(void);
void hi(char *);
void die(char *);

#define setlabel(label) slim_setlabel(label)
#define waserror() setlabel(&up->errlab[up->nerrlab++])
#define poperror() up->nerrlab--

/* The shared lock implementation expects TAS to return the old value. */
#define TAS(p) __atomic_exchange_n((p), 1U, __ATOMIC_ACQUIRE)
static inline void
mfence(void)
{
	__asm__ volatile("dmb ish" ::: "memory");
}

Mach *machp(void);
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
void arm64mmurelease(Proc *);
void mmuprocinit(Proc *);
void idlehands(void);
void coherence(void);
void dcacheclean(void *, usize);
int corecolor(int);
void cycles(u64 *);
int infected_with_std(void);
void disinfect_std(void);
void stopac(void);
void intrac(Proc *);
void DONE(void);
void runacore(void);
void hardhalt(void);
void stacksnippet(void);
int incref(Ref *);
int decref(Ref *);

#endif
