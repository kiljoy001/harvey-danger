/*
 * RK3566 board quirks.
 *
 * The vendor TF-A owns the NPU (and GPU) clocks and exposes them through
 * the ARM SCMI clock protocol on the SMC transport.  The device tree
 * describes the NPU registers but not the clock and power sequence needed
 * to reach them, so that sequence lives here, in a board file selected by
 * platform identity.  Generic paths never call into this file.
 */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "fdt.h"

extern void arm64uartputs(char *);
#define uartputs arm64uartputs

extern Dev etherdevtab, ipdevtab;
extern void ethergmacinit(Fdtinfo *);
extern void ethermediumlink(void);
extern int rk3566pipepoweron(void);

/*
 * Late RK356x bring-up, after the generic boot probes.  Everything here
 * is board policy: the watchdog U-Boot may have left running, the PCIe
 * PHY power domain, and the GMAC.  The generic path selects it by
 * Fdtboardrk3566/Fdtboardrk3568 and never calls it on another SoC.
 */
void
rk356xlateinit(Fdtinfo *info)
{
	Fdtdev *wdt;
	u64 wdtbase;

	if(info == nil)
		return;
	wdt = fdtdev(info, Fdtclasswdt);
	wdtbase = wdt != nil && wdt->base != 0 ? wdt->base : 0xfe600000ULL;
	*(volatile u32 *)wdtbase = 0;		/* WDT_CR */
	*(volatile u32 *)(wdtbase + 0xc) = 0x76;	/* WDT_CRR */
	if(rk3566pipepoweron() < 0)
		uartputs("HARVEY-ARM64 PD_PIPE FAIL\n");
	else
		uartputs("HARVEY-ARM64 PD_PIPE PASS\n");
#ifndef ARM64_NO_GMAC
	ethergmacinit(info);
	ethermediumlink();
	etherdevtab.reset();
	ipdevtab.reset();
#endif
}

#ifdef ARM64_RK3566_NPU_PROBE

/* SCMI shared-memory page; /chosen/harvey,scm overrides it. */
static u64 scmbase = 0x10f000ULL;

static void
uarthex(usize n)
{
	char buf[19];
	int i, c;

	buf[0] = '0';
	buf[1] = 'x';
	for(i = 0; i < 16; i++){
		c = (n >> ((15-i)*4)) & 15;
		buf[2+i] = c < 10 ? '0'+c : 'a'+c-10;
	}
	buf[18] = 0;
	uartputs(buf);
}

