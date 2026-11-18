/*
 * adrenofs - userspace Adreno 610 (SM6115 / Bengal) GPU driver via #P + drvkit.
 *
 * Brings up GPUCC GDSCs and clocks, inspects GPU SMMU-500 (kgsl-iommu),
 * initializes A610 RBBM/GBIF/UCHE/CP registers, loads a630_sqe.fw, and
 * exposes /srv/adreno (ctl, status, regs).
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"
#include "a630_sqe_fw.h"

enum {
	/* GPUCC offsets (base 0x05990000) */
	GpuccGxCxoClk	= 0x1060,
	GpuccGxGfx3dClk	= 0x1054,
	GpuccGxGdsc	= 0x100c,
	GpuccCxGdsc	= 0x106c,
	GpuccAhbClk	= 0x1078,
	GpuccCxoAonClk	= 0x1004,
	GpuccCxGmuClk	= 0x1098,
	GpuccCxoClk	= 0x109c,
	GpuccCxGfx3dClk	= 0x10a4,
	GpuccHlosSmmu	= 0x5000,

	GdscSwCollapse	= 1<<0,
	GdscPwrOn	= 1U<<31,
	ClkEnable	= 1<<0,
	ClkOff		= 1U<<31,

	/* SMMU-500 offsets (base 0x059a0000) */
	SmmuScr0	= 0x0000,
	SmmuIdr0	= 0x0020,
	SmmuCb0Base	= 0x8000,
	SmmuCbSctlr	= 0x0000,
	SmmuCbTtbr0Lo	= 0x0020,
	SmmuCbTtbr0Hi	= 0x0024,

	/* A6xx dword register indices (byte offset = idx << 2) */
	RegRbbmClientQos	= 0x0011,
	RegRbbmInt0Mask		= 0x0038,
	RegRbbmClockCntl	= 0x00ae,
	RegRbbmClockCntlSp0	= 0x00b0,
	RegRbbmClockCntl2Sp0	= 0x00b4,
	RegRbbmClockCntlTp0	= 0x00c0,
	RegRbbmClockCntl2Tp0	= 0x00c4,
	RegRbbmClockCntl3Tp0	= 0x00c8,
	RegRbbmClockCntl4Tp0	= 0x00cc,
	RegRbbmClockCntlRb0	= 0x00f0,
	RegRbbmClockCntl2Rb0	= 0x00f4,
	RegRbbmClockCntlCcu0	= 0x00f8,
	RegRbbmClockCntlRac	= 0x0104,
	RegRbbmClockCntl2Rac	= 0x0105,
	RegRbbmClockCntlUche	= 0x010b,
	RegRbbmInt0Status	= 0x0201,
	RegRbbmStatus		= 0x0210,
	RegRbbmStatus3		= 0x0213,
	RegRbbmIsdbCnt		= 0x0533,
	RegCpRbBase		= 0x0800,
	RegCpRbBaseHi		= 0x0801,
	RegCpRbCntl		= 0x0802,
	RegCpRbRptrAddrLo	= 0x0804,
	RegCpRbRptrAddrHi	= 0x0805,
	RegCpRbRptr		= 0x0806,
	RegCpRbWptr		= 0x0807,
	RegCpSqeCntl		= 0x0808,
	RegCpSqeInstrBaseLo	= 0x0830,
	RegCpSqeInstrBaseHi	= 0x0831,
	RegCpRoqThresholds1	= 0x08c1,
	RegCpRoqThresholds2	= 0x08c2,
	RegCpMemPoolSize	= 0x08c3,
	RegCpMemPoolDbgAddr	= 0x090e,
	RegUcheCacheWays	= 0x0e17,
	RegUcheFilterCntl	= 0x0e18,
	RegGbifQsbSide0		= 0x3c03,
	RegGbifQsbSide1		= 0x3c04,
	RegGbifQsbSide2		= 0x3c05,
	RegGbifQsbSide3		= 0x3c06,
	RegGbifHalt		= 0x3c45,
	RegRbbmSecvidTrustCntl	= 0xf400,

	/* PM4 Type 7 opcodes */
	CpNop		= 0x10,
	CpMemWrite	= 0x3d,
	CpMeInit	= 0x48,

	RbSize		= 32 * 1024,
	RbDwords	= RbSize / 4,
	/* BUFSZ = log2(32768/8)=12, BLKSZ = log2(32/8)=2 */
	RbCntlDefault	= (2<<8) | 12,
};

