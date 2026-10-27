/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM64 RK3566 DWC3/xHCI transport, #X/<controller>.
 *
 * The xHCI command/event/transfer engine lives in user space (o9xhci).
 * This device only owns the controller MMIO window, the DMA arena, and
 * the DWC3/PHY/clock/PMIC glue needed to put the controller in host mode.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"

enum {
	Nctlr		= 8,
	Dmasize		= 4*1024*1024,
	Regbase		= 0x100000,
	Dwc3Base	= 0xfd000000,
	Otg0Base	= 0xfcc00000,
	Dwc3Size	= 0x100000,

	CruBase		= 0xfdd20000,
	PmucruBase	= 0xfdd00000,
	PmuBase		= 0xfdd90000,
	PmuGrfBase	= 0xfdc20000,
	Usbgrf0Base	= 0xfdca0000,
	Usbgrf1Base	= 0xfdca8000,
	Usb2phy0Base	= 0xfe8a0000,
	Usb2phy1Base	= 0xfe8b0000,
	CombphyBase	= 0xfe830000,
	PipeGrfBase	= 0xfdc50000,
	PipePhyGrfBase	= 0xfdc80000,
	Gpio0Base	= 0xfdd60000,
};
#define Dmabase 0x100000000ULL

typedef struct Xtransport Xtransport;
struct Xtransport {
	QLock lock;
	volatile u32 *regs;
	u32 size, op;
	void *dma;
	u64 bus;
	int owned, poisoned;
};
static Xtransport controllers[Nctlr];
static int ncontrollers;
static u32 dbg_gsnpsid, dbg_gctl, dbg_usb2, dbg_usb3;
static u32 dbg_pmugrf, dbg_gpio, dbg_gpio_ddr, dbg_grf0, dbg_grf0sus;

/* Rockchip write-mask helper: upper half selects the bits being written. */
static void
wm(volatile u32 *base, int off, u32 mask, u32 val)
{
	base[off/4] = (val & mask) | (mask << 16);
}

static int
xtrusted(void)
{
	Proc *up = externup();

	return iseve() || strcmp(up->user, "root") == 0;
}

static void
gpioout(volatile u32 *gpio, int pin)
{
	u32 off, v;
	int high;

	high = pin >= 16;
	pin %= 16;
	off = high ? 0x0c : 0x08;
	v = gpio[off/4] & 0xffff;
	v |= 1U << pin;
	gpio[off/4] = v | 0xffff0000;		/* DDR: output */
	off = high ? 0x04 : 0x00;
	v = gpio[off/4] & 0xffff;
	v |= 1U << pin;
	gpio[off/4] = v | 0xffff0000;		/* DR: high */
}

static void
upd(volatile u32 *base, int off, u32 mask, u32 val)
{
	base[off/4] = (base[off/4] & ~mask) | (val & mask);
}

/*
 * RK3568 naneng-combphy USB3 mode.  The board's blue USB3 port needs this
 * before the DWC3 core can train a SuperSpeed link. The registers below
 * select the 100 MHz reference path, matching Linux's 100 MHz PHY setup.
 */
static void
combphyinit(void)
{
	volatile u32 *cru = (volatile u32 *)KADDR(CruBase);
	volatile u32 *pmucru = (volatile u32 *)KADDR(PmucruBase);
	volatile u32 *phy = (volatile u32 *)KADDR(CombphyBase);
	volatile u32 *pp = (volatile u32 *)KADDR(PipePhyGrfBase);

	/* PCLK_PIPEPHY1; 100 MHz refclk from PLL ph0 / 2 (ppll=200 MHz). */
	wm(cru, 0x388, 1U<<5, 0);
	wm(pmucru, 0x188, (1U<<9)|(1U<<10), 0);
	wm(pmucru, 0x124, (0x7U<<4)|(1U<<7), (1U<<4)|(1U<<7));
	wm(cru, 0x470, (1U<<6)|(1U<<7), 0);
	microdelay(1000);

	/* PLL/SSC and receiver tuning for USB3. */
	upd(phy, 0x7c, 0x30, 0x10);
	upd(phy, 0x38, 0x01, 0x01);
	upd(phy, 0x80, 0x1c, 0x08);
	phy[0x2c/4] = 0x04;
	upd(phy, 0x14, 0xc0, 0x40);
	phy[0x44/4] = 0x32;
	phy[0x28/4] = 0xf0;
	phy[0x50/4] = 0x0e;

	/* pipe_phy_grf: USB3 mode, 100 MHz refclk, PIPE selects. */
	wm(pp, 0x0004, 3U<<13, 2U<<13);
	wm(pp, 0x000c, 3U<<13, 1U<<13);
	wm(pp, 0x0008, 1U<<15, 0);
	wm(pp, 0x0008, 1U<<12, 0);
	wm(pp, 0x0000, 0x3f, 0x04);
	wm(pp, 0x0014, 3U, 3);
	microdelay(1000);
}