/*
 * Minimal SCMI client.  RK3566 TF-A owns the NPU (and GPU) clocks and
 * exposes them through the ARM SCMI clock protocol on the SMC transport:
 * function id 0x82000010, message buffer at the reserved page, by default 0x10f000.
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
	volatile Scmishmem *s = (volatile Scmishmem *)scmbase;
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
	r = smccall(0x82000010ULL, scmbase, 0);
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
		volatile Scmishmem *s = (volatile Scmishmem *)scmbase;
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
	uarthex(((volatile Scmishmem *)scmbase)->status);
	uartputs("\n");
}

void
rk3566npuprobe(Fdtinfo *info, u64 dmapage)
{
	u32 i, st, attr = 0;
	u64 v;

	if(info != nil && info->scmbase != 0)
		scmbase = info->scmbase;
	(void)dmapage;

	uartputs("psci ");
	uarthex(smccall(0x84000000ULL, 0, 0));
	uartputs(" smc1 ");
	uarthex(smccall(0x82000010ULL, scmbase, 0));
	uartputs("\n");

	uartputs("npu scmi channel ");
	uarthex(((volatile Scmishmem *)scmbase)->status);
	uartputs("\n");

	/* BASE PROTOCOL_VERSION: no payload, reply is status + version. */
	{
		volatile Scmishmem *sh = (volatile Scmishmem *)scmbase;

		for(i = 0; i < 1000000 && !(sh->status & 1); i++)
			__asm__ volatile("nop");
		sh->status = 0;
		sh->flags = 0;
		sh->length = 4;
		sh->header = (0x10 << 10);
		__asm__ volatile("dsb sy" ::: "memory");
		smccall(0x82000010ULL, scmbase, 0);
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
		volatile Scmishmem *sh = (volatile Scmishmem *)scmbase;

		for(i = 0; i < 1000000 && !(sh->status & 1); i++)
			__asm__ volatile("nop");
		sh->status = 0;
		sh->flags = 0;
		sh->length = 4;
		sh->header = 0x06 | (0x10 << 10);
		__asm__ volatile("dsb sy" ::: "memory");
		smccall(0x82000010ULL, scmbase, 0);
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
	v = ((volatile u64 *)scmbase)[0];
	uartputs("shmem0 ");
	uarthex(v);
	uartputs("\n");
	/*
	 * Bring the block up in the order the vendor genpd code uses:
	 * clocks and resets first, then power, then release the NIU idle
	 * request.  The NPU NIU appears to hold its idle ack until its
	 * hclk/aclk are actually running, which is why the earlier
	 * power-then-clock ordering never saw the ack drop.
	 */
	{
		volatile u32 *pmu = (volatile u32 *)0xfdd90000ULL;
		volatile u32 *cru = (volatile u32 *)0xfdd20000ULL;
		volatile u32 *npu = (volatile u32 *)0xfde40000ULL;

		uartputs("before st ");
		uarthex(pmu[0x0098/4]);
		uartputs(" req ");
		uarthex(pmu[0x0050/4]);
		uartputs(" ack ");
		uarthex(pmu[0x0060/4]);
		uartputs(" idle ");
		uarthex(pmu[0x0068/4]);
		uartputs("\n");

		/* Clocks: mux gpll/2, hclk/pclk /4, NPU gates 0-12 open. */
		cru[0x11c/4] = 0x81cf0000U | (1U<<6) | 1U;
		cru[0x120/4] = 0x00ff0000U | 0x33U;
		cru[0x30c/4] = 0x1fff0000U;
		/* srst_a/srst_h are bits 11/12 of SOFTRST_CON(2). */
		cru[0x408/4] = (0x1800U<<16) | 0x1800U;
		for(i = 0; i < 1000; i++)
			__asm__ volatile("nop");
		cru[0x408/4] = (0x1800U<<16) | 0U;
		__asm__ volatile("dsb sy" ::: "memory");
		uartputs("clk: sel7 ");
		uarthex(cru[0x11c/4]);
		uartputs(" sel8 ");
		uarthex(cru[0x120/4]);
		uartputs(" gate3 ");
		uarthex(cru[0x30c/4]);
		uartputs(" softrst2 ");
		uarthex(cru[0x408/4]);
		uartputs(" ack ");
		uarthex(pmu[0x0060/4]);
		uartputs("\n");

		/* Power on: clear pwr bit 1, wait for status bit 1 to clear. */
		pmu[0x00a0/4] = 0x00020000U;
		for(i = 0; i < 100000; i++){
			if((pmu[0x0098/4] & 2) == 0)
				break;
			__asm__ volatile("nop");
		}
		uartputs("powered st ");
		uarthex(pmu[0x0098/4]);
		uartputs(" w ");
		uarthex(i);
		uartputs(" ack ");
		uarthex(pmu[0x0060/4]);
		uartputs("\n");

		/* Release the NIU idle request (bit 2) and wait for the ack. */
		pmu[0x0050/4] = 0x00040000U;
		__asm__ volatile("dsb sy" ::: "memory");
		for(i = 0; i < 1000000; i++){
			if((pmu[0x0060/4] & 4) == 0 && (pmu[0x0068/4] & 4) == 0)
				break;
			__asm__ volatile("nop");
		}
		uartputs("released req ");
		uarthex(pmu[0x0050/4]);
		uartputs(" ack ");
		uarthex(pmu[0x0060/4]);
		uartputs(" idle ");
		uarthex(pmu[0x0068/4]);
		uartputs(" w ");
		uarthex(i);
		uartputs("\n");

		if((pmu[0x0060/4] & 4) != 0)
			uartputs("npu niu stays idle; not touching 0xfde40000\n");
		else{
			uartputs("npu version ");
			uarthex(npu[0x0000/4]);
			uartputs(" vnum ");
			uarthex(npu[0x0004/4]);
			uartputs(" iommu ");
			uarthex(((volatile u32 *)0xfde4b000ULL)[0x0004/4]);
			uartputs("\n");
		}
	}
	uartputs("HARVEY-ARM64 RK3566-NPU PROBE-DONE\n");
}

#endif
