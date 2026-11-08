/*
 * Secondary CPU bring-up.
 *
 * PSCI starts a core at arm64cpuentry() with the physical address of a
 * CpuBoot record in x0.  The entry stub (smp.S) installs the stack and the
 * boot CPU's translation regime, then calls arm64cpustart(), which sets up
 * that core's GIC interface and timer and enters the shared scheduler.
 */

#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fdt.h"
#include "mmu.h"
#include "fns.h"

typedef struct CpuBoot CpuBoot;
struct CpuBoot {
	u64 sp;		/* 0 */
	u64 mair;	/* 8 */
	u64 tcr;	/* 16 */
	u64 sctlr;	/* 24 */
	u64 vbar;	/* 32 */
	u64 mach;	/* 40 */
	u64 root;	/* 48 */
};

extern char arm64vectors[];
void setmachsched(Mach *);
extern u64 arm64kernelmair, arm64kerneltcr, arm64kernelsctlr;
void arm64cpuentry(void);
void arm64cpustart(CpuBoot *);

static CpuBoot cpuboot[MACHMAX];
static char cpustack[MACHMAX][KSTACK] __attribute__((aligned(16)));

static u64
psci(u32 method, u64 fn, u64 a0, u64 a1, u64 a2)
{
	register u64 x0 __asm__("x0") = fn;
	register u64 x1 __asm__("x1") = a0;
	register u64 x2 __asm__("x2") = a1;
	register u64 x3 __asm__("x3") = a2;

	if(method == 2)
		__asm__ volatile("smc #0" : "+r"(x0)
		    : "r"(x1), "r"(x2), "r"(x3)
		    : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
		      "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	else
		__asm__ volatile("hvc #0" : "+r"(x0)
		    : "r"(x1), "r"(x2), "r"(x3)
		    : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
		      "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

void
arm64smpinit(Fdtinfo *info)
{
	u64 r;
	int i, ncpu;
	Mach *m;
	CpuBoot *b;

	ncpu = info->ncpu;
	if(ncpu < 1)
		ncpu = 1;
	if(ncpu > MACHMAX)
		ncpu = MACHMAX;
	if(ncpu > 1 && (info->psci_method == 0 ||
	   (info->psci_cpuon == 0 && info->psci_std == 0))){
		print("ARM64 smp: no PSCI cpu_on, staying uniprocessor\n");
		ncpu = 1;
	}
	sys->nmach = ncpu;
	print("ARM64 smp: ncpu %d psci %d/%#x\n", info->ncpu,
	    info->psci_method, info->psci_cpuon);
	for(i = 0; i < ncpu; i++){
		m = &arm64machs[i];
		m->machno = i;
		m->online = i == 0;
		sys->machptr[i] = m;
		setmachsched(m);
	}
	if(ncpu < 2)
		return;
	for(i = 1; i < ncpu; i++){
		b = &cpuboot[i];
		b->sp = STACKALIGN((u64)cpustack[i] + KSTACK);
		b->mair = arm64kernelmair;
		b->tcr = arm64kerneltcr;
		b->sctlr = arm64kernelsctlr;
		b->vbar = (u64)arm64vectors;
		b->mach = (u64)&arm64machs[i];
		b->root = arm64mmukernelroot();
		r = psci(info->psci_method,
		    info->psci_cpuon ? info->psci_cpuon :
		    (info->psci_std ? 0xC4000003 : 0x84000003),
		    info->cpumpidr[i] ? info->cpumpidr[i] : (u64)i,
		    (u64)arm64cpuentry, (u64)b);
		if(r != 0)
			print("ARM64 cpu%d start failed: %#llx\n", i, r);
	}
}

void
arm64cpustart(CpuBoot *b)
{
	Mach *m;

	m = (Mach *)b->mach;
	m->online = 1;
	arm64cpuirqinit(m->machno);
	timersinit();
	print("ARM64 cpu%d online\n", m->machno);
	__asm__ volatile("dsb sy; isb; msr daifclr, #2" ::: "memory");
	schedinit();
	for(;;)
		;
}