typedef struct Adreno Adreno;
struct Adreno {
	Drv	gpu;
	Drv	gpucc;
	Drv	iommu;
	Drv	gcc;
	int	hasgpu;
	int	hasgpucc;
	int	hasiommu;
	int	hasgcc;
	int	powered;
	int	sqeok;
	u32int	rbbm_status;
	u32int	rbbm_int0;
	u32int	trust_before;
	u32int	trust_after;
	u32int	smmu_scr0;
	u32int	smmu_idr0;
	u32int	smmu_cb0_sctlr;
	u32int	rb_rptr;
	u32int	rb_wptr;
	u32int	fence_val;
	char	why[256];
};

static Adreno adreno;

static u32int
grd(Adreno *a, u32int reg)
{
	return drd(&a->gpu, reg << 2);
}

static void
gwr(Adreno *a, u32int reg, u32int v)
{
	drw(&a->gpu, reg << 2, v);
}

static uint
oddparity(uint v)
{
	uint p = 1;
	while(v != 0){
		p ^= (v & 1);
		v >>= 1;
	}
	return p;
}

static u32int
pkt7(uint opcode, uint cnt)
{
	return (7U<<28) | (cnt & 0x3fffU) | (oddparity(cnt)<<15) |
	       ((opcode & 0x7fU)<<16) | (oddparity(opcode)<<23);
}

static int
gdsc_enable(Drv *gpucc, u32int off)
{
	u32int v;
	int i;

	v = drd(gpucc, off);
	if(v & GdscSwCollapse){
		drw(gpucc, off, v & ~GdscSwCollapse);
		for(i = 0; i < 1000; i++){
			v = drd(gpucc, off);
			if(v & GdscPwrOn)
				return 0;
			sleep(1);
		}
		return -1;
	}
	return 0;
}

static void
clk_enable(Drv *gpucc, u32int off)
{
	u32int v;
	int i;

	v = drd(gpucc, off);
	if((v & ClkEnable) == 0){
		drw(gpucc, off, v | ClkEnable);
		for(i = 0; i < 200; i++){
			if((drd(gpucc, off) & ClkOff) == 0)
				break;
			sleep(1);
		}
	}
}

static int
power_on(Adreno *a)
{
	if(!a->hasgpucc || !a->hasgpu){
		snprint(a->why, sizeof a->why, "missing gpu or gpucc");
		return -1;
	}
	if(gdsc_enable(&a->gpucc, GpuccCxGdsc) < 0){
		snprint(a->why, sizeof a->why, "GPU_CX_GDSC timeout (0x%ux)",
		        drd(&a->gpucc, GpuccCxGdsc));
		return -1;
	}
	if(gdsc_enable(&a->gpucc, GpuccGxGdsc) < 0){
		snprint(a->why, sizeof a->why, "GPU_GX_GDSC timeout (0x%ux)",
		        drd(&a->gpucc, GpuccGxGdsc));
		return -1;
	}
	clk_enable(&a->gpucc, GpuccCxoAonClk);
	clk_enable(&a->gpucc, GpuccCxoClk);
	clk_enable(&a->gpucc, GpuccGxCxoClk);
	clk_enable(&a->gpucc, GpuccAhbClk);
	clk_enable(&a->gpucc, GpuccCxGmuClk);
	clk_enable(&a->gpucc, GpuccCxGfx3dClk);
	clk_enable(&a->gpucc, GpuccGxGfx3dClk);
	clk_enable(&a->gpucc, GpuccHlosSmmu);
	a->powered = 1;
	return 0;
}

