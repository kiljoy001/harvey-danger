/*
 * Minimal DesignWare HDMI TX bring-up for the RK3566.
 *
 * This is the fixed-mode subset of the U-Boot dw_hdmi driver: no audio,
 * no EDID, no DDC and no infoframes.  The output is RGB888, 8 bpc.
 * It is enough to bring up the VOP2 scanout path and later to add a real
 * display driver on top.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "dw_hdmi_reg.h"
#include "vop2.h"

typedef struct Hdmi Hdmi;
struct Hdmi {
	u64 base;
	int clock;
};

static u32
hdmird(Hdmi *h, int reg)
{
	return ((volatile u32 *)h->base)[reg];
}

static void
hdmiwr(Hdmi *h, int reg, u32 v)
{
	((volatile u32 *)h->base)[reg] = v;
}

static void
hdmimod(Hdmi *h, int reg, u32 mask, u32 v)
{
	hdmiwr(h, reg, (hdmird(h, reg) & ~mask) | (v & mask));
}

static void
phywrite(Hdmi *h, u32 data, u32 addr)
{
	int i;

	hdmiwr(h, HDMI_IH_I2CMPHY_STAT0, 0xff);
	hdmiwr(h, HDMI_PHY_I2CM_ADDRESS_ADDR, addr);
	hdmiwr(h, HDMI_PHY_I2CM_DATAO_1_ADDR, data >> 8);
	hdmiwr(h, HDMI_PHY_I2CM_DATAO_0_ADDR, data);
	hdmiwr(h, HDMI_PHY_I2CM_OPERATION_ADDR,
	       HDMI_PHY_I2CM_OPERATION_ADDR_WRITE);
	for(i = 0; i < 1000; i++){
		if(hdmird(h, HDMI_IH_I2CMPHY_STAT0) & 0x3){
			hdmiwr(h, HDMI_IH_I2CMPHY_STAT0,
			       hdmird(h, HDMI_IH_I2CMPHY_STAT0));
			return;
		}
		microdelay(100);
	}
	print("hdmi: phy i2c timeout addr %#ux\n", addr);
}

static void
phypower(Hdmi *h, int on)
{
	hdmimod(h, HDMI_PHY_CONF0, HDMI_PHY_CONF0_PDZ_MASK,
		on << HDMI_PHY_CONF0_PDZ_OFFSET);
}

static void
phytmds(Hdmi *h, int on)
{
	hdmimod(h, HDMI_PHY_CONF0, HDMI_PHY_CONF0_ENTMDS_MASK,
		on << HDMI_PHY_CONF0_ENTMDS_OFFSET);
}

static void
physpare(Hdmi *h, int on)
{
	hdmimod(h, HDMI_PHY_CONF0, HDMI_PHY_CONF0_SPARECTRL_MASK,
		on << HDMI_PHY_CONF0_SPARECTRL_OFFSET);
}

static void
phypddq(Hdmi *h, int on)
{
	hdmimod(h, HDMI_PHY_CONF0, HDMI_PHY_CONF0_GEN2_PDDQ_MASK,
		on << HDMI_PHY_CONF0_GEN2_PDDQ_OFFSET);
}

static void
phytxpwr(Hdmi *h, int on)
{
	hdmimod(h, HDMI_PHY_CONF0, HDMI_PHY_CONF0_GEN2_TXPWRON_MASK,
		on << HDMI_PHY_CONF0_GEN2_TXPWRON_OFFSET);
}

static void
phydatapol(Hdmi *h, int on)
{
	hdmimod(h, HDMI_PHY_CONF0, HDMI_PHY_CONF0_SELDATAENPOL_MASK,
		on << HDMI_PHY_CONF0_SELDATAENPOL_OFFSET);
}

static void
phyinterface(Hdmi *h, int on)
{
	hdmimod(h, HDMI_PHY_CONF0, HDMI_PHY_CONF0_SELDIPIF_MASK,
		on << HDMI_PHY_CONF0_SELDIPIF_OFFSET);
}

static int
phyconfigure(Hdmi *h, int clock)
{
	int i, spin;
	u32 st;

	/* power down before reconfiguring the PLL */
	phytxpwr(h, 0);
	phypddq(h, 1);

	hdmiwr(h, HDMI_MC_PHYRSTZ, HDMI_MC_PHYRSTZ_DEASSERT);
	hdmiwr(h, HDMI_MC_PHYRSTZ, HDMI_MC_PHYRSTZ_ASSERT);
	hdmiwr(h, HDMI_MC_HEACPHY_RST, HDMI_MC_HEACPHY_RST_ASSERT);

	hdmimod(h, HDMI_PHY_TST0, HDMI_PHY_TST0_TSTCLR_MASK,
		1 << HDMI_PHY_TST0_TSTCLR_OFFSET);
	hdmiwr(h, HDMI_PHY_I2CM_SLAVE_ADDR,
	       HDMI_PHY_I2CM_SLAVE_ADDR_PHY_GEN2);
	hdmimod(h, HDMI_PHY_TST0, HDMI_PHY_TST0_TSTCLR_MASK, 0);

	/* Use the Linux/Rockchip 8-bpc table. */
	if(clock <= 74250){
		phywrite(h, 0x8009, PHY_OPMODE_PLLCFG);
		phywrite(h, 0x0004, PHY_PLLGMPCTRL);
		phywrite(h, 0x0272, PHY_VLEVCTRL);
		phywrite(h, 0x802b, PHY_CKSYMTXCTRL);
		phywrite(h, 0x0004, PHY_TXTERM);
	}else if(clock <= 165000){
		/* Orange Pi 3B phy-table at 165 MHz: 0x800b/0x0000/0x026d. */
		phywrite(h, 0x0051, PHY_OPMODE_PLLCFG);
		phywrite(h, 0x0002, PHY_PLLGMPCTRL);
		phywrite(h, 0x026d, PHY_VLEVCTRL);
		phywrite(h, 0x800b, PHY_CKSYMTXCTRL);
		phywrite(h, 0x0000, PHY_TXTERM);
	}else{
		phywrite(h, 0x0040, PHY_OPMODE_PLLCFG);
		phywrite(h, 0x0003, PHY_PLLGMPCTRL);
		phywrite(h, 0x028d, PHY_VLEVCTRL);
		phywrite(h, 0x8039, PHY_CKSYMTXCTRL);
		phywrite(h, 0x0005, PHY_TXTERM);
	}
	phywrite(h, 0x0000, PHY_PLLCURRCTRL);
	phywrite(h, 0x0000, PHY_PLLPHBYCTRL);
	phywrite(h, 0x0006, PHY_PLLCLKBISTPHASE);
	phywrite(h, 0x8000, PHY_CKCALCTRL);

	phypower(h, 1);
	phytmds(h, 0);
	phytmds(h, 1);
	phytxpwr(h, 1);
	phypddq(h, 0);
	physpare(h, 1);

	for(spin = 0; spin < 50; spin++){
		st = hdmird(h, HDMI_PHY_STAT0);
		if(st & HDMI_PHY_TX_PHY_LOCK)
			return 0;
		microdelay(1000);
	}
	return -1;
}

