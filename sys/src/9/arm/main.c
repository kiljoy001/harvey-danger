/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * ARMv7-A QEMU "virt" bring-up: console, vectors, GIC and generic timer.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "arm.h"
#include "context.h"


enum {
	GiccIar		= 0x00c,
	GiccEoir	= 0x010,

	Timerirq	= 27,		/* virtual timer, PPI */
	Tickhz		= 100,
};

#define gicc(r)	(*(volatile u32*)((u32)GICC + (r)))

static volatile u32 ticks;

extern void (*consuartputs)(char *, int);

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
		diag("arm32: context switch FAILED\n");
	else
		diag("arm32: context switch ok\n");
	phase = 3;
	gotolabel(&bootlabel);
}

static void
taskb(void)
{
	if(phase != 1)
		diag("arm32: context order FAILED\n");
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
		diag("arm32: context probe did not return\n");
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

void
arm32irqhandler(void)
{
	u32 id;

	id = gicc(GiccIar) & 0x3ff;
	if(id >= 1020)			/* spurious */
		return;

	if(id == Timerirq){
		arm32clocktick();
		if(arm32portclock())
			arm32timerirq();
		else {
			arm32timerprobe();
			ticks++;
			if((ticks % Tickhz) == 0)
				diag("arm32: tick %d\n", (u32)(ticks / Tickhz));
		}
	}
	gicc(GiccEoir) = id;
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
arm32dataabort(u32 pc, u32 sp, u32 lr)
{
	u32 i, w;

	printk("arm32: data abort at %p far %p fsr %p\n",
		pc, arm32dfar(), arm32dfsr());
	printk("arm32: svc sp %p lr %p\n", sp, lr);
	for(i = 0; i < 24; i++){
		w = ((u32*)sp)[i];
		if(w >= 0x40010000 && w < (u32)&arm32l1)
			printk("  [%p] %p\n", sp + 4*i, w);
	}
	hardhalt();
}

/*
 * Verify that interrupts are really masked by splhi.  The generic counter
 * is not readable in this guest (CNTVCT traps unless CNTHCTL.PL1VCTEN is
 * set by a hypervisor), so calibrate a busy-wait against timer ticks and
 * then repeat it with interrupts off: the tick count must not move.
 */
static void
spin(u32 n)
{
	volatile u32 i;

	for(i = 0; i < n; i++)
		;
}

static void
splprobe(void)
{
	u32 old, t0, elapsed, n;

	n = 1000000;
	do {
		t0 = ticks;
		spin(n);
		elapsed = ticks - t0;
		n *= 2;
	} while(elapsed < 5 && n < 128000000);

	old = splhi();
	t0 = ticks;
	spin(n);
	diag("arm32: splhi %s (%d ticks per delay)\n",
		ticks == t0? "ok": "FAILED", elapsed);
	splx(old);

	t0 = ticks;
	spin(n);
	diag("arm32: splx %s\n", ticks != t0? "ok": "FAILED");
}

void
arm32main(u32 dtb)
{
	u32 *vecs, *p;

	uartinit();
	consuartputs = arm32conswrite;

	/* One Mach for the uniprocessor bring-up. */
	arm32setmach(&arm32machs[0]);
	machp()->machno = 0;
	machp()->online = 1;

	diag("\nHARVEY-ARM32 VIRT\n");
	diag("arm32: cpu id %p dtb %p (unused)\n", arm32id(), dtb);
	diag("arm32: kernel %p..%p\n", (u32)0x40010000, (u32)&arm32l1);

	arm32mmuinit();
	diag("arm32: mmu on\n");

	/* Copy the vectors to 0xffff0000 and switch SCTLR.V. */
	vecs = (u32*)HVECTORS;
	for(p = (u32*)arm32vectors; p < (u32*)arm32vectors_end; )
		*vecs++ = *p++;
	coherence();
	arm32sethighvectors();
	diag("arm32: vectors %p\n", (u32)HVECTORS);

	contextprobe();

	arm32gicinit();
	arm32timerinit();
	diag("arm32: timer %d Hz, tick %d Hz\n", arm32cntfrq(), Tickhz);

	__asm__ volatile("cpsie i" ::: "memory");
	diag("arm32: interrupts on\n");
	{
		char buf[64];

		snprint(buf, sizeof buf, "klibc %d %s %x", 42, "ok", 0xbeef);
		diag("arm32: snprint '%s'\n", buf);
	}

	/* The kernel allocator (sys/src/9/arm/qmalloc.c). */
	diag("arm32: kernel libc print works\n");
	mallocinit();
	{
		void *a, *b;

		a = malloc(1000);
		b = malloc(2000);
		if(a == nil || b == nil || a == b){
			diag("arm32: kernel malloc FAILED\n");
		} else {
			memmove(a, "allocator", 10);
			diag("arm32: kernel malloc ok '%s'\n", a);
		}
		free(a);
		free(b);
	}

	/* Let the timer tick once, then check that masking works. */
	while(ticks == 0)
		;

	splprobe();

	/*
	 * Hand over to the shared kernel: allocator, process table, console,
	 * clock, devices and the first scheduled kernel process.
	 */
	diag("arm32: starting kernel init\n");
	arm32kernelinit();
	for(;;)
		__asm__ volatile("wfi");
}