static void
probe_regs(Adreno *a)
{
	if(a->hasiommu){
		a->smmu_scr0 = drd(&a->iommu, SmmuScr0);
		a->smmu_idr0 = drd(&a->iommu, SmmuIdr0);
		a->smmu_cb0_sctlr = drd(&a->iommu, SmmuCb0Base + SmmuCbSctlr);
	}
	if(a->hasgpu && a->powered){
		a->rbbm_status = grd(a, RegRbbmStatus);
		a->rbbm_int0 = grd(a, RegRbbmInt0Status);
		a->trust_before = grd(a, RegRbbmSecvidTrustCntl);
		a->rb_rptr = grd(a, RegCpRbRptr);
		a->rb_wptr = grd(a, RegCpRbWptr);
	}
}

static void
a610_hw_init(Adreno *a)
{
	/* A610 GBIF/VBIF QoS */
	gwr(a, RegGbifQsbSide0, 0x00071620);
	gwr(a, RegGbifQsbSide1, 0x00071620);
	gwr(a, RegGbifQsbSide2, 0x00071620);
	gwr(a, RegGbifQsbSide3, 0x00071620);
	gwr(a, RegRbbmClientQos, 0x3);
	gwr(a, RegGbifHalt, 0x0);

	/* A610 HWCG (skipping GMU_GX registers per A610 errata) */
	gwr(a, RegRbbmClockCntlSp0, 0x22222222);
	gwr(a, RegRbbmClockCntl2Sp0, 0x02222220);
	gwr(a, RegRbbmClockCntlTp0, 0x22222222);
	gwr(a, RegRbbmClockCntl2Tp0, 0x22222222);
	gwr(a, RegRbbmClockCntl3Tp0, 0x22222222);
	gwr(a, RegRbbmClockCntl4Tp0, 0x00022222);
	gwr(a, RegRbbmClockCntlRb0, 0x22222222);
	gwr(a, RegRbbmClockCntl2Rb0, 0x01202222);
	gwr(a, RegRbbmClockCntlCcu0, 0x00002220);
	gwr(a, RegRbbmClockCntlRac, 0x05522022);
	gwr(a, RegRbbmClockCntl2Rac, 0x00005555);
	gwr(a, RegRbbmClockCntlUche, 0x22222222);
	gwr(a, RegRbbmIsdbCnt, 0x00000182);
	gwr(a, RegRbbmClockCntl, 0xaaa8aa82);

	/* UCHE & CP A610 sizing */
	gwr(a, RegUcheFilterCntl, 0x804);
	gwr(a, RegUcheCacheWays, 0x4);
	gwr(a, RegCpRoqThresholds2, 0x00800060);
	gwr(a, RegCpRoqThresholds1, 0x40201b16);
	gwr(a, RegCpMemPoolSize, 48);
	gwr(a, RegCpMemPoolDbgAddr, 47);
}