static void
phyinit(Hdmi *h)
{
	hdmiwr(h, HDMI_PHY_I2CM_INT_ADDR, HDMI_PHY_I2CM_INT_ADDR_DONE_POL);
	hdmiwr(h, HDMI_PHY_I2CM_CTLINT_ADDR,
	       HDMI_PHY_I2CM_CTLINT_ADDR_NAC_POL |
	       HDMI_PHY_I2CM_CTLINT_ADDR_ARBITRATION_POL);
	hdmiwr(h, HDMI_PHY_MASK0, ~HDMI_PHY_HPD);
	hdmiwr(h, HDMI_IH_PHY_STAT0, HDMI_IH_PHY_STAT0_HPD);
}

static void
avcomposer(Hdmi *h, Vmode *m)
{
	u32 v;
	int hbl, vbl;

	hbl = m->hfront + m->hsync + m->hback;
	vbl = m->vfront + m->vsync + m->vback;

	v = HDMI_FC_INVIDCONF_HDCP_KEEPOUT_INACTIVE;
	v |= HDMI_FC_INVIDCONF_VSYNC_IN_POLARITY_ACTIVE_HIGH;
	v |= HDMI_FC_INVIDCONF_HSYNC_IN_POLARITY_ACTIVE_HIGH;
	v |= HDMI_FC_INVIDCONF_DE_IN_POLARITY_ACTIVE_HIGH;
	v |= HDMI_FC_INVIDCONF_DVI_MODEZ_HDMI_MODE;
	v |= HDMI_FC_INVIDCONF_R_V_BLANK_IN_OSC_ACTIVE_LOW;
	v |= HDMI_FC_INVIDCONF_IN_I_P_PROGRESSIVE;
	hdmiwr(h, HDMI_FC_INVIDCONF, v);

	hdmiwr(h, HDMI_FC_INHACTV1, m->hactive >> 8);
	hdmiwr(h, HDMI_FC_INHACTV0, m->hactive);
	hdmiwr(h, HDMI_FC_INVACTV1, m->vactive >> 8);
	hdmiwr(h, HDMI_FC_INVACTV0, m->vactive);
	hdmiwr(h, HDMI_FC_INHBLANK1, hbl >> 8);
	hdmiwr(h, HDMI_FC_INHBLANK0, hbl);
	hdmiwr(h, HDMI_FC_INVBLANK, vbl);
	hdmiwr(h, HDMI_FC_HSYNCINDELAY1, m->hfront >> 8);
	hdmiwr(h, HDMI_FC_HSYNCINDELAY0, m->hfront);
	hdmiwr(h, HDMI_FC_VSYNCINDELAY, m->vfront);
	hdmiwr(h, HDMI_FC_HSYNCINWIDTH1, m->hsync >> 8);
	hdmiwr(h, HDMI_FC_HSYNCINWIDTH0, m->hsync);
	hdmiwr(h, HDMI_FC_VSYNCINWIDTH, m->vsync);
}

