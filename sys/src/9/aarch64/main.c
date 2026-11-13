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
extern u64 arm64teeshmbase, arm64teeshmsize;
extern u64 arm64bootarchbase;
extern u32 arm64bootarchsize;

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
	int n;

	if(uartbase == 0)
		return;
	uart = (volatile u32 *)uartbase;
	/*
	 * Never wait forever for FIFO room.  print() runs under ilock (IRQs
	 * masked), so an unbounded wait here stops the clock and parks every
	 * process if the console reader stalls.  Dropping output is much
	 * better than hanging the machine.
	 */
	if(uarttype == Fdtdwuart){
		/* DW UART: USR.TFNF reports room in the transmit FIFO. */
		for(n = 0; n < 1000000; n++)
			if(uart[0x7c/4] & (1<<1))
				break;
		if(n == 1000000)
			return;
	}else if(uarttype == Fdtpl011){
		for(n = 0; n < 1000000; n++)
			if((uart[Uartfr] & Uarttxff) == 0)
				break;
		if(n == 1000000)
			return;
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

/* Optional U-Boot-supplied boot archive (programs for /boot). */
enum { BootarchiveAddr = 0x50000000 };
u64 arm64bootarchbase;
u32 arm64bootarchsize;

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
/*
 * Minimal SCMI client.  RK3566 TF-A owns the NPU (and GPU) clocks and
 * exposes them through the ARM SCMI clock protocol on the SMC transport:
 * function id 0x82000010, message buffer at the reserved 0x10f000 page.
 * Without enabling the NPU clock through it, any read of 0xfde4b000 or
 * 0xfde40000 wedges the AXI bus.
 */
typedef struct Scmishmem Scmishmem;
struct Scmishmem {
	u32	rsvd;
	u32	status;
	u32	rsvd1[2];
	u32	flags;
	u32	length;
	u32	header;
	u32	payload[32];
};

enum {
	Scmiprotobase	= 0x10,
	Scmiprotoclock	= 0x14,
	Scmiclockattr	= 3,
	Scmiclockrateget = 6,
	Scmiclockcfgset = 7,
	Scmiclockenable = 1,
};

static u64
smccall(u64 id, u64 a1, u64 a2)
{
	register u64 x0 __asm__("x0") = id;
	register u64 x1 __asm__("x1") = a1;
	register u64 x2 __asm__("x2") = a2;

	__asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2) : "memory");
	return x0;
}

static u32 scmiret;

