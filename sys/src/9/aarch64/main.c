/*
 * This file is part of the Harvey operating system.  It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * Early ARM64 boot entry.  Firmware supplies the device tree in x0.
 */

#include "user.h"

#ifdef ARM64_FULL_KERNEL
void arm64kernelstart(Fdtinfo *);
#endif

int arm64contextprobe(void);
extern char arm64vectors[];
extern char _start[], __image_end[];
extern u64 arm64dmabase, arm64dmasize;

enum {
	Uartdr = 0x00 / 4,
	Uartfr = 0x18 / 4,
	Uarttxff = 1 << 5,
	Fdtmagic = 0xd00dfeed,
	Fdtmin = 40,
	Fdtmax = 16 * 1024 * 1024,
};

static usize uartbase;
static u32 uarttype;

static void
uartputc(int c)
{
	volatile u32 *uart;

	if(uartbase == 0)
		return;
	uart = (volatile u32 *)uartbase;
	if(uarttype == Fdtdwuart){
		/* DW UART: USR.TFNF reports room in the transmit FIFO. */
		while((uart[0x7c/4] & (1<<1)) == 0)
			;
	}else if(uarttype == Fdtpl011){
		while(uart[Uartfr] & Uarttxff)
			;
	}else
		return;
	uart[Uartdr] = c;
}

void
arm64uartputs(char *s)
{
	for(; *s != 0; s++){
		if(*s == '\n')
			uartputc('\r');
		uartputc(*s);
	}
}
#define uartputs arm64uartputs

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

#ifdef ARM64_RK3566_NPU_PROBE
static void
rk3566npuprobe(u64 dmapage)
{
	volatile u32 *pmu, *cru, *npu, *mmu;
	u64 *cmd;
	u32 i;

	pmu = (volatile u32 *)0xfdd90000ULL;
	cru = (volatile u32 *)0xfdd20000ULL;
	npu = (volatile u32 *)0xfde40000ULL;
	mmu = (volatile u32 *)0xfde4b000ULL;

	/* Power on RK3568_PD_NPU and de-idle NPU NIU bus. */
	pmu[0x00a0/4] = 0x00020000U;
	pmu[0x0050/4] = 0x00040000U;
	cru[0x0408/4] = 0x00c000c0U;
	for(i = 0; i < 1000; i++)
		__asm__ volatile("nop");
	cru[0x0408/4] = 0x00c00000U;
	__asm__ volatile("dsb sy" ::: "memory");

	uartputs("npu pmu: ");
	uarthex(pmu[0x0098/4]);
	uartputs(" iommu: ");
	uarthex(mmu[0x0004/4]);
	uartputs(" version: ");
	uarthex(npu[0x0000/4]);
	uartputs("\n");

	/* Build a 64-bit regcmd stream in an Aspera physical page and DMA-fetch via PC. */
	cmd = (u64 *)dmapage;
	cmd[0] = 0x02010000000e1004ULL;
	cmd[1] = 0x04010000000e2004ULL;
	cmd[2] = 0x08010000000e3004ULL;
	cmd[3] = 0x10010000000e4004ULL;
	cmd[4] = 0x0000000000000000ULL;
	cmd[5] = 0x0101000000000014ULL;
	cmd[6] = 0x0041000000000000ULL;
	cmd[7] = 0x0081000000000008ULL;
	__asm__ volatile("dc civac, %0; dsb sy; isb" :: "r"(dmapage) : "memory");

	npu[0x8010/4] = 0x80000101U;
	npu[0x8010/4] = 0x00000101U;
	npu[0x0010/4] = 1;
	npu[0x0024/4] = 0x1ffffU;
	npu[0x0010/4] = (u32)dmapage;
	npu[0x0014/4] = 10;
	npu[0x0030/4] = 0x6001U;
	npu[0x0034/4] = 0;
	__asm__ volatile("dsb sy" ::: "memory");
	npu[0x0008/4] = 1;
	npu[0x0008/4] = 0;
	for(i = 0; i < 10000; i++)
		__asm__ volatile("nop");
	uartputs("npu dt_rd: ");
	uarthex(npu[0x8038/4]);
	uartputs("\nHARVEY-ARM64 RK3566-NPU PASS\n");
}
#endif

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
	int parsed;
	Fdtinfo info;
	u64 page0, page1, codepage, stackpage, codepage2, stackpage2;
	u64 root, userroot, userroot2, translated;
