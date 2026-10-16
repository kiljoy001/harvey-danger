/*
 * This file is part of the Harvey operating system.  It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * Early ARM64 boot entry.  The fixed QEMU UART is used only until the
 * device tree has supplied the actual UART address.
 */

#include "fdt.h"

enum {
	Uartbase = 0x09000000,
	Uartdr = 0x00 / 4,
	Uartfr = 0x18 / 4,
	Uarttxff = 1 << 5,
	Fdtmagic = 0xd00dfeed,
	Fdtmin = 40,
	Fdtmax = 16 * 1024 * 1024,
};

static usize uartbase = Uartbase;

static void
uartputc(int c)
{
	volatile u32 *uart;

	uart = (volatile u32 *)uartbase;
	while(uart[Uartfr] & Uarttxff)
		;
	uart[Uartdr] = c;
}

static void
uartputs(char *s)
{
	for(; *s != 0; s++){
		if(*s == '\n')
			uartputc('\r');
		uartputc(*s);
	}
}

static u32
beget4(u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static void
uarthex(usize n)
{
	int i, c;

	uartputs("0x");
	for(i = 15; i >= 0; i--){
		c = (n >> (i * 4)) & 15;
		uartputc(c < 10 ? '0' + c : 'a' + c - 10);
	}
}

__attribute__((noreturn)) void
arm64fault(void)
{
	u64 esr, elr, far;

	__asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
	__asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
	__asm__ volatile("mrs %0, far_el1" : "=r"(far));
	uartputs("ARM64 exception esr ");
	uarthex(esr);
	uartputs(" elr ");
	uarthex(elr);
	uartputs(" far ");
	uarthex(far);
	uartputs("\n");
	for(;;)
		__asm__ volatile("wfi");
}

void
arm64main(usize dtb)
{
	u8 *p;
	u32 size;
	Fdtinfo info;

	uartputs("HARVEY-ARM64 ENTRY\n");
	if(dtb == 0 || (dtb & 7) != 0){
		uartputs("dtb: missing or unaligned\n");
		return;
	}
	p = (u8 *)dtb;
	size = beget4(p + 4);
	if(beget4(p) != Fdtmagic || size < Fdtmin || size > Fdtmax){
		uartputs("dtb: invalid header\n");
		return;
	}
	uartputs("dtb: valid at ");
	uarthex(dtb);
	uartputs(", bytes ");
	uarthex(size);
	if(fdtparse(p, size, &info) < 0){
		uartputs("\ndtb: cannot discover RAM and UART\n");
		return;
	}
	uartputs("\nram: ");
	uarthex(info.rambase);
	uartputs(" bytes ");
	uarthex(info.ramsize);
	uartputs("\nuart: ");
	uarthex(info.uartbase);
	uartbase = info.uartbase;
	uartputs("\ngic: ");
	uarthex(info.gicdist);
	uartputs(" cpu ");
	uarthex(info.giccpu);
	uartputs("\ntimer irq: ");
	uarthex(info.timerirq);
	uartputs("\nHARVEY-ARM64 EARLY-BOOT PASS\n");
	if(arm64timerinit(&info) < 0){
		uartputs("timer: initialization failed\n");
		return;
	}
	if(arm64timerwait() < 0){
		uartputs("timer: interrupts not delivered, state ");
		uarthex(arm64timerdiag());
		uartputs("\n");
		arm64timerstop();
		return;
	}
	arm64timerstop();
	uartputs("HARVEY-ARM64 TIMER-IRQ PASS\n");
#ifdef ARM64_TEST_FAULT
	__asm__ volatile("brk #0");
#endif
}
