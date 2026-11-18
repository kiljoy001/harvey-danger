/* QEMU virt GICv2 and ARM generic virtual timer bring-up. */
#include "fdt.h"
#ifdef ARM64_FULL_KERNEL
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "ureg.h"
#endif

extern u64 splhi(void);
extern void splx(u64);

extern char arm64vectors[];

static volatile u32 *giccpu;
static volatile u32 *gicdist;
static u32 timerirq;
static u32 period;
volatile u32 arm64ticks;
volatile u32 arm64el0irqs;
static volatile u32 lastirq;
#ifdef ARM64_FULL_KERNEL
static int clockready;

void
arm64clockready(void)
{
	clockready = 1;
}
#endif

static int isgicv3;

enum { TlbSGI = 0 };	/* software-generated interrupt used for TLB shootdown */

#ifdef ARM64_FULL_KERNEL
/*
 * Device interrupts.  A driver opens a wait file for its device; the
 * handler then counts the line and wakes the reader.  Nothing here is
 * touched until a driver asks for an interrupt, so a board with no IRQ
 * consumers behaves exactly as before.
 */
enum { Nirqwait = 992, Nirqwaiter = 4 };
typedef struct Irqwaiter Irqwaiter;
struct Irqwaiter {
	Rendez r;
	u32 *countp;
	u32 seen;
	int used;
};
typedef struct Irqwait Irqwait;
struct Irqwait {
	u32 count;
	int nused;
	Irqwaiter w[Nirqwaiter];
};
static Irqwait irqwait[Nirqwait];

static int
irqready(void *a)
{
	Irqwaiter *iw;

	iw = a;
	return *iw->countp != iw->seen;
}

static Irqwait *
irqslot(u32 irq)
{
	if(irq < 32 || irq >= 32 + Nirqwait)
		return nil;
	return &irqwait[irq - 32];
}

/*
 * Returns a waiter id >= 0, or -1.  A line may have several waiters
 * (two LKL guests can both watch the PMIC arbiter line); each waiter
 * sleeps on its own Rendez -- this kernel panics on two procs asleep
 * on one Rendez ("double sleep") -- and keeps its own seen counter.
 * The GIC line itself is programmed once, when the first waiter
 * arrives, and masked when the last one leaves.
 */
int
arm64irqenable(u32 irq)
{
	Irqwait *w;
	u32 mask, prio, shift;
	int i;

	if(gicdist == nil || (w = irqslot(irq)) == nil)
		return -1;
	for(i = 0; i < Nirqwaiter; i++)
		if(!w->w[i].used)
			break;
	if(i >= Nirqwaiter)
		return -1;
	w->w[i].used = 1;
	w->w[i].countp = &w->count;
	w->w[i].seen = w->count;
	if(w->nused++ > 0)
		return i;
	mask = 1U << (irq % 32);
	prio = 0x400/4 + irq/4;
	shift = (irq % 4) * 8;
	gicdist[prio] = (gicdist[prio] & ~(0xffU << shift)) | (0xa0U << shift);
	if(irq > 15)
		if(isgicv3){
			*(volatile u32 *)((u8 *)gicdist + 0x6000 + irq*8) = 0;
			*(volatile u32 *)((u8 *)gicdist + 0x6000 + irq*8 + 4) = 0;
			gicdist[0x080/4 + irq/32] |= mask;
		}else{
			u32 t = 0x800/4 + (irq & ~3)/4;
			u32 s = (irq & 3) * 8;
			gicdist[t] = (gicdist[t] & ~(0xffU << s)) | (0x01U << s);
			gicdist[0x080/4] &= ~mask;
		}
	gicdist[0x100/4 + irq/32] = mask;
	__asm__ volatile("dsb sy; isb" ::: "memory");
	return i;
}

void
arm64irqdisable(u32 irq, int id)
{
	Irqwait *w;

	if(gicdist == nil || (w = irqslot(irq)) == nil)
		return;
	if(id < 0 || id >= Nirqwaiter || !w->w[id].used)
		return;
	w->w[id].used = 0;
	if(--w->nused > 0)
		return;
	gicdist[0x180/4 + irq/32] = 1U << (irq % 32);
	__asm__ volatile("dsb sy" ::: "memory");
}

