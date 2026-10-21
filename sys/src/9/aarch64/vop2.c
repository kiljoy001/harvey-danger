/*
 * RK3566 VOP2 bring-up.
 *
 * One 1920x1080 RGB888 plane is scanned out from an IOMMU-mapped buffer.
 * The display path is deliberately small: VOP2 VP0, Smart0 window, HDMI0
 * output.  Clocks are programmed directly in the RK3568 CRU because Harvey
 * does not have a clock framework yet.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "fdt.h"
#include "rkiommu.h"
#include "dw_hdmi_reg.h"
#include "vop2.h"

enum {
	CruBase		= 0xfdd20000,
	PmucruBase	= 0xfdd00000,
	PmuBase		= 0xfdd90000,
	GrfBase		= 0xfdc60000,
	VopBase		= 0xfe040000,
	HdmiBase	= 0xfe0a0000,

	FbWidth		= 1920,
	FbHeight	= 1080,
	FbStride	= FbWidth * 4,
	FbSize		= FbHeight * FbStride,
	FbIova		= 0x10000000ULL,

	/* VOP2 system registers */
	VopCfgDone	= 0x000,
	VopVersion	= 0x004,
	VopSysAuto	= 0x008,
	VopSysAxi	= 0x024,
	VopDspIfEn	= 0x028,
	VopDspIfCtrl	= 0x02c,
	VopDspIfPol	= 0x030,
	VopSysPd	= 0x034,
	VopOvlCtrl	= 0x600,
	VopOvlLayerSel	= 0x604,
	VopOvlPortSel	= 0x608,

	/* VP0 registers */
	Vp0DspCtrl	= 0xc00,
	Vp0ColorBar	= 0xc08,
	Vp0ClkCtrl	= 0xc0c,
	Vp0DspBg	= 0xc2c,
	Vp0PreScan	= 0xc30,
	Vp0PostHact	= 0xc34,
	Vp0PostVact	= 0xc38,
	Vp0PostSclFac	= 0xc3c,
	Vp0PostSclCtrl	= 0xc40,
	Vp0Htotal	= 0xc48,
	Vp0Hact		= 0xc4c,
	Vp0Vtotal	= 0xc50,
	Vp0Vact		= 0xc54,
	Vp0BcshCtrl	= 0xc60,
	Vp0BcshBcs	= 0xc64,
	Vp0BcshH	= 0xc68,

	/* Esmart0 window registers (test alternate primary window) */
	Smart0Ctrl0	= 0x1800,
	Smart0Ctrl1	= 0x1804,
	Smart0Region0Ctrl = 0x1810,
	Smart0YrgbMst	= 0x1814,
	Smart0Vir	= 0x181c,
	Smart0ActInfo	= 0x1820,
	Smart0DspInfo	= 0x1824,
	Smart0DspSt	= 0x1828,
	Smart0SclCtrl	= 0x1830,
	Smart0SclFac	= 0x1834,
	Smart0SclCbr	= 0x1838,
	Smart0SclOff	= 0x183c,
	SmartDly	= 0x6f8,
	Vp0BgMix	= 0x6e0,
};

static volatile u32 *cru;
static volatile u32 *pmucru;
static volatile u32 *pmu;
static volatile u32 *grf;
static volatile u32 *vop;

static void
vopwr(int reg, u32 v)
{
	vop[reg/4] = v;
}

static u32
voprd(int reg)
{
	return vop[reg/4];
}

/* Rockchip write-mask register: high 16 bits are the mask. */
static void
cruhi(int reg, u32 mask, u32 val)
{
	cru[reg/4] = (mask << 16) | (val & mask);
}

static void
pmucruhi(int reg, u32 mask, u32 val)
{
	pmucru[reg/4] = (mask << 16) | (val & mask);
}

static void
pmuhi(int reg, u32 mask, u32 val)
{
	pmu[reg/4] = (mask << 16) | (val & mask);
}

static void
pllset(int reg, u32 fbdiv, u32 postdiv1, u32 refdiv, u32 postdiv2)
{
	u32 con0, con1;

	con0 = fbdiv | (postdiv1 << 12);
	con1 = refdiv | (postdiv2 << 6) | (1 << 12); /* integer mode */

	cruhi(reg + 0x0, 0xfff | (0x7 << 12), con0);
	cruhi(reg + 0x4, 0x3f | (0x7 << 6) | (1 << 12) | (1 << 13), con1);
}