/*
 * Put the selected DWC3 controller into host mode.  The sequence follows
 * the RK3568 Linux/U-Boot glue: PD_PIPE, CRU gates, resets, USB2 PHY,
 * VBUS, GCTL port capability, and the UTMI/PIPE interface registers.
 */
static int
dwc3init(void)
{
	volatile u32 *cru = (volatile u32 *)KADDR(CruBase);
	volatile u32 *pmucru = (volatile u32 *)KADDR(PmucruBase);
	volatile u32 *pmu = (volatile u32 *)KADDR(PmuBase);
	volatile u32 *pmugrf = (volatile u32 *)KADDR(PmuGrfBase);
	volatile u32 *grf0 = (volatile u32 *)KADDR(Usbgrf0Base);
	volatile u32 *grf1 = (volatile u32 *)KADDR(Usbgrf1Base);
	volatile u32 *gpio = (volatile u32 *)KADDR(Gpio0Base);
	volatile u32 *dwc = (volatile u32 *)KADDR(Dwc3Base);
	u32 v;

	/* PD_PIPE power on and idle request clear. */
	wm(pmu, 0xa0, 1U<<8, 0);
	wm(pmu, 0x50, 1U<<11, 0);

	/* ACLK_PIPE/PCLK_PIPE and USB3OTG1 gates. */
	wm(cru, 0x328, (1U<<0)|(1U<<1)|(1U<<12)|(1U<<13)|(1U<<14), 0);

	/* USB2 PHY0/USB2 PHY1 reference gates and GPIO0 pclk/dbclk. */
	wm(pmucru, 0x188, (1U<<1)|(1U<<2), 0);
	wm(pmucru, 0x184, (1U<<9)|(1U<<10), 0);

	/* Deassert SRST_USB3OTG1 (bit 5). */
	wm(cru, 0x424, 1U<<5, 0);

	/* VBUS for the USB host ports: GPIO0_A6, mux function 0. */
	wm(pmugrf, 0x04, 0xfU<<8, 0);
	gpioout(gpio, 6);

	/* USB2 PHY0 host port: clock output enabled (0), phy_sus 0x1d2. */
	wm(grf0, 0x0008, 1U<<4, 0);
	wm(grf0, 0x0004, 0x1ff, 0x1d2);
	wm(grf0, 0x0080, 1U<<1, 1U<<1);
	microdelay(2000);

	combphyinit();

	/* DWC3 host mode and PHY interface. */
	v = dwc[0xc110/4];			/* GCTL */
	v &= ~(3U<<4);				/* SCALEDOWN */
	v = (v & ~(3U<<12)) | (1U<<12);		/* PRTCAPDIR_HOST */
	dwc[0xc110/4] = v;

	v = dwc[0xc200/4];			/* GUSB2PHYCFG(0) */
	v &= ~(1U<<6);				/* SUSPHY off */
	v &= ~((1U<<3)|(0xfU<<10));		/* PHYIF/USBTRDTIM */
	v |= (1U<<3)|(5U<<10);			/* UTMI wide */
	v &= ~(1U<<31);				/* PHYSOFTRST off */
	dwc[0xc200/4] = v;

	v = dwc[0xc2c0/4];			/* GUSB3PIPECTL(0) */
	v &= ~((1U<<17)|(1U<<27)|(1U<<31));
	dwc[0xc2c0/4] = v;

	coherence();
	microdelay(10000);

	dbg_gsnpsid = dwc[0xc120/4];
	dbg_gctl = dwc[0xc110/4];
	dbg_usb2 = dwc[0xc200/4];
	dbg_usb3 = dwc[0xc2c0/4];
	dbg_pmugrf = pmugrf[0x04/4];
	dbg_gpio = gpio[0];
	dbg_gpio_ddr = gpio[0x08/4];
	dbg_grf0 = grf0[0x08/4];
	dbg_grf0sus = grf0[0x04/4];
	return 0;
}

