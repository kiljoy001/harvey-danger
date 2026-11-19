/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * ARMv7-A QEMU "virt" bring-up: console, vectors, GIC and generic timer.
 */
#include "u.h"
#include "mem.h"
#include "arm.h"

enum {
	GicdCtlr	= 0x000,
	GicdIsenabler	= 0x100,
	GicdIpriority	= 0x400,
	GicdItargets	= 0x800,
	GiccCtlr	= 0x000,
	GiccPmr		= 0x004,
	GiccIar		= 0x00c,
	GiccEoir	= 0x010,

	Timerirq	= 27,		/* virtual timer, PPI */
	Tickhz		= 100,
};

#define gicd(r)	(*(volatile u32*)((u32)GICD + (r)))
#define gicc(r)	(*(volatile u32*)((u32)GICC + (r)))

static u32 timerinterval;
static u32 ticks;

static void
putdec(u32 v)
{
	static u32 pow10[] = {
		1000000000, 100000000, 10000000, 1000000, 100000,
		10000, 1000, 100, 10, 1,
	};
	int i, d, started;

	started = 0;
	for(i = 0; i < 10; i++){
		d = 0;
		while(v >= pow10[i]){
			v -= pow10[i];
			d++;
		}
		if(d || started || i == 9){
			uartputc('0' + d);
			started = 1;
		}
	}
}

static void
puthex(u32 v)
{
	static char hex[] = "0123456789abcdef";
	int i;

	for(i = 28; i >= 0; i -= 4)
		uartputc(hex[(v >> i) & 0xf]);
}

void
printk(char *fmt, ...)
{
	__builtin_va_list ap;
	char *s;
	u32 v;

	__builtin_va_start(ap, fmt);
	for(; *fmt; fmt++){
		if(*fmt != '%'){
			uartputc(*fmt);
			continue;
		}
		switch(*++fmt){
		case 'd':
			putdec(__builtin_va_arg(ap, u32));
			break;
		case 'x':
		case 'p':
			v = __builtin_va_arg(ap, u32);
			puthex(v);
			break;
		case 's':
			s = __builtin_va_arg(ap, char*);
			putstr(s);
			break;
		case 'c':
			uartputc(__builtin_va_arg(ap, int));
			break;
		case '%':
			uartputc('%');
			break;
		default:
			uartputc('%');
			uartputc(*fmt);
			break;
		}
	}
	__builtin_va_end(ap);
}

static void
gicinit(void)
{
	gicd(GicdCtlr) = 0;
	gicc(GiccCtlr) = 0;

	/* Priority 0xa0 for the timer, and route it to cpu 0. */
	gicd(GicdIpriority + (Timerirq & ~3)) = 0xa0a0a0a0;
	gicd(GicdItargets + (Timerirq & ~3)) = 0x01010101;

	gicc(GiccPmr) = 0xf0;
	gicc(GiccCtlr) = 1;
	gicd(GicdCtlr) = 1;

	/* PPI 16..31 live in ISENABLER0. */
	gicd(GicdIsenabler) = 1 << Timerirq;
}

static void
timerstart(void)
{
	u32 freq;

	freq = arm32cntfrq();
	timerinterval = freq / Tickhz;
	arm32setcntvtval(timerinterval);
	arm32setcntvctl(1);		/* enable, unmasked */
}

void
arm32irqhandler(void)
{
	u32 id;

	id = gicc(GiccIar) & 0x3ff;
	if(id >= 1020)			/* spurious */
		return;

	if(id == Timerirq){
		arm32setcntvtval(timerinterval);
		ticks++;
		if((ticks % Tickhz) == 0){
			printk("arm32: tick %d\n", (u32)(ticks / Tickhz));
		}
	}
	gicc(GiccEoir) = id;
}

void
arm32svchandler(void)
{
	printk("arm32: unexpected svc\n");
}

void
arm32unexpected(u32 pc)
{
	printk("arm32: unexpected exception at %p\n", pc);
	hardhalt();
}

void
arm32prefetchabort(u32 pc)
{
	printk("arm32: prefetch abort at %p\n", pc);
	hardhalt();
}

void
arm32dataabort(u32 pc)
{
	printk("arm32: data abort at %p far %p fsr %p\n",
		pc, arm32dfar(), arm32dfsr());
	hardhalt();
}

void
arm32main(u32 dtb)
{
	uartinit();
	printk("\nHARVEY-ARM32 VIRT\n");
	printk("arm32: cpu id %p dtb %p (unused)\n", arm32id(), dtb);
	printk("arm32: kernel %p..%p\n", (u32)0x40010000, (u32)&arm32l1);

	arm32setvbar((u32)arm32vectors);
	printk("arm32: vectors %p\n", (u32)arm32vectors);

	arm32mmuinit();
	printk("arm32: mmu on\n");

	gicinit();
	timerstart();
	printk("arm32: timer %d Hz, tick %d Hz\n", arm32cntfrq(), Tickhz);

	__asm__ volatile("cpsie i" ::: "memory");
	printk("arm32: interrupts on\n");

	for(;;)
		__asm__ volatile("wfi");
}