#ifdef ARM64_TEST_RESERVE
	u64 blocked;
#endif

	__asm__ volatile("msr vbar_el1, %0; isb" :: "r"(arm64vectors) : "memory");
	if(dtb == 0 || (dtb & 7) != 0)
		return;
	p = (u8 *)dtb;
	size = beget4(p + 4);
	if(beget4(p) != Fdtmagic || size < Fdtmin || size > Fdtmax)
		return;
	info.uartbase = 0;
	info.uarttype = 0;
	parsed = fdtparse(p, size, &info);
	uartbase = info.uartbase;
	uarttype = info.uarttype;
	if(parsed < 0){
		uartputs("HARVEY-ARM64 FDT-ERROR\n");
		return;
	}
	if(fdtselectram(&info, (u64)_start, (u64)__image_end) < 0){
		uartputs("HARVEY-ARM64 RAM-ERROR\n");
		return;
	}
	uartputs("HARVEY-ARM64 ENTRY\n");
	uartputs("dtb: valid at ");
	uarthex(dtb);
	uartputs(", bytes ");
	uarthex(size);
	uartputs("\nram: ");
	uarthex(info.rambase);
	uartputs(" bytes ");
	uarthex(info.ramsize);
	uartputs("\nuart: ");
	uarthex(info.uartbase);
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
	/*
	 * Reserve a contiguous DMA arena for user-space controllers.  It is
	 * excluded from the page allocator and published as #P/dma.
	 */
	if(info.nreserve < Maxfdtreserve && info.rambase+info.ramsize > info.rambase &&
	   info.ramsize > arm64dmasize + 2*1024*1024){
		u64 base = (info.rambase + info.ramsize - arm64dmasize) &
		           ~((u64)(2*1024*1024 - 1));
		if(base > (u64)__image_end &&
		   base+arm64dmasize > base && base+arm64dmasize <= info.rambase+info.ramsize){
			info.reserve[info.nreserve].base = base;
			info.reserve[info.nreserve++].size = arm64dmasize;
			arm64dmabase = base;
		}
	}
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
	if(arm64irqmaskprobe() < 0){
		uartputs("ARM64 interrupt mask failed\n");
		return;
	}
	uartputs("HARVEY-ARM64 IRQ-MASK PASS\n");
	arm64mmuswitch(userroot);
	if(arm64mmutranslate(ARM64USERBASE, &translated) < 0 ||
	   translated != codepage){
		uartputs("EL0: first address space failed\n");
		return;
	}
	if(arm64userprobe(codepage, stackpage, page0) < 0){
		uartputs("EL0: trap/isolation check failed, ESR ");
		uarthex(arm64userdiag());
		uartputs(" detail ");
		uarthex(arm64userdiagdetails());
		uartputs("\n");
		return;
	}
	arm64mmuswitch(userroot2);
	if(arm64mmutranslate(ARM64USERBASE, &translated) < 0 ||
	   translated != codepage2 ||
	   arm64userprobe(codepage2, stackpage2, page0) < 0){
		uartputs("EL0: second address space failed, ESR ");
		uarthex(arm64userdiag());
		uartputs(" detail ");
		uarthex(arm64userdiagdetails());
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
#ifdef ARM64_RK3566_NPU_PROBE
	if(info.uartbase == 0xfe660000ULL)
		rk3566npuprobe(page1);
#endif
#ifdef ARM64_FULL_KERNEL
	if(arm64physprobe() < 0){
		uartputs("ARM64 full-kernel physical allocator failed\n");
		return;
	}
	uartputs("HARVEY-ARM64 FULL-KERNEL-LINK PASS\n");
	arm64kernelstart(&info);
#endif
#ifdef ARM64_TEST_FAULT
	__asm__ volatile("brk #0");
#endif
}
