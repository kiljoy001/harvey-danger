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

#ifdef ARM64_FULL_KERNEL
/*
 * Device interrupts.  A driver opens a wait file for its device; the
 * handler then counts the line and wakes the reader.  Nothing here is
 * touched until a driver asks for an interrupt, so a board with no IRQ
 * consumers behaves exactly as before.
 */
enum { Nirqwait = 256 };
typedef struct Irqwait Irqwait;
struct Irqwait {
	Rendez r;
	u32 count;
	u32 seen;
	int open;
};
static Irqwait irqwait[Nirqwait];

static int
irqready(void *a)
{
	Irqwait *w;

	w = a;
	return w->count != w->seen;
}

static Irqwait *
irqslot(u32 irq)
{
	if(irq < 32 || irq >= 32 + Nirqwait)
		return nil;
	return &irqwait[irq - 32];
}

int
arm64irqenable(u32 irq)
{
	Irqwait *w;
	u32 mask, prio, shift;

	if(gicdist == nil || (w = irqslot(irq)) == nil)
		return -1;
	mask = 1U << (irq % 32);
	prio = 0x400/4 + irq/4;
	shift = (irq % 4) * 8;
	gicdist[prio] = (gicdist[prio] & ~(0xffU << shift)) | (0xa0U << shift);
	if(irq > 15)
		if(isgicv3){
			*(volatile u64 *)((u8 *)gicdist + 0x6000 + irq*8) = 0;
			gicdist[0x080/4] |= mask;
		}else{
			u32 t = 0x800/4 + (irq & ~3)/4;
			u32 s = (irq & 3) * 8;
			gicdist[t] = (gicdist[t] & ~(0xffU << s)) | (0x01U << s);
			gicdist[0x080/4] &= ~mask;
		}
	gicdist[0x100/4 + irq/32] = mask;
	w->open = 1;
	__asm__ volatile("dsb sy; isb" ::: "memory");
	return 0;
}

void
arm64irqdisable(u32 irq)
{
	Irqwait *w;

	if(gicdist == nil || (w = irqslot(irq)) == nil)
		return;
	gicdist[0x180/4 + irq/32] = 1U << (irq % 32);
	w->open = 0;
	__asm__ volatile("dsb sy" ::: "memory");
}

int
arm64irqwait(u32 irq)
{
	Irqwait *w;

	if((w = irqslot(irq)) == nil || !w->open)
		return -1;
	while(w->count == w->seen)
		sleep(&w->r, irqready, w);
	w->seen = w->count;
	return (int)w->count;
}

static void
irqnote(u32 irq)
{
	Irqwait *w;

	if((w = irqslot(irq)) == nil || !w->open)
		return;
	w->count++;
	wakeup(&w->r);
}
#else
#define irqnote(irq)
#endif

int
arm64timerinit(Fdtinfo *info)
{
	volatile u32 *dist, *sgi;
	u64 freq;
	u32 mask, reg, shift;

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
		sgi = (volatile u32 *)(info->giccpu + 0x10000ULL);
		dist[0x000/4] = 0x37;
		giccpu[0x014/4] &= ~(1U<<1);
		while(giccpu[0x014/4] & (1U<<2))
			;
		sgi[0x180/4] = 0xffffffff;
		sgi[0x280/4] = 0xffffffff;
		sgi[0x080/4] |= mask;
		sgi[0xd00/4] &= ~mask;
		reg = 0x400/4 + timerirq/4;
		shift = (timerirq%4)*8;
		sgi[reg] = (sgi[reg] & ~(0xffU<<shift)) | (0x80U<<shift);
		sgi[0x100/4] = mask;
		__asm__ volatile("msr S3_0_C12_C12_5, %0; isb" :: "r"((u64)7) : "memory");
		__asm__ volatile("msr S3_0_C4_C6_0, %0; isb" :: "r"((u64)0xff) : "memory");
		__asm__ volatile("msr S3_0_C12_C12_7, %0; isb" :: "r"((u64)1) : "memory");
	}else{
		dist[0x000/4] = 0;
		dist[0x180/4] = 0xffffffff;
		dist[0x280/4] = 0xffffffff;
		/* QEMU's default GICv2 accepts this PPI through Group 0. */
		dist[0x080/4] &= ~mask;
		reg = 0x400/4 + timerirq/4;
		shift = (timerirq%4)*8;
		dist[reg] = (dist[reg] & ~(0xffU<<shift)) | (0x80U<<shift);
		dist[0x100/4] = mask;
		giccpu[0x004/4] = 0xff;
		giccpu[0x000/4] = 1;
		dist[0x000/4] = 1;
	}
	__asm__ volatile("dsb sy; isb" ::: "memory");
	__asm__ volatile("msr cntv_tval_el0, %0; msr cntv_ctl_el0, %1; isb"
	                 :: "r"((u64)period), "r"((u64)1) : "memory");
	__asm__ volatile("msr daifclr, #2" ::: "memory");
	return 0;
}

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

#ifndef ARM64_FULL_KERNEL
	(void)ureg;
#endif

	__asm__ volatile("mrs %0, spsr_el1" : "=r"(spsr));
	if(isgicv3){
		__asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(iar));
		irq = iar & 0x3ff;
	}else{
		irq = giccpu[0x00c/4] & 0x3ff;
	}
	lastirq = irq;
	if(irq == timerirq){
		arm64ticks++;
		if((spsr&15) == 0)
			arm64el0irqs++;
		__asm__ volatile("msr cntv_tval_el0, %0; isb" :: "r"((u64)period) : "memory");
		if(isgicv3)
			__asm__ volatile("msr S3_0_C12_C12_1, %0; isb" :: "r"((u64)irq) : "memory");
		else
			giccpu[0x010/4] = irq;
#ifdef ARM64_FULL_KERNEL
		if(clockready)
			timerintr(ureg, 0);
#endif
	}else{
		irqnote(irq);
		/* Complete the interrupt so the line can fire again. */
		if(isgicv3)
			__asm__ volatile("msr S3_0_C12_C12_1, %0; isb" :: "r"((u64)irq) : "memory");
		else
			giccpu[0x010/4] = irq;
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