static int
hpllstart(void)
{
	int i;
	u32 v;

	/* Put HPLL in slow mode before changing its dividers, as Linux does. */
	pmucruhi(0x80, 3 << 2, 0);
	microdelay(1000);

	/* HPLL: 24 MHz * 99 / 1 / 4 / 4 = 148.5 MHz. */
	pmucruhi(0x40, 0xfff | (0x7 << 12), 99 | (4 << 12));
	pmucruhi(0x44, 0x3f | (0x7 << 6) | (1 << 12) | (1 << 13) | (1 << 15),
		 1 | (4 << 6) | (1 << 12));	/* PLLPDSEL=0 */
	microdelay(1000);
	for(i = 0; i < 100; i++){
		v = pmucru[0x44/4];
		if(v & (1 << 10))
			break;
		microdelay(1000);
	}
	pmucruhi(0x80, 3 << 2, 1 << 2);	/* normal mode */
	microdelay(1000);
	if(i == 100){
		print("vop2: hpll lock timeout, con0 %x con1 %x\n",
			pmucru[0x40/4], pmucru[0x44/4]);
		return -1;
	}
	return 0;
}

static int
vpllstart(void)
{
	int i;
	u32 v;

	/* 24 MHz * 99 / 1 / 4 / 4 = 148.5 MHz */
	pllset(0xa0, 99, 4, 1, 4);
	cruhi(0xc0, 3 << 12, 1 << 12);	/* VPLL normal mode */
	for(i = 0; i < 100; i++){
		v = cru[0xa4/4];
		if(v & (1 << 10))
			return 0;
		microdelay(1000);
	}
	print("vop2: vpll lock timeout, con0 %#ux con1 %#ux\n",
		cru[0xa0/4], cru[0xa4/4]);
	return -1;
}

static void
clocks(void)
{
	/* PD_VO is active low: clear power and idle request bits. */
	pmuhi(0xa0, 1 << 7, 0);
	pmuhi(0x50, 1 << 4, 0);

	/* ACLK_VO/HCLK_VO/PCLK_VO, ACLK_VOP_PRE/ACLK_VOP/HCLK_VOP/DCLK_VOP0 */
	cruhi(0x350, (1 << 0) | (1 << 1) | (1 << 2) |
	      (1 << 6) | (1 << 8) | (1 << 9) | (1 << 10), 0);
	/* PCLK_HDMI_HOST, CLK_HDMI_SFR, CLK_HDMI_CEC */
	cruhi(0x354, (1 << 3) | (1 << 4) | (1 << 5), 0);

	/* DCLK_VOP0 <- HPLL, divider 1.  CLKSEL_CON39 has an
	 * upper-half write mask. */
	cruhi(0x19c, (3 << 10) | 0xff, 0);

	/* HDMI reference clock <- HPLL (bit 7 clear). */
	pmucruhi(0x120, 1 << 7, 0);

	/* HDMI DDC pins are driven by the controller. */
	grf[0x364/4] = (0xc000 << 16) | 0xc000;
}

static void
resets(void)
{
	/* Deassert A_VOP, H_VOP, VOP0, P_HDMI_HOST and HDMI_HOST. */
	cruhi(0x440, (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) |
	      (1 << 14) | (1 << 15), 0);
	microdelay(1000);
}