static int
sqe_test(Adreno *a)
{
	u32int *sqe, *rb, *memptrs;
	u64int sqepa, rbpa, mempa, fencepa;
	uint wptr = 0;
	int i;

	a->sqeok = 0;
	if(!a->powered && power_on(a) < 0)
		return -1;

	sqe = drvdma(&a->gpu, 64*1024, 4096, &sqepa);
	rb = drvdma(&a->gpu, RbSize, 4096, &rbpa);
	memptrs = drvdma(&a->gpu, 4096, 4096, &mempa);
	if(sqe == nil || rb == nil || memptrs == nil){
		snprint(a->why, sizeof a->why, "drvdma failed: %r");
		return -1;
	}

	memmove(sqe, a630_sqe_fw, sizeof a630_sqe_fw);
	memset(rb, 0, RbSize);
	memset(memptrs, 0, 4096);
	fencepa = mempa + 64;

	drvdmacache(DrvClean, sqepa, sizeof a630_sqe_fw);
	drvdmacache(DrvClean, mempa, 4096);

	a610_hw_init(a);

	/* Program RB0 and SQE microcode */
	gwr(a, RegCpRbBase, (u32int)rbpa);
	gwr(a, RegCpRbBaseHi, (u32int)(rbpa >> 32));
	gwr(a, RegCpRbCntl, RbCntlDefault);
	gwr(a, RegCpRbRptrAddrLo, (u32int)mempa);
	gwr(a, RegCpRbRptrAddrHi, (u32int)(mempa >> 32));
	gwr(a, RegCpRbWptr, 0);

	gwr(a, RegCpSqeInstrBaseLo, (u32int)sqepa);
	gwr(a, RegCpSqeInstrBaseHi, (u32int)(sqepa >> 32));
	gwr(a, RegCpSqeCntl, 1);

	/* Attempt non-secure transition if TZ doesn't block */
	a->trust_before = grd(a, RegRbbmSecvidTrustCntl);
	gwr(a, RegRbbmSecvidTrustCntl, 0x0);
	a->trust_after = grd(a, RegRbbmSecvidTrustCntl);

	/* Emit CP_ME_INIT + CP_MEM_WRITE fence */
	rb[wptr++] = pkt7(CpMeInit, 8);
	rb[wptr++] = 0x0000002f;
	rb[wptr++] = 0x00000003;
	rb[wptr++] = 0x20000000;
	rb[wptr++] = 0x00000000;
	rb[wptr++] = 0x00000000;
	rb[wptr++] = 0x00000000;
	rb[wptr++] = 0x00000000;
	rb[wptr++] = 0x00000000;

	rb[wptr++] = pkt7(CpMemWrite, 3);
	rb[wptr++] = (u32int)fencepa;
	rb[wptr++] = (u32int)(fencepa >> 32);
	rb[wptr++] = 0x1337beef;

	drvdmacache(DrvClean, rbpa, wptr * 4);
	gwr(a, RegCpRbWptr, wptr);

	for(i = 0; i < 500; i++){
		drvdmacache(DrvInval, mempa, 4096);
		a->rb_rptr = grd(a, RegCpRbRptr);
		a->rb_wptr = grd(a, RegCpRbWptr);
		a->fence_val = memptrs[16];
		if(a->fence_val == 0x1337beef){
			a->sqeok = 1;
			break;
		}
		sleep(1);
	}
	a->rbbm_status = grd(a, RegRbbmStatus);
	a->rbbm_int0 = grd(a, RegRbbmInt0Status);

	drvdmafree(sqepa, 64*1024);
	drvdmafree(rbpa, RbSize);
	drvdmafree(mempa, 4096);
	return a->sqeok ? 0 : -1;
}

static int
statustext(Adreno *a, char *buf, int nbuf)
{
	return snprint(buf, nbuf,
		"gpu %s (0x%llux+%#llux irq %d)\n"
		"gpucc %s (0x%llux)\n"
		"iommu %s (0x%llux) scr0=0x%ux idr0=0x%ux cb0_sctlr=0x%ux\n"
		"powered %d sqeok %d\n"
		"rbbm_status=0x%ux rbbm_int0=0x%ux trust=0x%ux->0x%ux\n"
		"rb_rptr=%ud rb_wptr=%ud fence=0x%ux\n"
		"why=%s\n",
		a->hasgpu ? a->gpu.name : "none", a->gpu.base, a->gpu.size, a->gpu.irq,
		a->hasgpucc ? a->gpucc.name : "none", a->gpucc.base,
		a->hasiommu ? a->iommu.name : "none", a->iommu.base,
		a->smmu_scr0, a->smmu_idr0, a->smmu_cb0_sctlr,
		a->powered, a->sqeok,
		a->rbbm_status, a->rbbm_int0, a->trust_before, a->trust_after,
		a->rb_rptr, a->rb_wptr, a->fence_val,
		a->why[0] ? a->why : "ok");
}