/*
 * Minimal DWC3 core wakeup.  U-Boot owns clocks/PHYs/VBUS; we only make
 * sure the core is in host mode and its UTMI/PIPE interfaces are awake
 * before the user-space xHCI engine resets the host block.
 */
static void
dwc3core(u64 base)
{
	volatile u32 *dwc = (volatile u32 *)KADDR(base);
	u32 v;

	v = dwc[0xc110/4];
	v &= ~(3U<<4);
	v = (v & ~(3U<<12)) | (1U<<12);
	dwc[0xc110/4] = v;
	coherence();
	microdelay(5000);
}

static void
vbusinit(void)
{
	volatile u32 *pmucru = (volatile u32 *)KADDR(PmucruBase);
	volatile u32 *pmugrf = (volatile u32 *)KADDR(PmuGrfBase);
	volatile u32 *gpio = (volatile u32 *)KADDR(Gpio0Base);

	/* USB2 host controller clocks (EHCI/OHCI). */
	wm((volatile u32 *)KADDR(CruBase), 0x340,
	   (1U<<0)|(1U<<1)|(1U<<2)|(1U<<12)|(1U<<13)|(1U<<14)|(1U<<15), 0);
	/* GPIO0 pclk/dbclk live in PMU_CLKGATE_CON(1), not CON(2). */
	wm(pmucru, 0x184, (1U<<9)|(1U<<10), 0);
	/* GPIO0_A6/A5, function 0, host and OTG VBUS enables. */
	wm(pmugrf, 0x04, 0xfU<<8, 0);
	wm(pmugrf, 0x04, 0xfU<<4, 0);
	gpioout(gpio, 6);
	gpioout(gpio, 5);
	microdelay(2000);
}

static void
ehciprobe(void)
{
	volatile u32 *cru = (volatile u32 *)KADDR(CruBase);
	u64 base[2] = {0xfd800000ULL, 0xfd880000ULL};
	int i;
	u32 cap, portsc;

	/* USB2 host clocks: PCLK_USB, HCLK_USB2HOST0/1 and ARB. */
	wm(cru, 0x340, (1U<<0)|(1U<<1)|(1U<<2)|(1U<<12)|(1U<<13)|
	   (1U<<14)|(1U<<15), 0);
	microdelay(1000);
	for(i = 0; i < 2; i++){
		volatile u32 *e = (volatile u32 *)KADDR(base[i]);
		volatile u32 *op;
		cap = e[0]&0xff;
		op = e + cap/4;
		op[0] = 0;			/* stop */
		microdelay(1000);
		op[0x40/4] = 1;			/* CONFIGFLAG: route ports to EHCI */
		op[0x44/4] = (1U<<12);		/* port power */
		microdelay(2000);
		portsc = op[0x44/4];
		print("xhci: ehci%d cap %ux portsc %ux\n", i, e[0], portsc);
	}
}

/*
 * OHCI companion bring-up for the RK3566 USB2 ports.  Linux's
 * ohci-platform/ohci-hcd sequence is: clocks/resets, HCR, HCFS reset,
 * HCCA/FmInterval, operational state, then root-hub port power.
 */