int
arm64irqwait(u32 irq, int id)
{
	Irqwait *w;
	Irqwaiter *iw;

	if((w = irqslot(irq)) == nil)
		return -1;
	if(id < 0 || id >= Nirqwaiter || !w->w[id].used)
		return -1;
	iw = &w->w[id];
	if(w->count == iw->seen && gicdist != nil && irq >= 32){
		gicdist[0x100/4 + irq/32] = 1U << (irq % 32);
		__asm__ volatile("dsb sy" ::: "memory");
	}
	while(w->count == iw->seen)
		sleep(&iw->r, irqready, iw);
	iw->seen = w->count;
	return (int)w->count;
}

static void
irqnote(u32 irq)
{
	Irqwait *w;

	if(gicdist != nil && irq >= 32){
		gicdist[0x180/4 + irq/32] = 1U << (irq % 32);
		__asm__ volatile("dsb sy" ::: "memory");
	}
	if((w = irqslot(irq)) == nil || w->nused == 0)
		return;
	w->count++;
	/* wake every waiter; each re-checks its own seen counter */
	{
		int i;

		for(i = 0; i < Nirqwaiter; i++)
			if(w->w[i].used)
				wakeup(&w->w[i].r);
	}
}
#else
#define irqnote(irq)
#endif

int
arm64timerinit(Fdtinfo *info)
{
	volatile u32 *dist;
	u64 freq;
	u32 mask;

	fb_stripe(8, 0x00800080);	/* Slot 8: timerinit entered (Purple) */
	if(info->gicdist == 0 || info->giccpu == 0 ||
	   info->timerirq < 16 || info->timerirq >= 32)
		return -1;
	dist = (volatile u32 *)info->gicdist;
	gicdist = dist;
	giccpu = (volatile u32 *)info->giccpu;
	timerirq = info->timerirq;
	isgicv3 = info->gicversion == 3;
	mask = 1U << timerirq;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	if(freq < 100 || freq/100 > 0x7fffffff)
		return -1;
	period = freq/100;
	arm64ticks = 0;
	arm64el0irqs = 0;

	__asm__ volatile("msr vbar_el1, %0; isb" :: "r"(arm64vectors) : "memory");
	if(isgicv3){
		dist[0x000/4] = 0x37;
		fb_stripe(9, 0x00008080);	/* Slot 9: GICD_CTLR written (Dark Cyan) */
	}else{
		dist[0x000/4] = 0;
		dist[0x180/4] = 0xffffffff;
		dist[0x280/4] = 0xffffffff;
		/* QEMU's default GICv2 accepts this PPI through Group 0. */
		dist[0x080/4] &= ~mask;
		dist[0x000/4] = 1;
	}
	arm64cpuirqinit(0);
	__asm__ volatile("dsb sy; isb" ::: "memory");
	__asm__ volatile("msr daifclr, #2" ::: "memory");
	return 0;
}

/*
 * Per-CPU interrupt setup.  GICv2's CPU interface and SGI/PPI banks are
 * selected by the accessing core, so every core uses the same address.
 * GICv3 uses a per-CPU redistributor plus the ICC_* system registers.
 * Each CPU arms its own virtual timer.
 */
