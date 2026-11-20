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
	print("adrenofs: hw_init gbif\n");
	/* A610 GBIF/VBIF QoS */
	gwr(a, RegGbifQsbSide0, 0x00071620);
	gwr(a, RegGbifQsbSide1, 0x00071620);
	gwr(a, RegGbifQsbSide2, 0x00071620);
	gwr(a, RegGbifQsbSide3, 0x00071620);
	gwr(a, RegRbbmClientQos, 0x3);
	gwr(a, RegGbifHalt, 0x0);
	print("adrenofs: hw_init gbif done\n");

	print("adrenofs: hw_init hwcg\n");
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
	print("adrenofs: hw_init hwcg done\n");

	print("adrenofs: hw_init uche/cp\n");
	/* UCHE & CP A610 sizing */
	gwr(a, RegUcheFilterCntl, 0x804);
	gwr(a, RegUcheCacheWays, 0x4);
	gwr(a, RegCpRoqThresholds2, 0x00800060);
	gwr(a, RegCpRoqThresholds1, 0x40201b16);
	gwr(a, RegCpMemPoolSize, 48);
	gwr(a, RegCpMemPoolDbgAddr, 47);
	print("adrenofs: hw_init uche/cp done\n");
}

static int
sqe_test(Adreno *a)
{
	u32int *sqe, *rb, *memptrs;
	u64int sqepa, rbpa, mempa, fencepa;
	uint wptr = 0;
	int i;

	a->sqeok = 0;

	print("adrenofs: sqe stage dma alloc sqe\n");
	sqe = drvdma(&a->gpu, 64*1024, 4096, &sqepa);
	print("adrenofs: sqe stage dma alloc rb\n");
	rb = drvdma(&a->gpu, RbSize, 4096, &rbpa);
	print("adrenofs: sqe stage dma alloc mem\n");
	memptrs = drvdma(&a->gpu, 4096, 4096, &mempa);
	print("adrenofs: sqe stage dma done sqe=%p rb=%p mem=%p\n",
		sqe, rb, memptrs);
	if(sqe == nil || rb == nil || memptrs == nil){
		snprint(a->why, sizeof a->why, "drvdma failed: %r");
		return -1;
	}

	memmove(sqe, a630_sqe_fw, sizeof a630_sqe_fw);
	memset(rb, 0, RbSize);
	memset(memptrs, 0, 4096);
	fencepa = mempa + 64;

	print("adrenofs: sqe stage cache clean\n");
	drvdmacache(DrvClean, sqepa, sizeof a630_sqe_fw);
	print("adrenofs: sqe stage cache sqe done\n");
	drvdmacache(DrvClean, mempa, 4096);
	print("adrenofs: sqe stage cache mem done\n");
	/* Power up only after the DMA arena is attached: on boot13
	 * segattach("dma") wedged the console when it ran after
	 * power_on's GPUCC register writes (adrenofs -d, which never
	 * powers the GPU, attaches the same arena cleanly). */
	print("adrenofs: sqe stage power_on\n");
	if(power_on(a) < 0){
		drvdmafree(sqepa, 64*1024);
		drvdmafree(rbpa, RbSize);
		drvdmafree(mempa, 4096);
		return -1;
	}
	probe_regs(a);
	print("adrenofs: sqe stage hw_init\n");

	a610_hw_init(a);
	print("adrenofs: sqe stage rb/sqe program\n");

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

	print("adrenofs: sqe stage hw_init done, submitting ring\n");
	drvdmacache(DrvClean, rbpa, wptr * 4);
	gwr(a, RegCpRbWptr, wptr);
	print("adrenofs: sqe stage ring submitted, waiting fence\n");

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
	print("adrenofs: sqe stage wait done sqeok=%d rptr=%ud wptr=%ud fence=0x%ux rbbm=0x%ux int0=0x%ux\n",
		a->sqeok, a->rb_rptr, a->rb_wptr, a->fence_val,
		a->rbbm_status, a->rbbm_int0);
	if(!a->sqeok)
		snprint(a->why, sizeof a->why,
			"sqe fence timeout rptr=%ud wptr=%ud fence=0x%ux rbbm=0x%ux",
			a->rb_rptr, a->rb_wptr, a->fence_val, a->rbbm_status);

	drvdmafree(sqepa, 64*1024);
	drvdmafree(rbpa, RbSize);
	drvdmafree(mempa, 4096);
	return a->sqeok ? 0 : -1;
}