static void
ohciinit(u64 base, int n)
{
	volatile u32 *o = (volatile u32 *)KADDR(base);
	volatile u32 *grf1 = (volatile u32 *)KADDR(Usbgrf1Base);
	void *hcca;
	u32 rev, ctl, desc, fm, st[4] = {0};
	int i, np, j;

	/* PHY1 host/OTG power and 480 MHz output (active low clock bit). */
	wm(grf1, 0x0008, 1U<<4, 0);
	/* rk3568_phy_cfgs: PHY1 power-on = 0x1d2, suspend = 0x1d1. */
	wm(grf1, 0x0004, 0x1ff, 0x1d2);
	wm(grf1, 0x0000, 0x1ff, 0x1d2);
	wm(grf1, 0x0080, (1U<<0)|(1U<<1), (1U<<0)|(1U<<1));
	microdelay(2000);

	rev = o[0/4];
	if(rev == ~0U || rev == 0){
		print("ohci%d: not responding (%ux)\n", n, rev);
		return;
	}

	o[0x14/4] = 1U<<31;			/* interrupt disable: MIE */
	o[0x08/4] = 1;				/* HCR */
	for(i = 0; i < 100 && (o[0x08/4] & 1); i++)
		microdelay(10);
	if(o[0x08/4] & 1){
		print("ohci%d: controller reset timed out\n", n);
		return;
	}

	ctl = o[0x04/4];
	ctl &= ~(3U<<6);			/* HCFS = RESET (0) */
	o[0x04/4] = ctl;
	microdelay(50000);

	o[0x08/4] = 1;				/* HCR again */
	for(i = 0; i < 100 && (o[0x08/4] & 1); i++)
		microdelay(10);
	if(o[0x08/4] & 1){
		print("ohci%d: controller reset timed out\n", n);
		return;
	}

	fm = o[0x34/4];
	if(fm == 0 || fm == ~0U)
		fm = 0x2edf2778U;
	o[0x34/4] = fm;
	hcca = mallocalign(256, 256, 0, 0);
	if(hcca == nil){
		print("ohci%d: no HCCA memory\n", n);
		return;
	}
	memset(hcca, 0, 256);
	dcacheclean(hcca, 256);
	o[0x18/4] = PADDR(hcca);
	o[0x40/4] = (fm & 0x3fff) * 9 / 10;

	desc = o[0x48/4];
	np = desc & 0xff;
	if(np > 4)
		np = 4;
	for(j = 1; j <= np; j++)
		o[(0x54 + (j-1)*4)/4] = 1U<<8;	/* port power */
	ctl = (ctl & ~(3U<<6)) | (2U<<6);	/* HCFS = OPERATIONAL */
	o[0x04/4] = ctl;
	microdelay(10000);

	for(j = 0; j < np; j++){
		volatile u32 *ps = &o[(0x54 + j*4)/4];
		*ps = (1U<<8)|(1U<<4);		/* PPS | PRS */
		for(i = 0; i < 100 && !(*ps & (1U<<20)); i++)
			microdelay(100);
		*ps = 1U<<20;			/* clear PRSC */
		microdelay(10000);
		st[j] = *ps;
	}
	print("ohci%d: rev %ux ndp %d ctl %ux p1 %ux p2 %ux p3 %ux p4 %ux\n",
	      n, rev, np, o[0x04/4], st[0], st[1], st[2], st[3]);
}

static int
addcontroller(u64 base)
{
	Xtransport *t;
	volatile u32 *regs;

	regs = (volatile u32 *)KADDR(base);
	if(regs == nil || regs[0] == ~0U ||
	   (regs[0]&0xff) < 0x20 || (regs[0]&3) != 0)
		return -1;
	t = &controllers[ncontrollers++];
	t->regs = regs;
	t->size = Dwc3Size;
	t->op = regs[0]&0xff;
	return 0;
}

/* The second DWC3 is the USB2 OTG/host block at 0xfcc00000. */
static int
dwc3init_otg0(void)
{
	volatile u32 *cru = (volatile u32 *)KADDR(CruBase);
	volatile u32 *grf0 = (volatile u32 *)KADDR(Usbgrf0Base);
	volatile u32 *dwc = (volatile u32 *)KADDR(Otg0Base);
	u32 v;

	/* ACLK/PCLK PIPE and USB3OTG0 gates. */
	wm(cru, 0x328, (1U<<0)|(1U<<1)|(1U<<8)|(1U<<9)|(1U<<10), 0);
	/* Deassert SRST_USB3OTG0 (bit 4). */
	wm(cru, 0x424, 1U<<4, 0);
	/* USB2 PHY0 OTG port: clock output enabled (0), phy_sus power-on. */
	wm(grf0, 0x0008, 1U<<4, 0);
	wm(grf0, 0x0000, 0x1ff, 0);
	wm(grf0, 0x0080, 1U<<0, 1U<<0);
	microdelay(2000);

	v = dwc[0xc110/4];
	v &= ~(3U<<4);
	v = (v & ~(3U<<12)) | (1U<<12);
	dwc[0xc110/4] = v;
	v = dwc[0xc200/4];
	v &= ~((1U<<6)|(1U<<3)|(0xfU<<10)|(1U<<31));
	v |= (1U<<3)|(5U<<10);
	dwc[0xc200/4] = v;
	coherence();
	microdelay(5000);
	return 0;
}