static void
videosample(Hdmi *h)
{
	hdmiwr(h, HDMI_TX_INVID0, HDMI_TX_INVID0_INTERNAL_DE_GENERATOR_DISABLE |
	       (0x01 << HDMI_TX_INVID0_VIDEO_MAPPING_OFFSET));
	hdmiwr(h, HDMI_TX_INSTUFFING,
	       HDMI_TX_INSTUFFING_BDBDATA_STUFFING_ENABLE |
	       HDMI_TX_INSTUFFING_RCRDATA_STUFFING_ENABLE |
	       HDMI_TX_INSTUFFING_GYDATA_STUFFING_ENABLE);
	hdmiwr(h, HDMI_TX_GYDATA0, 0);
	hdmiwr(h, HDMI_TX_GYDATA1, 0);
	hdmiwr(h, HDMI_TX_RCRDATA0, 0);
	hdmiwr(h, HDMI_TX_RCRDATA1, 0);
	hdmiwr(h, HDMI_TX_BCBDATA0, 0);
	hdmiwr(h, HDMI_TX_BCBDATA1, 0);
}

static void
videopacketize(Hdmi *h)
{
	u32 v;

	hdmiwr(h, HDMI_VP_PR_CD, 0);
	hdmimod(h, HDMI_VP_STUFF, HDMI_VP_STUFF_PR_STUFFING_MASK,
		HDMI_VP_STUFF_PR_STUFFING_STUFFING_MODE);
	hdmimod(h, HDMI_VP_CONF,
		HDMI_VP_CONF_PR_EN_MASK | HDMI_VP_CONF_BYPASS_SELECT_MASK,
		HDMI_VP_CONF_PR_EN_DISABLE |
		HDMI_VP_CONF_BYPASS_SELECT_VID_PACKETIZER);
	hdmimod(h, HDMI_VP_STUFF, HDMI_VP_STUFF_IDEFAULT_PHASE_MASK,
		1 << HDMI_VP_STUFF_IDEFAULT_PHASE_OFFSET);
	hdmiwr(h, HDMI_VP_REMAP, HDMI_VP_REMAP_YCC422_16BIT);

	v = HDMI_VP_CONF_BYPASS_EN_ENABLE |
	    HDMI_VP_CONF_PP_EN_DISABLE |
	    HDMI_VP_CONF_YCC422_EN_DISABLE;
	hdmimod(h, HDMI_VP_CONF, HDMI_VP_CONF_BYPASS_EN_MASK |
		HDMI_VP_CONF_PP_EN_ENMASK | HDMI_VP_CONF_YCC422_EN_MASK, v);
	hdmimod(h, HDMI_VP_STUFF, HDMI_VP_STUFF_PP_STUFFING_MASK |
		HDMI_VP_STUFF_YCC422_STUFFING_MASK,
		HDMI_VP_STUFF_PP_STUFFING_STUFFING_MODE |
		HDMI_VP_STUFF_YCC422_STUFFING_STUFFING_MODE);
	hdmimod(h, HDMI_VP_CONF, HDMI_VP_CONF_OUTPUT_SELECTOR_MASK,
		HDMI_VP_CONF_OUTPUT_SELECTOR_BYPASS);
}