void
arm64cpuirqinit(int cpu)
{
	volatile u32 *gic, *rd, *sgi;
	u32 mask, reg, shift;

	mask = 1U << timerirq;
	if(isgicv3){
		u64 mpidr;

		fb_stripe(10, 0x00808000);	/* Slot 10: cpuirqinit start */
		__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
		{
			u32 core_idx = ((mpidr >> 8) & 1) * 4 + (mpidr & 3);
			rd = giccpu + core_idx * (0x20000 / 4);
			if((mpidr & 0xfff) == 0)
				fb_stripe(11, 0x00ffffff);	/* White: MPIDR 0x000 */
			else if((mpidr & 0xfff) == 0x100)
				fb_stripe(11, 0x000000ff);	/* Blue: MPIDR 0x100 */
			else
				fb_stripe(11, 0x0000ff00);	/* Green: Other MPIDR */
		}
		sgi = rd + 0x10000/4;
		rd[0x014/4] &= ~(1U<<1);
		for(int to = 0; to < 1000000; to++){
			if((rd[0x014/4] & (1U<<2)) == 0)
				break;
		}
		fb_stripe(12, 0x0000ff80);	/* Slot 12: WAKER clear */
		sgi[0x180/4] = 0xffffffff;
		sgi[0x280/4] = 0xffffffff;
		sgi[0x080/4] |= mask;
		sgi[0xd00/4] &= ~mask;
		reg = 0x400/4 + timerirq/4;
		shift = (timerirq%4)*8;
		sgi[reg] = (sgi[reg] & ~(0xffU<<shift)) | (0x80U<<shift);
		sgi[0x100/4] = mask | (1U<<TlbSGI);
		sgi[0x080/4] |= 1U<<TlbSGI;	/* group 1 */
		sgi[0x400/4] &= ~0xffU;		/* SGI0 highest priority */
		fb_stripe(13, 0x0000ffff);	/* Slot 13: sgi configured */
		__asm__ volatile("msr S3_0_C12_C12_5, %0; isb" :: "r"((u64)7) : "memory");
		__asm__ volatile("msr S3_0_C4_C6_0, %0; isb" :: "r"((u64)0xff) : "memory");
		__asm__ volatile("msr S3_0_C12_C12_7, %0; isb" :: "r"((u64)1) : "memory");
		fb_stripe(14, 0x00ff00ff);	/* Slot 14: ICC programmed */
	}else{
		/* GICv2 CPU interfaces are banked: every core uses the same
		 * address and the GIC selects its bank. */
		gic = giccpu;
		gic[0x004/4] = 0xff;
		gic[0x000/4] = 1;
		/* The IGROUPR0 bank is per-CPU too: put this core's timer PPI
		 * in group 0, which GICC_CTLR above enables. */
		gicdist[0x080/4] &= ~(mask | (1U<<TlbSGI));
		gicdist[0x100/4] = mask | (1U<<TlbSGI);
		gicdist[0x400/4] &= ~0xffU;
		reg = 0x400/4 + timerirq/4;
		shift = (timerirq%4)*8;
		gicdist[reg] = (gicdist[reg] & ~(0xffU<<shift)) | (0x80U<<shift);
	}
	__asm__ volatile("dsb sy; isb" ::: "memory");
	__asm__ volatile("msr cntv_tval_el0, %0; msr cntv_ctl_el0, %1; isb"
	                 :: "r"((u64)period), "r"((u64)1) : "memory");
}

/*
 * Broadcast SGI 0 to every other CPU.  GICv2 uses the target-list-other
 * filter; GICv3 uses IRM.  The handler flushes the local TLB and, when the
 * interrupted context is user mode, releases the page tables now; inside a
 * syscall it defers to the user-return boundary.
 */
static volatile u32 ipiseen[MACHMAX];

void
arm64ipi(void)
{
	if(isgicv3)
		__asm__ volatile("msr S3_0_C12_C11_5, %0; isb"
		    :: "r"((1ULL<<40) | (TlbSGI<<24)) : "memory");
	else{
		gicdist[0xF00/4] = (1U<<24) | TlbSGI;	/* all but self */
		__asm__ volatile("dsb sy" ::: "memory");
	}
}

static void
arm64tlbirq(Ureg *u)
{
	/*
	 * Only invalidate translations here.  Releasing page tables from an
	 * arbitrary interrupt is unsafe: the interrupted context may be in the
	 * middle of a kernel stack frame, and the release takes the physical
	 * allocator and kernel heap locks.  The Proc's newtlb is consumed at
	 * the next user-mode clock tick, context switch, or user return, where
	 * the address space is not in flight.
	 */
	USED(u);
	ipiseen[machp()->machno]++;
	mmuflushtlb();
	machp()->mmuflush = 0;
	coherence();
}

/*
 * Boot probe: broadcast a TLB SGI and check that every online CPU took it.
 * This validates the GIC SGI setup independently of segment churn.
 */
int
arm64ipiprobe(void)
{
	u32 seen[MACHMAX];
	int i, n, ok;

	for(i = 0; i < sys->nmach; i++)
		seen[i] = ipiseen[i];
	arm64ipi();
	for(n = 0; n < 2000; n++){
		ok = 1;
		for(i = 0; i < sys->nmach; i++)
			if(i != machp()->machno && ipiseen[i] == seen[i])
				ok = 0;
		if(ok)
			return 0;
		microdelay(1000);
	}
	print("ARM64 ipi fail: gicv3 %d gicdist %#p giccpu %#p ctlr %#x igroupr %#x isen %#x lastirq %ud\n",
	    isgicv3, gicdist, giccpu, gicdist[0x000/4], gicdist[0x080/4],
	    gicdist[0x100/4], lastirq);
	for(i = 0; i < sys->nmach; i++)
		if(i != machp()->machno)
			print("ARM64 ipi cpu%d seen %ud (was %ud)\n",
			    i, ipiseen[i], seen[i]);
	return -1;
}

