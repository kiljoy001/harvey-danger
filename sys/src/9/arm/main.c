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
#include "context.h"

/* The kernel libc's formatting routine, used here to prove the shared
 * C runtime (fmt, locks, varargs) works on ARMv7. */
int snprint(char *, int, const char *, ...);

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
static volatile u32 ticks;

/*
 * Context switch self-test: two coroutines trade control through the
 * primitive the shared scheduler will use.
 */
static Label bootlabel, alabel, blabel;
static u8 astack[8192] __attribute__((aligned(8)));
static u8 bstack[8192] __attribute__((aligned(8)));
static volatile int phase;

static void
taska(void)
{
	phase = 1;
	if(slim_setlabel(&alabel) == 0)
		gotolabel(&blabel);
	if(phase != 2)
		printk("arm32: context switch FAILED\n");
	else
		printk("arm32: context switch ok\n");
	phase = 3;
	gotolabel(&bootlabel);
}

static void
taskb(void)
{
	if(phase != 1)
		printk("arm32: context order FAILED\n");
	phase = 2;
	gotolabel(&alabel);
}

static void
contextprobe(void)
{
	phase = 0;
	labelstart(&alabel, taska, astack + sizeof(astack));
	labelstart(&blabel, taskb, bstack + sizeof(bstack));
	if(slim_setlabel(&bootlabel) == 0)
		gotolabel(&alabel);
	if(phase != 3)
		printk("arm32: context probe did not return\n");
}

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

/*
 * Verify that interrupts are really masked by splhi.  The generic counter
 * is not readable in this guest (CNTVCT traps unless CNTHCTL.PL1VCTEN is
 * set by a hypervisor), so calibrate a busy-wait against timer ticks and
 * then repeat it with interrupts off: the tick count must not move.
 */
static void
splprobe(void)
{
	u32 old, t0, elapsed, n;

	n = 1000000;
	do {
		t0 = ticks;
		delay(n);
		elapsed = ticks - t0;
		n *= 2;
	} while(elapsed < 5 && n < 200000000);

	old = splhi();
	t0 = ticks;
	delay(n);
	printk("arm32: splhi %s (%d ticks per delay)\n",
		ticks == t0? "ok": "FAILED", elapsed);
	splx(old);

	t0 = ticks;
	delay(n);
	printk("arm32: splx %s\n", ticks != t0? "ok": "FAILED");
}

void
arm32main(u32 dtb)
{
	u32 *vecs, *p;

	uartinit();
	printk("\nHARVEY-ARM32 VIRT\n");
	printk("arm32: cpu id %p dtb %p (unused)\n", arm32id(), dtb);
	printk("arm32: kernel %p..%p\n", (u32)0x40010000, (u32)&arm32l1);

	arm32mmuinit();
	printk("arm32: mmu on\n");

	/* Copy the vectors to 0xffff0000 and switch SCTLR.V. */
	vecs = (u32*)HVECTORS;
	for(p = (u32*)arm32vectors; p < (u32*)arm32vectors_end; )
		*vecs++ = *p++;
	coherence();
	arm32sethighvectors();
	printk("arm32: vectors %p\n", (u32)HVECTORS);

	contextprobe();

	gicinit();
	timerstart();
	printk("arm32: timer %d Hz, tick %d Hz\n", arm32cntfrq(), Tickhz);

	__asm__ volatile("cpsie i" ::: "memory");
	printk("arm32: interrupts on\n");
	{
		char buf[64];

		snprint(buf, sizeof buf, "klibc %d %s %x", 42, "ok", 0xbeef);
		printk("arm32: snprint '%s'\n", buf);
	}

	/* Let the timer tick once, then check that masking works. */
	while(ticks == 0)
		;

	splprobe();

	for(;;)
		__asm__ volatile("wfi");
}