static void
xreset(void)
{
	/*
	 * Initialize board power, PHYs and controller host modes before the
	 * user-space xHCI engine takes ownership. This reprograms U-Boot state.
	 */
	ncontrollers = 0;
	vbusinit();
	dwc3init();
	dwc3init_otg0();
	ohciinit(0xfd840000ULL, 0);
	ohciinit(0xfd8c0000ULL, 1);
	if(addcontroller(Dwc3Base) < 0)
		print("xhci: USB3 controller not responding\n");
	if(addcontroller(Otg0Base) < 0)
		print("xhci: OTG0 controller not responding\n");
}

/* Called with the transport locked, without holding an interrupt lock. */
static void
quiesce(Xtransport *t)
{
	int i;
	volatile u32 *op = t->regs + t->op/4;

	op[0] = 0;
	coherence();
	for(i = 0; i < 100; i++){
		if(op[1]&1)
			break;
		delay(1);
	}
	coherence();
	if(i == 100 || op[1] == ~0U)
		t->poisoned = 1;
	/* DMA is never freed, including on a failed stop. */
}

static void
xshutdown(void)
{
	int i;

	for(i = 0; i < ncontrollers; i++)
		if(controllers[i].owned)
			quiesce(&controllers[i]);
}

static int
xgen(Chan *c, char *unused, Dirtab *dt, int ndt, int s, Dir *dp)
{
	Qid q;
	char name[16];

	USED(unused); USED(dt); USED(ndt);
	if(s == DEVDOTDOT){
		mkqid(&q, 0, 0, QTDIR);
		devdir(c, q, "#X", 0, eve, DMDIR|0500, dp);
		return 1;
	}
	if(s < 0 || s >= ncontrollers)
		return -1;
	snprint(name, sizeof name, "%d", s);
	mkqid(&q, s+1, 0, QTFILE);
	devdir(c, q, name, 0, eve, 0600, dp);
	return 1;
}

static Chan *
xattach(char *spec)
{
	if(!xtrusted())
		error(Eperm);
	return devattach('X', spec);
}

static Walkqid *
xwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, nil, 0, xgen);
}

static int
xstat(Chan *c, u8 *db, int n)
{
	return devstat(c, db, n, nil, 0, xgen);
}

static Chan *
xopen(Chan *c, int mode)
{
	Proc *up = externup();
	Xtransport *t;
	u32 i;

	if(!xtrusted())
		error(Eperm);
	if(c->qid.type&QTDIR)
		return devopen(c, mode, nil, 0, xgen);
	if(openmode(mode) != ORDWR || c->qid.path < 1 ||
	   c->qid.path > (u64)ncontrollers)
		error(Eperm);
	t = &controllers[c->qid.path-1];
	if(c->qid.path == 1){
		volatile u32 *g0 = (volatile u32 *)KADDR(Usbgrf0Base);
		volatile u32 *g1 = (volatile u32 *)KADDR(Usbgrf1Base);
		volatile u32 *e0 = (volatile u32 *)KADDR(0xfd800000);
		volatile u32 *e1 = (volatile u32 *)KADDR(0xfd880000);
		u32 c0 = e0[0]&0xff, c1 = e1[0]&0xff;
		print("xhci: grf0 c0 %ux sus %ux clk %ux grf1 c0 %ux sus %ux clk %ux\n",
		      g0[0xc0/4], g0[0x04/4], g0[0x08/4],
		      g1[0xc0/4], g1[0x04/4], g1[0x08/4]);
		print("xhci: ehci0 cap %ux portsc %ux ehci1 cap %ux portsc %ux\n",
		      e0[0], e0[(c0+0x44)/4], e1[0], e1[(c1+0x44)/4]);
		volatile u32 *h0 = (volatile u32 *)KADDR(0xfd840000);
		volatile u32 *h1 = (volatile u32 *)KADDR(0xfd8c0000);
		print("xhci: ohci0 rev %ux ndp %u p1 %ux p2 %ux ohci1 rev %ux ndp %u p1 %ux p2 %ux\n",
		      h0[0], h0[0x48/4]&0xff, h0[0x54/4], h0[0x58/4],
		      h1[0], h1[0x48/4]&0xff, h1[0x54/4], h1[0x58/4]);
	}
	qlock(&t->lock);
	if(waserror()){
		qunlock(&t->lock);
		nexterror();
	}
	if(t->owned)
		error(Einuse);
	if(t->poisoned)
		error("xhci transport quarantined until reboot");
	if(t->dma == nil){
		t->dma = mallocalign(Dmasize, 65536, 0, 0);
		if(t->dma == nil)
			error(Enomem);
		t->bus = PADDR(t->dma);
		for(i = 0; i < Dmasize; i += 4096)
			if(PADDR((u8*)t->dma+i) != t->bus+i){
				free(t->dma);
				t->dma = nil;
				error("xhci requires contiguous DMA memory");
			}
	}
	t->owned = 1;
	c->aux = t;
	c->mode = ORDWR;
	c->flag |= COPEN;
	c->offset = 0;
	c->iounit = 65536;
	poperror();
	qunlock(&t->lock);
	return c;
}