static void
timing(Vmode *m)
{
	int htotal, vtotal, hsync_start, vsync_start;
	int hact_st, hact_end, vact_st, vact_end;
	u32 v;

	htotal = m->hactive + m->hfront + m->hsync + m->hback;
	vtotal = m->vactive + m->vfront + m->vsync + m->vback;
	hsync_start = m->hactive + m->hfront;
	vsync_start = m->vactive + m->vfront;
	hact_st = htotal - hsync_start;
	hact_end = hact_st + m->hactive;
	vact_st = vtotal - vsync_start;
	vact_end = vact_st + m->vactive;

	vopwr(Vp0Htotal, (htotal << 16) | m->hsync);
	vopwr(Vp0Hact, (hact_st << 16) | hact_end);
	vopwr(Vp0Vtotal, (vtotal << 16) | m->vsync);
	vopwr(Vp0Vact, (vact_st << 16) | vact_end);
	vopwr(Vp0PostHact, (hact_st << 16) | hact_end);
	vopwr(Vp0PostVact, (vact_st << 16) | vact_end);

	/* No post scaling. */
	vopwr(Vp0PostSclFac, 0);
	vopwr(Vp0PostSclCtrl, 0);
	/* RK3568 VP0 default pre-scan/bg delays: bg_dly=42, hsync=44. */
	vopwr(Vp0PreScan, 0x03e9002c);
	vopwr(Vp0BgMix, 42 << 24);
	vopwr(Vp0DspBg, 0x3fc00000);	/* red background for path test */
	vopwr(Vp0BcshCtrl, 0);
	vopwr(Vp0BcshBcs, 0);
	vopwr(Vp0BcshH, 0);

	/* Keep VP0 in standby until timing and mixer state are committed. */
	v = 1u << 31;
	vopwr(Vp0DspCtrl, v);
	vopwr(Vp0ClkCtrl, 0);
}

static void
window(void)
{
	u32 act, dsp;

	act = ((FbHeight - 1) << 16) | (FbWidth - 1);
	dsp = act;

	vopwr(Smart0Ctrl0, 0);		/* RGB, no CSC */
	vopwr(Smart0Ctrl1, 0);
	vopwr(Smart0Region0Ctrl, 0);	/* disabled while programming */
	vopwr(Smart0YrgbMst, (u32)FbIova);
	vopwr(Smart0Vir, FbStride / 4);
	vopwr(Smart0ActInfo, act);
	vopwr(Smart0DspInfo, dsp);
	vopwr(Smart0DspSt, 0);
	vopwr(Smart0SclCtrl, 0);
	vopwr(Smart0SclFac, 0);
	vopwr(Smart0SclCbr, 0);
	vopwr(Smart0SclOff, 0);

	/* Smart0 is layer 3.  One layer on VP0.  Make layer/port mux
	 * configuration take effect immediately, as Linux does. */
	vopwr(VopOvlLayerSel, 2);
	/* VP0 has one layer; VP1/VP2 have none (mux value 8). */
	vopwr(VopOvlPortSel, 0x880);
	vopwr(VopOvlCtrl, 1 << 28);

	/* Default window delay for Esmart0 is 20. */
	vopwr(SmartDly, 20);

	/* ARGB8888, enable Esmart0 scanout from the IOMMU-mapped buffer. */
	vopwr(Smart0Region0Ctrl, 1);
}

static void
pattern(u32 *p, int w, int h)
{
	static const u32 bars[8] = {
		0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
		0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000,
	};
	u32 c;
	int x, y, i;

	for(y = 0; y < h; y++){
		for(x = 0; x < w; x++){
			i = x * 8 / w;
			c = bars[i];
			if(y < h / 16)
				c = 0xff000000 |
				    (x * 255 / w) << 16 |
				    (x * 255 / w) << 8 |
				    (x * 255 / w);
			p[y * (FbStride/4) + x] = c;
		}
	}
}

static u64
fbmap(Rkiommu *mmu)
{
	u64 pa, iova;
	int color, i, pages;

	pa = physalloc(FbSize, &color, nil);
	if(pa != 0){
		if(rkiommumap(mmu, FbIova, pa, FbSize, 1) < 0){
			print("vop2: cannot map framebuffer iova %#llx\n", FbIova);
			physfree(pa, FbSize);
			return 0;
		}
		print("vop2: fb pa %#llx pte %#x\n", pa,
			mmu->dom->pt[(FbIova >> RkiommuDteshift) & RkiommuDtemask]
			[(FbIova >> RkiommuPteshift) & RkiommuPtemask]);
		pattern(KADDR(pa), FbWidth, FbHeight);
		dcacheclean(KADDR(pa), FbSize);
		return pa;
	}

	/* Fall back to page-at-a-time allocation. */
	pages = FbSize / 4096;
	for(i = 0; i < pages; i++){
		pa = physalloc(4096, &color, nil);
		if(pa == 0)
			return 0;
		iova = FbIova + (u64)i * 4096;
		if(rkiommumap(mmu, iova, pa, 4096, 1) < 0){
			physfree(pa, 4096);
			return 0;
		}
		memset(KADDR(pa), 0, 4096);
		dcacheclean(KADDR(pa), 4096);
	}
	return 1;
}

