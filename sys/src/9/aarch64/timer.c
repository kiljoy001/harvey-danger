/* QEMU virt GICv2 and ARM generic virtual timer bring-up. */
#include "fdt.h"

extern char arm64vectors[];

static volatile u32 *giccpu;
static volatile u32 *gicdist;
static u32 timerirq;
static u32 period;
volatile u32 arm64ticks;
static volatile u32 lastirq;

int
arm64timerinit(Fdtinfo *info)
{
	volatile u32 *dist;
	u64 freq;
	u32 mask, reg, shift;

	if(info->gicdist == 0 || info->giccpu == 0 ||
	   info->timerirq < 16 || info->timerirq >= 32)
		return -1;
	dist = (volatile u32 *)info->gicdist;
	gicdist = dist;
	giccpu = (volatile u32 *)info->giccpu;
	timerirq = info->timerirq;
	mask = 1U << timerirq;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	if(freq < 100 || freq/100 > 0x7fffffff)
		return -1;
	period = freq/100;
	arm64ticks = 0;

	__asm__ volatile("msr vbar_el1, %0; isb" :: "r"(arm64vectors) : "memory");
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
	__asm__ volatile("dsb sy; isb" ::: "memory");
	__asm__ volatile("msr cntv_tval_el0, %0; msr cntv_ctl_el0, %1; isb"
	                 :: "r"((u64)period), "r"((u64)1) : "memory");
	__asm__ volatile("msr daifclr, #2" ::: "memory");
	return 0;
}

void
arm64irq(void)
{
	u32 irq;

	irq = giccpu[0x00c/4] & 0x3ff;
	lastirq = irq;
	if(irq == timerirq){
		arm64ticks++;
		__asm__ volatile("msr cntv_tval_el0, %0; isb" :: "r"((u64)period) : "memory");
		giccpu[0x010/4] = irq;
	}else{
		giccpu[0x000/4] = 0;
		gicdist[0x000/4] = 0;
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
