/*
 * This file is part of the Harvey operating system. It is subject to the
 * GNU GPL v2 in the top-level LICENSE.gpl file.
 */
#ifndef HARVEY_AARCH64_FNS_H
#define HARVEY_AARCH64_FNS_H

#include "../port/portfns.h"

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
void mmuprocinit(Proc *);
void idlehands(void);
void coherence(void);
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