static void
videopath(Hdmi *h)
{
	u32 clkdis;

	hdmiwr(h, HDMI_FC_CTRLDUR, 12);
	hdmiwr(h, HDMI_FC_EXCTRLDUR, 32);
	hdmiwr(h, HDMI_FC_EXCTRLSPAC, 1);
	hdmiwr(h, HDMI_FC_CH0PREAM, 0x0b);
	hdmiwr(h, HDMI_FC_CH1PREAM, 0x16);
	hdmiwr(h, HDMI_FC_CH2PREAM, 0x21);
	hdmiwr(h, HDMI_MC_FLOWCTRL,
	       HDMI_MC_FLOWCTRL_FEED_THROUGH_OFF_CSC_BYPASS);

	clkdis = 0x7f & ~HDMI_MC_CLKDIS_PIXELCLK_DISABLE;
	hdmiwr(h, HDMI_MC_CLKDIS, clkdis);
	clkdis &= ~HDMI_MC_CLKDIS_TMDSCLK_DISABLE;
	hdmiwr(h, HDMI_MC_CLKDIS, clkdis);
}

static void
clearoverflow(Hdmi *h)
{
	u32 v;
	int i;

	hdmiwr(h, HDMI_MC_SWRSTZ, (u8)~HDMI_MC_SWRSTZ_TMDSSWRST_REQ);
	v = hdmird(h, HDMI_FC_INVIDCONF);
	for(i = 0; i < 4; i++)
		hdmiwr(h, HDMI_FC_INVIDCONF, v);
}

static void
csc(Hdmi *h)
{
	/* RGB in, RGB out: default unity coefficients, 24 bpp. */
	hdmiwr(h, HDMI_CSC_CFG, 0);
	hdmimod(h, HDMI_CSC_SCALE, HDMI_CSC_SCALE_CSC_COLORDE_PTH_MASK,
		HDMI_CSC_SCALE_CSC_COLORDE_PTH_24BPP);
	hdmiwr(h, HDMI_CSC_COEF_A1_MSB, 0x20);
	hdmiwr(h, HDMI_CSC_COEF_A1_LSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_A2_MSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_A2_LSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_A3_MSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_A3_LSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_A4_MSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_A4_LSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_B1_MSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_B1_LSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_B2_MSB, 0x20);
	hdmiwr(h, HDMI_CSC_COEF_B2_LSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_B3_MSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_B3_LSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_B4_MSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_B4_LSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_C1_MSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_C1_LSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_C2_MSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_C2_LSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_C3_MSB, 0x20);
	hdmiwr(h, HDMI_CSC_COEF_C3_LSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_C4_MSB, 0x00);
	hdmiwr(h, HDMI_CSC_COEF_C4_LSB, 0x00);
}

int
dw_hdmi_start(u64 base, Vmode *m)
{
	Hdmi h;
	u32 ih;
	int i;

	h.base = base;
	h.clock = m->clock;

	ih = HDMI_IH_MUTE_MUTE_WAKEUP_INTERRUPT |
	     HDMI_IH_MUTE_MUTE_ALL_INTERRUPT;
	hdmiwr(&h, HDMI_IH_MUTE, ih);
	hdmiwr(&h, HDMI_I2CM_INT, ~0x04);
	hdmiwr(&h, HDMI_I2CM_CTLINT, ~0x44);
	phyinit(&h);

	avcomposer(&h, m);

	// The PHY sequence is specified to be run twice.
	for(i = 0; i < 2; i++){
		phydatapol(&h, 1);
		phyinterface(&h, 0);
		phytmds(&h, 0);
		phypower(&h, 0);
		if(phyconfigure(&h, m->clock) < 0){
			print("hdmi: phy pll did not lock at %d kHz\n", m->clock);
			return -1;
		}
	}

	videopath(&h);
	videopacketize(&h);
	csc(&h);
	videosample(&h);
	clearoverflow(&h);
	return 0;
}
