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
	CruBase		= 0xfdd00000,
	PmucruBase	= 0xfdd20000,
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

	/* Smart0 window registers */
	Smart0Ctrl0	= 0x1c00,
	Smart0Ctrl1	= 0x1c04,
	Smart0Region0Ctrl = 0x1c10,
	Smart0YrgbMst	= 0x1c14,
	Smart0Vir	= 0x1c1c,
	Smart0ActInfo	= 0x1c20,
	Smart0DspInfo	= 0x1c24,
	Smart0DspSt	= 0x1c28,
	Smart0SclCtrl	= 0x1c30,
	Smart0SclFac	= 0x1c34,
	Smart0SclCbr	= 0x1c38,
	Smart0SclOff	= 0x1c3c,
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

	/* HPLL: 24 MHz * 99 / 1 / 4 / 4 = 148.5 MHz. */
	pmucruhi(0x40, 0xfff | (0x7 << 12), 99 | (4 << 12));
	pmucruhi(0x44, 0x3f | (0x7 << 6) | (1 << 12) | (1 << 13),
		 1 | (4 << 6) | (1 << 12));
	pmucruhi(0x80, 3 << 2, 1 << 2);	/* normal mode */
	for(i = 0; i < 100; i++){
		v = pmucru[0x44/4];
		if(v & (1 << 10))
			return 0;
		microdelay(1000);
	}
	print("vop2: hpll lock timeout, con0 %x con1 %x\n",
		pmucru[0x40/4], pmucru[0x44/4]);
	return -1;
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

	/* DCLK_VOP0 <- HPLL, divider 1.  HPLL is programmed to 148.5 MHz
	 * below; HDMI requires this parent per the RK3568 clock rules. */
	cruhi(0x19c, (3 << 10) | 0xff, 0);

	/* HDMI reference clock <- xin24m. */
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
	vopwr(Vp0PreScan, 0);
	vopwr(Vp0DspBg, 0);
	vopwr(Vp0BcshCtrl, 0);
	vopwr(Vp0BcshBcs, 0);
	vopwr(Vp0BcshH, 0);

	v = 0;
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

	/* Smart0 is layer 3.  One layer on VP0. */
	vopwr(VopOvlLayerSel, 3);
	vopwr(VopOvlPortSel, 0x700);
	vopwr(VopOvlCtrl, 0);

	/* ARGB8888, enable. */
	vopwr(Smart0Region0Ctrl, 1);
}

static void
pattern(u32 *p, int w, int h)
{
	static const u32 bars[8] = {
		0x00ffffff, 0x00ffff00, 0x0000ffff, 0x0000ff00,
		0x00ff00ff, 0x00ff0000, 0x000000ff, 0x00000000,
	};
	u32 c;
	int x, y, i;

	for(y = 0; y < h; y++){
		for(x = 0; x < w; x++){
			i = x * 8 / w;
			c = bars[i];
			if(y < h / 16)
				c = (x * 255 / w) << 16 |
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
		pattern(KADDR(pa), FbWidth, FbHeight);
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
	ver = voprd(VopVersion);
	print("vop2: version %x\n", ver);
	if(hpllstart() < 0)
		return -1;

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
	vopwr(VopDspIfPol, 1 << 7);
	vopwr(VopDspIfEn, (0 << 10) | (1 << 1));

	/* Keep the VOP color-bar generator available as a path test. */
	vopwr(Vp0ColorBar, 1);

	/* Global config done + VP0 config done. */
	vopwr(VopCfgDone, 0x8001);
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
	return 0;
}