static u32
scmicall(u32 proto, u32 cmd, u32 a0, u32 a1, u32 *out)
{
	volatile Scmishmem *s = (volatile Scmishmem *)0x10f000ULL;
	u64 r;
	u32 i, v;

	for(i = 0; i < 1000000 && !(s->status & 1); i++)
		__asm__ volatile("nop");
	s->status = 0;
	s->flags = 0;
	/* SCMI header: id[7:0], type[9:8], protocol[17:10], token[27:18]. */
	s->header = (cmd & 0xff) | ((proto & 0xff) << 10);
	if(a1 != 0){
		s->length = 12;
		s->payload[0] = a0;
		s->payload[1] = a1;
	}else{
		s->length = 8;
		s->payload[0] = a0;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	/* The vendor TF-A takes the SCMI shmem address in x1. */
	r = smccall(0x82000010ULL, 0x10f000ULL, 0);
	scmiret = (u32)r;
	for(i = 0; i < 1000000 && !(s->status & 3); i++)
		__asm__ volatile("nop");
	v = s->payload[0];
	if(out != nil)
		*out = s->payload[1];
	return v;
}

static void
scmiprint(u32 id)
{
	u32 st, attr, rate;
	char name[17];
	int i;

	attr = 0;
	st = scmicall(Scmiprotoclock, Scmiclockattr, id, 0, &attr);
	uartputs("scmi clk ");
	uarthex(id);
	uartputs(" attr ");
	uarthex(attr);
	/* the short name follows the attributes word, 16 bytes */
	{
		volatile Scmishmem *s = (volatile Scmishmem *)0x10f000ULL;
		for(i = 0; i < 16; i++)
			name[i] = ((u8 *)&s->payload[2])[i];
		name[16] = 0;
	}
	uartputs(" st ");
	uarthex(st);
	uartputs(" name ");
	uartputs(name);
	rate = 0;
	st = scmicall(Scmiprotoclock, Scmiclockrateget, id, 0, &rate);
	uartputs(" rate ");
	uarthex(rate);
	uartputs(" st ");
	uarthex(st);
	uartputs(" smcret ");
	uarthex(scmiret);
	uartputs(" chan ");
	uarthex(((volatile Scmishmem *)0x10f000ULL)->status);
	uartputs("\n");
}

static void
rk3566npuprobe(u64 dmapage)
{
	u32 i, st, attr = 0;
	u64 v;

	(void)dmapage;

	uartputs("psci ");
	uarthex(smccall(0x84000000ULL, 0, 0));
	uartputs(" smc1 ");
	uarthex(smccall(0x82000010ULL, 0x10f000ULL, 0));
	uartputs("\n");

	uartputs("npu scmi channel ");
	uarthex(((volatile Scmishmem *)0x10f000ULL)->status);
	uartputs("\n");

	/* BASE PROTOCOL_VERSION: no payload, reply is status + version. */
	{
		volatile Scmishmem *sh = (volatile Scmishmem *)0x10f000ULL;

		for(i = 0; i < 1000000 && !(sh->status & 1); i++)
			__asm__ volatile("nop");
		sh->status = 0;
		sh->flags = 0;
		sh->length = 4;
		sh->header = (0x10 << 10);
		__asm__ volatile("dsb sy" ::: "memory");
		smccall(0x82000010ULL, 0x10f000ULL, 0);
		for(i = 0; i < 1000000 && !(sh->status & 3); i++)
			__asm__ volatile("nop");
		uartputs("base ver st ");
		uarthex(sh->payload[0]);
		uartputs(" ver ");
		uarthex(sh->payload[1]);
		uartputs(" len ");
		uarthex(sh->length);
		uartputs(" hdr ");
		uarthex(sh->header);
		uartputs(" w ");
		for(int k = 0; k < 6; k++){
			uarthex(sh->payload[k]);
			uartputs(" ");
		}
		uartputs("\n");
	}
	/* BASE DISCOVER_LIST_PROTOCOLS. */
	{
		volatile Scmishmem *sh = (volatile Scmishmem *)0x10f000ULL;

		for(i = 0; i < 1000000 && !(sh->status & 1); i++)
			__asm__ volatile("nop");
		sh->status = 0;
		sh->flags = 0;
		sh->length = 4;
		sh->header = 0x06 | (0x10 << 10);
		__asm__ volatile("dsb sy" ::: "memory");
		smccall(0x82000010ULL, 0x10f000ULL, 0);
		for(i = 0; i < 1000000 && !(sh->status & 3); i++)
			__asm__ volatile("nop");
		uartputs("base protos st ");
		uarthex(sh->payload[0]);
		uartputs(" n ");
		uarthex(sh->payload[1]);
		uartputs(" w0 ");
		uarthex(sh->payload[2]);
		uartputs(" w1 ");
		uarthex(sh->payload[3]);
		uartputs("\n");
	}
	for(i = 0; i < 4; i++)
		scmiprint(i);
	st = scmicall(Scmiprotoclock, Scmiclockcfgset, 2, Scmiclockenable, &attr);
	uartputs("clk2 enable st ");
	uarthex(st);
	uartputs(" smcret ");
	uarthex(scmiret);
	uartputs("\n");
	v = ((volatile u64 *)0x10f000ULL)[0];
	uartputs("shmem0 ");
	uarthex(v);
	uartputs("\n");
	/* Now try the block itself: power domain, resets, version. */
	{
		volatile u32 *pmu = (volatile u32 *)0xfdd90000ULL;
		volatile u32 *cru = (volatile u32 *)0xfdd20000ULL;
		volatile u32 *npu = (volatile u32 *)0xfde40000ULL;
		volatile u32 *mmu = (volatile u32 *)0xfde4b000ULL;

		pmu[0x0050/4] = 0x00040004U;
		pmu[0x00a0/4] = 0x00020000U;
		pmu[0x0050/4] = 0x00040000U;
		cru[0x11c/4] = 0x81cf0000U | (1U<<6) | 1U;
		cru[0x120/4] = 0x00ff0000U | 0x33U;
		cru[0x30c/4] = 0x1fff0000U;
		cru[0x408/4] = (0x1800U<<16) | 0x1800U;
		for(i = 0; i < 1000; i++)
			__asm__ volatile("nop");
		cru[0x408/4] = (0x1800U<<16) | 0U;
		__asm__ volatile("dsb sy" ::: "memory");
		uartputs("npu pmu ");
		uarthex(pmu[0x0098/4]);
		/*
		 * Reading 0xfde40000 or 0xfde4b000 still aborts even with
		 * PD_NPU on, vdd_npu at 900 mV, the CRU NPU clocks open and
		 * the SCMI npu clock enabled.  The block itself is the next
		 * thing to sort out; do not take the machine down here.
		 */
		(void)npu;
		(void)mmu;
		uartputs("\n");
	}
	uartputs("HARVEY-ARM64 RK3566-NPU PROBE-DONE\n");
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
	{
		u64 spsr;
		__asm__ volatile("mrs %0, spsr_el1" : "=r"(spsr));
		uartputs(" spsr ");
		uarthex(spsr);
	}
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

	arm64machinit();
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
	/*
	 * The vendor RK3566 OP-TEE build's memory map puts the static
	 * non-secure shared memory at 0x09200000, 2 MiB (type
	 * MEM_AREA_NSEC_SHM, right after TEE/TA RAM).  Reserve it so the
	 * page allocator never reuses it, and publish it as "teeshm" for
	 * the userspace OP-TEE client.
	 */
	if(info.nreserve < Maxfdtreserve && 0x9200000 >= info.rambase &&
	   0x9200000 + 0x200000 <= info.rambase + info.ramsize){
		info.reserve[info.nreserve].base = 0x9200000;
		info.reserve[info.nreserve++].size = 0x200000;
		arm64teeshmbase = 0x9200000;
		arm64teeshmsize = 0x200000;
	}
	/*
	 * U-Boot may load a boot archive next to the kernel (see the board
	 * boot command).  Reserve it before the page allocator exists so the
	 * blobs stay intact; program names in it replace the embedded ones.
	 */
	/*
	 * QEMU virt with 768 MiB puts the DMA arena at 0x50000000, which is
	 * also the board's archive address.  Scan the board address first,
	 * then the lower QEMU slot that clears the arena.
	 */
	{
		static const u64 cand[] = { 0x50000000ULL, 0x49000000ULL };
		u64 addr;
		u32 ci;

		for(ci = 0; ci < sizeof(cand)/sizeof(cand[0]) && info.nreserve < Maxfdtreserve; ci++){
			u8 *ap;
			u32 total = 0;

			addr = cand[ci];
			if(addr < info.rambase ||
			   addr + 24 > info.rambase + info.ramsize)
				continue;
			ap = (u8 *)addr;
			if(ap[0]=='O' && ap[1]=='9' && ap[2]=='B' && ap[3]=='O' &&
			   ap[4]=='O' && ap[5]=='T' && ap[6]=='0' && ap[7]=='1')
				total = (u32)ap[16] | (u32)ap[17]<<8 |
				        (u32)ap[18]<<16 | (u32)ap[19]<<24;
			if(total < 24 || total > 1280u*1024*1024 ||
			   addr < (u64)__image_end ||
			   addr+total > info.rambase+info.ramsize)
				continue;
			info.reserve[info.nreserve].base = addr;
			info.reserve[info.nreserve++].size = total;
			arm64bootarchbase = addr;
			arm64bootarchsize = total;
			break;
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
	/*
	 * Probe the reserved DMA arena through its identity mapping.  This is
	 * the physical range user space receives as #P/dma; a bus abort here
	 * names the failing block instead of leaving a user fault looping.
	 */
	if(arm64dmabase != 0 && arm64dmasize != 0){
		u64 off;
		volatile u64 *q;

		for(off = 0; off < arm64dmasize; off += 2*1024*1024){
			q = (volatile u64 *)(arm64dmabase + off);
			*q = 0x0123456789abcdefULL;
			if(*q != 0x0123456789abcdefULL){
				uartputs("ARM64 DMA-PROBE FAIL ");
				uarthex(arm64dmabase + off);
				uartputs("\n");
				break;
			}
		}
		if(off >= arm64dmasize)
			uartputs("HARVEY-ARM64 DMA-PROBE PASS\n");
	}
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