/*
 * DMA-only probe (-d): allocate, map, touch and free one buffer
 * through #P/dma without powering the GPU.  Isolates the drvkit
 * DMA path (alloc write, arena segattach, cache op) that wedged
 * the console at "sqe stage dma" on boot8.
 */
static int
dma_probe(Adreno *a)
{
	u32int *buf;
	u64int pa;
	int i;

	print("adrenofs: dma probe alloc\n");
	buf = drvdma(&a->gpu, 64*1024, 4096, &pa);
	print("adrenofs: dma probe alloc done buf=%p pa=0x%llux\n", buf, pa);
	if(buf == nil){
		snprint(a->why, sizeof a->why, "drvdma failed: %r");
		return -1;
	}
	for(i = 0; i < 1024; i++)
		buf[i] = 0x5a000000 | i;
	print("adrenofs: dma probe touch done buf[1023]=0x%ux\n", buf[1023]);
	drvdmacache(DrvClean, pa, 4096);
	print("adrenofs: dma probe cache clean done\n");
	drvdmafree(pa, 64*1024);
	print("adrenofs: dma probe free done\n");
	snprint(a->why, sizeof a->why, "ok");
	return 0;
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
	fprint(2, "usage: adrenofs [-i] [-t] [-d]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	DrvSrv *s;
	int info = 0, test = 0, dmaprobe = 0;
	char buf[1024];

	ARGBEGIN{
	case 'i':
		info = 1;
		break;
	case 't':
		test = 1;
		break;
	case 'd':
		dmaprobe = 1;
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	print("adrenofs: stage find gpu\n");
	if(drvfindexact(&adreno.gpu, "qcom,kgsl-3d0@5900000") == 0 &&
	   drvmap(&adreno.gpu) == 0)
		adreno.hasgpu = 1;
	print("adrenofs: stage find gpucc hasgpu=%d\n", adreno.hasgpu);
	if(drvfindexact(&adreno.gpucc, "qcom,gpucc@5990000") == 0 &&
	   drvmap(&adreno.gpucc) == 0)
		adreno.hasgpucc = 1;
	print("adrenofs: stage find iommu hasgpucc=%d\n", adreno.hasgpucc);
	if(drvfindexact(&adreno.iommu, "qcom,kgsl-iommu@59a0000") == 0 &&
	   drvmap(&adreno.iommu) == 0)
		adreno.hasiommu = 1;
	print("adrenofs: stage find gcc hasiommu=%d\n", adreno.hasiommu);
	if(drvfindexact(&adreno.gcc, "qcom,gcc@1400000") == 0 &&
	   drvmap(&adreno.gcc) == 0)
		adreno.hasgcc = 1;
	print("adrenofs: stage found hasgpu=%d hasgpucc=%d hasgcc=%d\n",
		adreno.hasgpu, adreno.hasgpucc, adreno.hasgcc);

	if(dmaprobe){
		int rc = dma_probe(&adreno);

		print("HARVEY-ARM64 ADRENO DMA %s\n", rc == 0 ? "PASS" : "FAIL");
		exits(rc == 0 ? nil : "dma");
	}

	if(adreno.hasgpu && adreno.hasgpucc && !test){
		print("adrenofs: stage power_on\n");
		power_on(&adreno);
		print("adrenofs: stage probe_regs powered=%d\n", adreno.powered);
		probe_regs(&adreno);
		print("adrenofs: stage probed\n");
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