static void
fsread(Req *r, void *aux)
{
	DrvFile *f;
	char buf[1024];
	int n;

	USED(aux);

	f = drvfile(r);
	if(strcmp(f->name, "status") == 0){
		probe_regs(&adreno);
		n = statustext(&adreno, buf, sizeof buf);
		readbuf(r, buf, n);
		respond(r, nil);
		return;
	}
	if(strcmp(f->name, "regs") == 0){
		probe_regs(&adreno);
		n = snprint(buf, sizeof buf,
			"rbbm_status=0x%ux\nrbbm_int0=0x%ux\nsecvid_trust=0x%ux\n"
			"cp_rb_rptr=%ud\ncp_rb_wptr=%ud\n",
			adreno.rbbm_status, adreno.rbbm_int0, adreno.trust_after,
			adreno.rb_rptr, adreno.rb_wptr);
		readbuf(r, buf, n);
		respond(r, nil);
		return;
	}
	respond(r, "not implemented");
}

static void
fswrite(Req *r, void *aux)
{
	DrvFile *f;
	char cmd[64];
	int n;

	USED(aux);

	f = drvfile(r);
	if(strcmp(f->name, "ctl") != 0){
		respond(r, "permission denied");
		return;
	}
	n = r->ifcall.count;
	if(n >= sizeof cmd)
		n = sizeof cmd - 1;
	memmove(cmd, r->ifcall.data, n);
	cmd[n] = 0;
	while(n > 0 && (cmd[n-1] == '\n' || cmd[n-1] == ' '))
		cmd[--n] = 0;
	if(strcmp(cmd, "poweron") == 0){
		if(power_on(&adreno) < 0){
			respond(r, adreno.why);
			return;
		}
		probe_regs(&adreno);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	if(strcmp(cmd, "test") == 0){
		sqe_test(&adreno);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	respond(r, "unknown ctl");
}

static DrvFile files[] = {
	{ "ctl",    0666, &adreno, nil,    fswrite },
	{ "status", 0444, &adreno, fsread, nil     },
	{ "regs",   0444, &adreno, fsread, nil     },
};

static void
usage(void)
{
	fprint(2, "usage: adrenofs [-i] [-t]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	DrvSrv *s;
	int info = 0, test = 0;
	char buf[1024];

	ARGBEGIN{
	case 'i':
		info = 1;
		break;
	case 't':
		test = 1;
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	if(drvfind(&adreno.gpu, "gpu", 0) == 0 && drvmap(&adreno.gpu) == 0)
		adreno.hasgpu = 1;
	if(drvfindexact(&adreno.gpucc, "qcom,gpucc@5990000") == 0 &&
	   drvmap(&adreno.gpucc) == 0)
		adreno.hasgpucc = 1;
	if(drvfind(&adreno.iommu, "iommu", 0) == 0 && drvmap(&adreno.iommu) == 0)
		adreno.hasiommu = 1;
	if(drvfindexact(&adreno.gcc, "qcom,gcc@1400000") == 0 &&
	   drvmap(&adreno.gcc) == 0)
		adreno.hasgcc = 1;

	if(adreno.hasgpu && adreno.hasgpucc){
		power_on(&adreno);
		probe_regs(&adreno);
	}
	if(test)
		sqe_test(&adreno);

	if(info || test){
		statustext(&adreno, buf, sizeof buf);
		print("%s", buf);
		if(test)
			print("HARVEY-ARM64 ADRENO SQE %s\n",
			      adreno.sqeok ? "PASS" : "FAIL");
		exits(adreno.hasgpu ? nil : "nogpu");
	}

	s = drvsrv("adreno", files, nelem(files));
	if(s == nil)
		drvfail("adrenofs: cannot build 9P tree");
	drvsrvpost(s);
	drvhealthrun("adrenofs");
}
