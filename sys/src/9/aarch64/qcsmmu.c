/*
 * Qualcomm apps SMMU (ARM MMU-500, SMMUv2 programming model) on SM6115.
 *
 * The bootloader leaves the SMMU on with sCR0.USFCFG set, so a stream
 * with no mapping faults and its first DMA resets the phone.  It maps
 * the masters it uses itself (SDHC, USB, UFS, display) to context banks
 * whose CBAR is stage 1 with stage 2 bypass and whose SCTLR.M is clear:
 * no translation at all, bus addresses are physical.  The stock kernel
 * keeps that state (qcom,skip-init) and adds the streams it needs.
 *
 * Harvey's drivers hand devices physical addresses, so each stream in
 * streams[] is pointed at the context bank SDHC already uses, in a free
 * stream-match slot.  The GPU's SMMU has the same compatible string but
 * is unclocked at boot (reading it hangs the bus), so the node is found
 * by name.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "fdt.h"

enum {
	Idr0		= 0x020,
	Idr1		= 0x024,
	Smr		= 0x800,
	S2cr		= 0xc00,

	Cbar		= 0x000,	/* global register space 1 */
	Cbsctlr		= 0x000,	/* context bank */

	Smrvalid	= 1u<<31,
	Cbarbypass	= 1<<16,	/* stage 1, stage 2 bypass */
	Sctlrm		= 1<<0,

	Sdhcstream	= 0xc0,		/* mapped by the bootloader */
};

/* Streams Harvey's drivers use DMA on, beyond the bootloader's. */
static struct {
	u16	id;
	u16	mask;
	char	*who;
} streams[] = {
	{ 0xe3,  0x0, "qupv3_0 SE DMA (touch SPI)" },
	{ 0x1a0, 0x1, "icnss / WCN3990 Wi-Fi" },
	{ 0x140, 0x0, "IPA v4.2 AP" },
	{ 0x400, 0x0, "CAMSS TFE DMA" },
};

static volatile u32 *smmu;
static u32 pagesz, numpage;

static u32
gr0(u32 reg)
{
	return smmu[reg/4];
}

static void
gr0w(u32 reg, u32 v)
{
	smmu[reg/4] = v;
	coherence();
}

int
qcsmmuinit(Fdtinfo *info)
{
	Fdtdev *d;
	u32 idr1, nsmr, ncb, smr, cb, i, s, freeslot;
	int n;

	smmu = nil;
	for(n = 0; n < info->ndev; n++){
		d = &info->dev[n];
		if(strncmp(d->name, "apps-smmu@", 10) == 0 &&
		   strcmp(d->compat, "qcom,qsmmu-v500") == 0){
			smmu = (volatile u32 *)d->base;
			break;
		}
	}
	if(smmu == nil)
		return -1;
	nsmr = gr0(Idr0) & 0xff;
	idr1 = gr0(Idr1);
	ncb = idr1 & 0xff;
	pagesz = (idr1 & (1u<<31)) ? 0x10000 : 0x1000;
	numpage = 1 << (((idr1 >> 28) & 7) + 1);

	/* The context bank the bootloader gave SDHC, if it is a bypass one. */
	cb = ~0;
	for(i = 0; i < nsmr; i++){
		smr = gr0(Smr + 4*i);
		if((smr & Smrvalid) && (smr & 0x7fff) == Sdhcstream &&
		   (gr0(S2cr + 4*i) & (3<<16)) == 0){
			cb = gr0(S2cr + 4*i) & 0xff;
			break;
		}
	}
	if(cb >= ncb || (smmu[(pagesz + Cbar + 4*cb)/4] & (3<<16)) != Cbarbypass ||
	   (smmu[((numpage + cb)*pagesz + Cbsctlr)/4] & Sctlrm) != 0){
		print("qcsmmu: no untranslated context bank; streams left unmapped\n");
		return -1;
	}

	for(s = 0; s < nelem(streams); s++){
		freeslot = ~0;
		for(i = 0; i < nsmr; i++){
			smr = gr0(Smr + 4*i);
			if((smr & Smrvalid) == 0){
				if(freeslot == ~0)
					freeslot = i;
				continue;
			}
			if((smr & 0x7fff) == streams[s].id)
				break;
		}
		if(i < nsmr)
			continue;	/* already mapped */
		if(freeslot == ~0){
			print("qcsmmu: no free stream match for %#ux\n", streams[s].id);
			return -1;
		}
		/* S2CR before the SMR goes valid, so the stream never sees a stale one. */
		gr0w(S2cr + 4*freeslot, cb);
		gr0w(Smr + 4*freeslot, Smrvalid | ((u32)streams[s].mask << 16) | streams[s].id);
		print("qcsmmu: stream %#ux/%#ux (%s) slot %ud cb %ud\n",
			streams[s].id, streams[s].mask, streams[s].who, freeslot, cb);
	}
	return 0;
}
