/*
 * This file is part of the Harvey operating system.  It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * Early ARM64 boot entry.  The fixed QEMU UART is used only until the
 * device tree has supplied the actual UART address.
 */

#include "user.h"

int arm64contextprobe(void);

#ifdef ARM64_TEST_RESERVE
extern char __image_end[];
#endif

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
	u64 page0, page1, codepage, stackpage, codepage2, stackpage2;
	u64 root, userroot, userroot2, translated;
#ifdef ARM64_TEST_RESERVE
	u64 blocked;
#endif

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
#ifdef ARM64_TEST_RESERVE
	if(info.nreserve == Maxfdtreserve)
		return;
	blocked = ((u64)__image_end+4095) & ~(u64)4095;
	info.reserve[info.nreserve].base = blocked;
	info.reserve[info.nreserve++].size = 4096;
#endif
	if(arm64physinit(&info, dtb, size) < 0 ||
	   (page0 = arm64physpage()) == 0 ||
	   (page1 = arm64physpage()) == 0 ||
	   page1 == page0 || ((page0|page1)&4095) != 0 ||
	   *(u64 *)page0 != 0 || *(u64 *)(page0+4096-8) != 0 ||
	   *(u64 *)page1 != 0 || *(u64 *)(page1+4096-8) != 0){
		uartputs("physical pages: initialization failed\n");
		return;
	}
	*(u64 *)page0 = 0x1122334455667788ULL;
	if(*(u64 *)page0 != 0x1122334455667788ULL || *(u64 *)page1 != 0){
		uartputs("physical pages: isolation failed\n");
		return;
	}
	uartputs("physical page: ");
	uarthex(page0);
	uartputs("\nHARVEY-ARM64 PHYS-PAGE PASS\n");
#ifdef ARM64_TEST_RESERVE
	if(page0 <= blocked){
		uartputs("reserved page: allocator reused it\n");
		return;
	}
	uartputs("HARVEY-ARM64 RESERVED-PAGE PASS\n");
#endif
	codepage = arm64physpage();
	stackpage = arm64physpage();
	codepage2 = arm64physpage();
	stackpage2 = arm64physpage();
	root = arm64mmubuild(&info);
	if(codepage == 0 || stackpage == 0 || codepage2 == 0 ||
	   stackpage2 == 0 || root == 0 ||
	   (userroot = arm64mmuuser(root, codepage, stackpage)) == 0 ||
	   (userroot2 = arm64mmuuser(root, codepage2, stackpage2)) == 0){
		uartputs("MMU: page table build failed\n");
		return;
	}
	arm64mmuon(root);
	if(!arm64mmuactive() || *(u64 *)page0 != 0x1122334455667788ULL ||
	   *(u64 *)page1 != 0){
		uartputs("MMU: translated RAM check failed\n");
		return;
	}
	if(arm64mmutranslate(page0, &translated) < 0 || translated != page0 ||
	   arm64mmutranslate(info.uartbase, &translated) < 0 ||
	   translated != info.uartbase ||
	   arm64mmutranslate(info.rambase+info.ramsize-4096, &translated) < 0 ||
	   translated != info.rambase+info.ramsize-4096 ||
	   arm64mmutranslate(info.rambase+info.ramsize, &translated) == 0){
		uartputs("MMU: translation boundaries failed\n");
		return;
	}
	uartputs("HARVEY-ARM64 MMU-ON PASS\n");
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
	uartputs("HARVEY-ARM64 TIMER-IRQ PASS\n");
	arm64mmuswitch(userroot);
	if(arm64mmutranslate(ARM64USERBASE, &translated) < 0 ||
	   translated != codepage){
		uartputs("EL0: first address space failed\n");
		return;
	}
	if(arm64userprobe(codepage, stackpage, page0) < 0){
		uartputs("EL0: trap/isolation check failed, ESR ");
		uarthex(arm64userdiag());
		uartputs("\n");
		return;
	}
	arm64mmuswitch(userroot2);
	if(arm64mmutranslate(ARM64USERBASE, &translated) < 0 ||
	   translated != codepage2 ||
	   arm64userprobe(codepage2, stackpage2, page0) < 0){
		uartputs("EL0: second address space failed, ESR ");
		uarthex(arm64userdiag());
		uartputs("\n");
		return;
	}
	arm64mmuswitch(root);
	if(arm64mmutranslate(ARM64USERBASE, &translated) == 0){
		uartputs("EL0: user mapping visible in kernel root\n");
		return;
	}
	if(arm64contextprobe() < 0){
		uartputs("ARM64 context switch failed\n");
		return;
	}
	uartputs("HARVEY-ARM64 CONTEXT-SWITCH PASS\n");
	arm64timerstop();
	if(arm64el0irqs == 0){
		uartputs("EL0: timer did not interrupt user code\n");
		return;
	}
	uartputs("HARVEY-ARM64 EL0-SVC PASS\n");
	uartputs("HARVEY-ARM64 EL0-ISOLATION PASS\n");
	uartputs("HARVEY-ARM64 EL0-PERMISSIONS PASS\n");
	uartputs("HARVEY-ARM64 ADDRESS-SPACE SWITCH PASS\n");
	uartputs("HARVEY-ARM64 EL0-TIMER-IRQ PASS\n");
#ifdef ARM64_TEST_FAULT
	__asm__ volatile("brk #0");
#endif
}