static void
xclose(Chan *c)
{
	Xtransport *t;

	if(!(c->flag&COPEN) || c->qid.type&QTDIR)
		return;
	t = c->aux;
	if(t == nil)
		return;
	qlock(&t->lock);
	quiesce(t);
	t->owned = 0;
	qunlock(&t->lock);
}

static i32
xio(Chan *c, void *a, i32 n, i64 offset, int writing)
{
	Proc *up = externup();
	Xtransport *t = c->aux;
	u64 off = offset;
	u32 value;
	char info[160];

	if(t == nil || offset < 0 || n < 0)
		error(Ebadarg);
	qlock(&t->lock);
	if(waserror()){
		qunlock(&t->lock);
		nexterror();
	}
	if(!t->owned || t->poisoned)
		error(Eio);
	if(off < Regbase && !writing){
		snprint(info, sizeof info, "xhci-transport/1 %llx %x %x\n",
			t->bus, Dmasize, t->size);
		n = readstr(offset, a, n, info);
	}else if(off >= Regbase && off < Regbase+t->size){
		off -= Regbase;
		if(n != 4 || (off&3) || off > t->size-4)
			error(Ebadarg);
		if(writing){
			memmove(&value, a, 4);
			coherence();
			t->regs[off/4] = value;
			coherence();
		}else{
			value = t->regs[off/4];
			coherence();
			memmove(a, &value, 4);
		}
	}else if(off >= Dmabase){
		off -= Dmabase;
		if(n > Dmasize || off > Dmasize-n)
			error(Ebadarg);
		if(writing){
			memmove((u8*)t->dma+off, a, n);
			dcacheclean((u8*)t->dma+off, n);
		}else{
			dcacheclean((u8*)t->dma+off, n);
			memmove(a, (u8*)t->dma+off, n);
		}
	}else
		error(Ebadarg);
	poperror();
	qunlock(&t->lock);
	return n;
}

static i32
xread(Chan *c, void *a, i32 n, i64 off)
{
	if(c->qid.type&QTDIR)
		return devdirread(c, a, n, nil, 0, xgen);
	return xio(c, a, n, off, 0);
}

static i32
xwrite(Chan *c, void *a, i32 n, i64 off)
{
	if(c->qid.type&QTDIR)
		error(Eperm);
	return xio(c, a, n, off, 1);
}

Dev xhcidevtab = {
	.dc = 'X',
	.name = "xhci-transport",

	.reset = xreset,
	.init = devinit,
	.shutdown = xshutdown,

	.attach = xattach,
	.walk = xwalk,
	.stat = xstat,
	.open = xopen,
	.create = devcreate,
	.close = xclose,
	.read = xread,
	.bread = devbread,
	.write = xwrite,
	.bwrite = devbwrite,
	.remove = devremove,
	.wstat = devwstat,
};