#ifdef ARM64_FULL_KERNEL
/*
 * Return-to-user scheduling for interrupts.  hzsched() already marks the
 * interrupted process when another one should run, but until now only the
 * syscall path acted on it.  A guest that spins in user mode without
 * issuing a syscall (LKL's idle loop) therefore never yielded and starved
 * every other process, including the timer thread that has to wake it.
 */
static void
irqreturn(Ureg *ureg, int clock)
{
	Proc *up = externup();

	if(ureg == nil || (ureg->psr & 15) != 0)
		return;
	if(up == nil || up->state != Running)
		return;
	if(clock){
		if(up->delaysched)
			sched();
	}else
		preempted();
}
#endif

void
arm64irq(
#ifdef ARM64_FULL_KERNEL
	Ureg *ureg
#else
	void *ureg
#endif
)
{
	u32 irq;
	u64 spsr, iar;
	volatile u32 *gic;

	gic = giccpu;

#ifndef ARM64_FULL_KERNEL
	(void)ureg;
#endif

	__asm__ volatile("mrs %0, spsr_el1" : "=r"(spsr));
	if(isgicv3){
		__asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(iar));
		irq = iar & 0x3ff;
	}else{
		irq = gic[0x00c/4] & 0x3ff;
	}
	lastirq = irq;
	if(irq < 16){
		if(irq == TlbSGI)
			arm64tlbirq(ureg);
		if(isgicv3)
			__asm__ volatile("msr S3_0_C12_C12_1, %0; isb" :: "r"((u64)irq) : "memory");
		else
			gic[0x010/4] = irq;
		return;
	}
	if(irq == timerirq){
		arm64ticks++;
		if((spsr&15) == 0)
			arm64el0irqs++;
		__asm__ volatile("msr cntv_tval_el0, %0; isb" :: "r"((u64)period) : "memory");
		if(isgicv3)
			__asm__ volatile("msr S3_0_C12_C12_1, %0; isb" :: "r"((u64)irq) : "memory");
		else
			gic[0x010/4] = irq;
#ifdef ARM64_FULL_KERNEL
		if(clockready)
			timerintr(ureg, 0);
		irqreturn(ureg, 1);
#endif
	}else{
		irqnote(irq);
		/* Complete the interrupt so the line can fire again. */
		if(isgicv3)
			__asm__ volatile("msr S3_0_C12_C12_1, %0; isb" :: "r"((u64)irq) : "memory");
		else
			gic[0x010/4] = irq;
#ifdef ARM64_FULL_KERNEL
		irqreturn(ureg, 0);
#endif
	}
}

void
arm64timerstop(void)
{
	__asm__ volatile("msr daifset, #2; msr cntv_ctl_el0, %0; isb"
	                 :: "r"((u64)0) : "memory");
}

int
arm64timerwait(void)
{
	u64 now, deadline;

	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
	deadline = now + (u64)period*20;
	while(arm64ticks < 3){
		__asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
		if(now >= deadline)
			return -1;
	}
	return 0;
}

u64
arm64timerdiag(void)
{
	u64 ctl;

	__asm__ volatile("mrs %0, cntv_ctl_el0" : "=r"(ctl));
	return (ctl&7) | ((u64)(gicdist[0x200/4]&0xffffffff)<<8) |
	       ((u64)(lastirq&0x3ff)<<40) |
	       ((u64)(giccpu[0x000/4]&3)<<52) |
	       ((u64)(gicdist[0x000/4]&3)<<54);
}

int
arm64irqmaskprobe(void)
{
	u64 old, now, start, freq, daif;
	u32 ticks;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	old = splhi();
	__asm__ volatile("mrs %0, daif" : "=r"(daif));
	if((old & (1ULL<<7)) != 0 || (daif & (1ULL<<7)) == 0){
		splx(old);
		return -1;
	}
	ticks = arm64ticks;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
	do {
		__asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
	} while(now-start < freq/50);
	if(arm64ticks != ticks){
		splx(old);
		return -1;
	}
	splx(old);
	__asm__ volatile("mrs %0, daif" : "=r"(daif));
	if(daif & (1ULL<<7))
		return -1;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
	do {
		__asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
		if(arm64ticks > ticks)
			return 0;
	} while(now-start < freq/10);
	return -1;
}