int
vop2init(Fdtinfo *info)
{
	Rkiommu *mmu;
	Rkdomain *dom;
	Vmode m;
	u64 fb;
	u32 ver;

	USED(info);
	mmu = rkiommubybase(0xfe043e00);
	if(mmu == nil){
		print("vop2: no VOP IOMMU\n");
		return -1;
	}

	cru = (volatile u32 *)CruBase;
	pmucru = (volatile u32 *)PmucruBase;
	pmu = (volatile u32 *)PmuBase;
	grf = (volatile u32 *)GrfBase;
	vop = (volatile u32 *)VopBase;

	clocks();
	resets();
	vopwr(VopSysAuto, 0);		/* disable auto gating */
	vopwr(0x50, 1);			/* RK3566 OTP window enable */
	ver = voprd(VopVersion);
	print("vop2: version %x\n", ver);
	if(hpllstart() < 0)
		return -1;
	print("vop2: hpll %x/%x mode %x\n",
		pmucru[0x40/4], pmucru[0x44/4], pmucru[0x80/4]);

	dom = rkiommudomain();
	if(dom == nil){
		print("vop2: no IOMMU domain\n");
		return -1;
	}
	if(rkiommuattach(mmu, dom) < 0){
		print("vop2: IOMMU attach failed\n");
		return -1;
	}
	fb = fbmap(mmu);
	if(fb == 0){
		print("vop2: no framebuffer\n");
		return -1;
	}
	if(rkiommuenable(mmu) < 0){
		print("vop2: IOMMU enable failed\n");
		return -1;
	}

	m.clock = 148500;
	m.hactive = FbWidth;
	m.hfront = 88;
	m.hsync = 44;
	m.hback = 148;
	m.vactive = FbHeight;
	m.vfront = 4;
	m.vsync = 5;
	m.vback = 36;

	timing(&m);
	window();
	vopwr(VopDspIfCtrl, 0);
	vopwr(VopDspIfPol, (1 << 28) | (3 << 4) | (1 << 7));
	vopwr(VopDspIfEn, (0 << 10) | (1 << 1));

	/* Use the hardware background as a path test. */
	vopwr(Vp0ColorBar, 0);

	/* Global config done + VP0 config done, then release standby. */
	vopwr(VopCfgDone, 0x8001);
	microdelay(20000);
	vopwr(Vp0DspCtrl, 0);
	microdelay(20000);

	print("vop2: %dx%d framebuffer at iova %#llx\n",
		FbWidth, FbHeight, FbIova);
	print("vop2: dspctrl %x ifen %x layer %x port %x stat %x\n",
		voprd(Vp0DspCtrl), voprd(VopDspIfEn),
		voprd(VopOvlLayerSel), voprd(VopOvlPortSel), voprd(0x60));
	print("vop2: smart0 ctrl %x vir %x mst %x act %x dsp %x\n",
		voprd(Smart0Region0Ctrl), voprd(Smart0Vir),
		voprd(Smart0YrgbMst), voprd(Smart0ActInfo), voprd(Smart0DspInfo));
	print("vop2: cru19c %x cru40 %x cru44 %x gates %x/%x\n",
		cru[0x19c/4], cru[0x40/4], cru[0x44/4],
		cru[0x350/4], cru[0x354/4]);
	{
		int hr;
		hr = dw_hdmi_start(HdmiBase, &m);
		microdelay(20000);
		print("vop2: hdmi start %d phy %x clkdis %x invid %x\n",
			hr, ((volatile u32 *)HdmiBase)[HDMI_PHY_STAT0],
			((volatile u32 *)HdmiBase)[HDMI_MC_CLKDIS],
			((volatile u32 *)HdmiBase)[HDMI_FC_INVIDCONF]);
	}
	print("vop2: colorbar %x dspctrl %x iommu %x/%x\n",
		voprd(Vp0ColorBar), voprd(Vp0DspCtrl),
		((volatile u32 *)mmu->base[0])[0x04/4],
		((volatile u32 *)mmu->base[0])[0x20/4]);
	print("vop2: stat0 %x", voprd(0x60));
	microdelay(50000);
	print(" stat1 %x\n", voprd(0x60));
	rkiommufault(mmu);
	return 0;
}
