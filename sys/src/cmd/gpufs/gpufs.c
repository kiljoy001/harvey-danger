/*
 * gpufs - the Mali Bifrost (G52) GPU as a 9P service.
 *
 * This is the first rung: power the RK3568 PD_GPU domain, ungate the GPU
 * clocks, release the CRU resets, and read the Mali identification and
 * feature registers.  Those registers are what a job-submission driver and
 * its userspace need before anything else, and reading them proves the
 * power/clock/reset sequence on the board.
 *
 * Not here yet: the GPU MMU, job descriptors, tiler/shader power, and the
 * Bifrost job slots.  pfdev-style register definitions are from ARM's
 * mali_midg_regmap.h (the same ones panfrost uses).
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"
#include "drawreplay.h"

enum {
	PmuBase		= 0xfdd90000,
	CruBase		= 0xfdd20000,

	/* RK3568 PMU power-domain controller (PMU_PWR_DWN_CON 0xa0,
	 * PMU_BUS_IDLE_CON 0x50, status 0x98, ack 0x60, idle 0x68). */
	PmuPwr		= 0xa0,
	PmuReq		= 0x50,
	PmuStatus	= 0x98,
	PmuAck		= 0x60,
	PmuIdle		= 0x68,

	PdGpu		= 1<<0,	/* power bit */
	PdGpuReq	= 1<<1,	/* idle request/ack bit */

	/* CRU: CLK_GPU_SRC gate is CLKGATE_CON(2) bit 0, CLK_GPU is bit 3.
	 * SRST_GPU is 32 and SRST_A_GPU_NIU is 33, both in SOFTRST_CON(2). */
	CruGpuGate	= 0x300 + 2*4,
	CruGpuSoftrst	= 0x400 + 2*4,

	/* Mali top-level registers. */
	GpuId		= 0x00,
	GpuL2Features	= 0x04,
	GpuCoreFeatures	= 0x08,
	GpuTilerFeatures = 0x0c,
	GpuMemFeatures	= 0x10,
	GpuMmuFeatures	= 0x14,
	GpuAsPresent	= 0x18,
	GpuJsPresent	= 0x1c,
	GpuIntRawstat	= 0x20,
	GpuIntClear	= 0x24,
	GpuIntMask	= 0x28,
	GpuIntStat	= 0x2c,
	GpuCmd		= 0x30,
	GpuStatus	= 0x34,
	GpuFaultStatus	= 0x3c,
	GpuFaultAddrLo	= 0x40,
	GpuFaultAddrHi	= 0x44,
	GpuThreadMax	= 0xa0,
	GpuThreadMaxWg	= 0xa4,
	GpuThreadMaxBar	= 0xa8,
	GpuThreadFeatures = 0xac,
	GpuShaderPresentLo = 0x100,
	GpuShaderPresentHi = 0x104,
	GpuTilerPresentLo = 0x110,
	GpuTilerPresentHi = 0x114,
	GpuL2PresentLo	= 0x120,
	GpuL2PresentHi	= 0x124,
	GpuCoherency	= 0x300,
	GpuStackPresentLo = 0xe00,
	GpuStackPresentHi = 0xe04,

	GpuJmConfig	= 0xf00,
	GpuLatestFlush	= 0x38,
	GpuCmdSoftReset = 0x01,
	GpuIrqResetCompleted = 1<<8,
	GpuIrqMaskAll	= 0x00030f87,	/* fault, multiple, reset, power,
					 * perfcnt, clean caches */

	/* Power-on handshake. */
	L2PwronLo	= 0x1a0,
	L2ReadyLo	= 0x160,
	ShaderPwronLo	= 0x180,
	ShaderReadyLo	= 0x140,
	TilerPwronLo	= 0x190,
	TilerReadyLo	= 0x150,

	/* GPU MMU: 0x2400 + (as << 6). */
	MmuBase		= 0x2400,
	MmuAsShift	= 6,
	MmuIntRawstat	= 0x2000,
	MmuIntClear	= 0x2004,
	MmuIntMask	= 0x2008,
	AsTranstabLo	= 0x00,
	AsTranstabHi	= 0x04,
	AsMemattrLo	= 0x08,
	AsMemattrHi	= 0x0c,
	AsLockaddrLo	= 0x10,
	AsLockaddrHi	= 0x14,
	AsCommand	= 0x18,
	AsStatus	= 0x28,
	AsTranscfgLo	= 0x30,
	AsTranscfgHi	= 0x34,
	AsStatusActive	= 1<<0,

	AsCmdUpdate	= 0x01,
	AsCmdFlushPt	= 0x04,
	AsCmdFlushMem	= 0x05,

	/* Job manager: 0x1800 + (slot << 7). */
	JobIntRawstat	= 0x1000,
	JobIntClear	= 0x1004,
	JobIntMask	= 0x1008,
	JsBase		= 0x1800,
	JsStride	= 0x80,
	JobSlot		= 1,	/* JS0 fragment, JS1 vertex/tiler */
	JsHeadLo	= 0x40,
	JsHeadHi	= 0x44,
	JsAffinityLo	= 0x50,
	JsAffinityHi	= 0x54,
	JsConfig	= 0x58,
	JsCommand	= 0x60,
	JsFlushId	= 0x70,
	JsStatus	= 0x24,

	JobTypeWriteValue = 2,
	WriteValueImmediate64 = 7,

	JobVa		= 0x10000000ULL,
	DataVa		= 0x10001000ULL,
	FbVa		= 0x20000000ULL,	/* GPU view of the scanout buffer */
	LocalVa		= 0x02000000ULL,	/* scratch render target for tests */
	HeapVa		= 0x08000000ULL,	/* tiler heap for the draw diagnostic */
	Heapsize	= 4*1024*1024,
	TileShift	= 4,			/* 16x16 pixel tiles */
	FbMaxPages	= 4096,
};

typedef struct Gpu Gpu;
struct Gpu {
	Drv	d;		/* the gpu@... node */
	Drv	pmu;
	Drv	cru;
	QLock	lk;

	int	powered;
	int	mapped;
	int	responded;
	char	why[160];	/* first thing that went wrong */

	u32	rawid, id, rev, major, minor, status;
	u32	l2, core, tiler, mem, mmu, as, js;
	u32	threads, threadwg, threadbar;
	u32	shaderlo, shaderhi, tilerlo, tilerhi;
	u32	l2lo, l2hi, stacklo, stackhi;
	u32	jsf[3];

	/* MMU and the first job. */
	u64	pgdpa, l1pa, l2pa, l3pa;
	u64	jobpa, datapa;
	u64	*pgd, *pt1, *pt2, *pt3;
	u32	*job;			/* job descriptors are u32 words */
	void	*locbuf;		/* scratch render target */
	u64	locpa;
	void	*drawbuf;		/* replay pool for the draw diagnostic */
	u64	drawpa;
	void	*heapbuf;		/* tiler heap for the draw diagnostic */
	u64	heappa;
	void	*data;
	int	jobok;
	u64	jobval;
	char	joberr[96];
};

static Gpu gpu;
static void irqmask(Gpu*);

/* HIWORD-MASK register write: bits 31:16 are the mask, 15:0 the value. */
static void
wm(Drv *d, u32 off, u32 mask, u32 val)
{
	drw(d, off, (val & mask) | (mask << 16));
}

static void
setwhy(Gpu *g, char *fmt, ...)
{
	va_list a;

	if(g->why[0] != 0)
		return;		/* keep the first failure */
	va_start(a, fmt);
	vseprint(g->why, g->why + sizeof g->why, fmt, a);
	va_end(a);
}

/*
 * PD_GPU is power bit 0 and idle request bit 1 in the PMU controller.  The
 * sequence is the same one devxhci.c uses for PD_PIPE (and Linux's genpd
 * driver): clear power-down, wait for status to follow, clear the idle
 * request, wait for both the ack and idle bits.
 */
static int
pmuon(Gpu *g)
{
	int i;

	if(drvfindreg(&g->pmu, PmuBase) < 0 || drvmap(&g->pmu) < 0){
		setwhy(g, "no PMU at %#llux in #P", (u64)PmuBase);
		return -1;
	}
	if(drvfindreg(&g->cru, CruBase) < 0 || drvmap(&g->cru) < 0){
		setwhy(g, "no CRU at %#llux in #P", (u64)CruBase);
		return -1;
	}

	wm(&g->pmu, PmuPwr, PdGpu, 0);
	for(i = 0; i < 1000; i++){
		if((drd(&g->pmu, PmuStatus) & PdGpu) == 0)
			break;
		sleep(1);
	}
	if(i == 1000){
		setwhy(g, "PD_GPU power-on timeout (status %#ux)",
		       drd(&g->pmu, PmuStatus));
		return -1;
	}

	wm(&g->pmu, PmuReq, PdGpuReq, 0);
	for(i = 0; i < 1000; i++){
		if((drd(&g->pmu, PmuAck) & PdGpuReq) == 0 &&
		   (drd(&g->pmu, PmuIdle) & PdGpuReq) == 0)
			break;
		sleep(1);
	}
	if(i == 1000){
		setwhy(g, "PD_GPU idle timeout (ack %#ux idle %#ux)",
		       drd(&g->pmu, PmuAck), drd(&g->pmu, PmuIdle));
		return -1;
	}

	/*
	 * The GPU core rate is owned by the firmware through SCMI, so the CRU
	 * mux/div are read-only; the two gates and the resets are ours.  A
	 * cleared gate bit enables the clock (write-mask, value 0); a cleared
	 * reset bit releases it.
	 */
	wm(&g->cru, CruGpuGate, (1<<0)|(1<<3), 0);
	wm(&g->cru, CruGpuSoftrst, (1<<0)|(1<<1), 0);

	/*
	 * No interrupt handler yet: mask every source and clear whatever the
	 * boot left pending, so the GPU cannot raise a line nobody claims.
	 */
	irqmask(g);

	g->powered = 1;
	return 0;
}

static void
pmuoff(Gpu *g)
{
	if(!g->powered)
		return;
	wm(&g->pmu, PmuPwr, PdGpu, PdGpu);
	g->powered = 0;
}

/* Read the identification and feature block.  A dead bus gives 0 or ~0. */
static void
ident(Gpu *g)
{
	g->rawid = drd(&g->d, GpuId);
	/*
	 * GPU_ID2 packs arch minor in bits 27:24 and the product in 19:16.
	 * Mask like panfrost's panfrost_model_cmp: 0x74021000 is a G52
	 * (product 0x7002) with arch minor 4, not a product 0x7402.
	 */
	g->id = g->rawid >> 16;
	if(g->id & 0xf000)
		g->id &= 0xf00f;
	g->rev = g->rawid & 0xffff;
	g->major = (g->rev >> 12) & 0xf;
	g->minor = (g->rev >> 4) & 0xff;
	g->status = g->rev & 0xf;

	g->l2 = drd(&g->d, GpuL2Features);
	g->core = drd(&g->d, GpuCoreFeatures);
	g->tiler = drd(&g->d, GpuTilerFeatures);
	g->mem = drd(&g->d, GpuMemFeatures);
	g->mmu = drd(&g->d, GpuMmuFeatures);
	g->as = drd(&g->d, GpuAsPresent);
	g->js = drd(&g->d, GpuJsPresent);
	g->threads = drd(&g->d, GpuThreadMax);
	g->threadwg = drd(&g->d, GpuThreadMaxWg);
	g->threadbar = drd(&g->d, GpuThreadMaxBar);
	g->shaderlo = drd(&g->d, GpuShaderPresentLo);
	g->shaderhi = drd(&g->d, GpuShaderPresentHi);
	g->tilerlo = drd(&g->d, GpuTilerPresentLo);
	g->tilerhi = drd(&g->d, GpuTilerPresentHi);
	g->l2lo = drd(&g->d, GpuL2PresentLo);
	g->l2hi = drd(&g->d, GpuL2PresentHi);
	g->stacklo = drd(&g->d, GpuStackPresentLo);
	g->stackhi = drd(&g->d, GpuStackPresentHi);
	g->jsf[0] = drd(&g->d, 0xc0);
	g->jsf[1] = drd(&g->d, 0xc4);
	g->jsf[2] = drd(&g->d, 0xc8);

	g->responded = g->rawid != 0 && g->rawid != ~0U;
	if(!g->responded)
		setwhy(g, "GPU_ID reads %#ux (bus not responding)", g->rawid);
}

static char *
model(u32 id)
{
	switch(id){
	case 0x7000: return "g51";
	case 0x7001: return "g76";
	case 0x7002: return "g52";
	case 0x7003: return "g31";
	case 0x9001: return "g57";
	case 0x9003: return "g57";
	case 0x9093: return "g57";
	}
	return "bifrost";
}

static char *
statustext(Gpu *g, char *buf, int nbuf)
{
	char *p, *e;
	u32 va, pa;

	p = buf;
	e = buf + nbuf;
	if(!g->mapped)
		return seprint(p, e, "no GPU node: %s\n", g->why);
	if(!g->powered)
		return seprint(p, e, "power off: %s\n", g->why);

	va = g->mmu & 0xff;
	pa = (g->mmu >> 8) & 0xff;
	p = seprint(p, e, "model mali-%s\n", model(g->id));
	p = seprint(p, e, "id 0x%ux raw 0x%08ux revision 0x%ux major %ud minor %ud status %ud\n",
	            g->id, g->rawid, g->rev, g->major, g->minor, g->status);
	p = seprint(p, e, "features l2 0x%08ux core 0x%08ux tiler 0x%08ux mem 0x%08ux\n",
	            g->l2, g->core, g->tiler, g->mem);
	p = seprint(p, e, "mmu 0x%08ux va %ud pa %ud as 0x%ux js 0x%ux\n",
	            g->mmu, va, pa, g->as, g->js);
	p = seprint(p, e, "js features 0x%08ux 0x%08ux 0x%08ux\n",
	            g->jsf[0], g->jsf[1], g->jsf[2]);
	p = seprint(p, e, "present shaders 0x%08ux%08ux tiler 0x%08ux%08ux l2 0x%08ux%08ux stack 0x%08ux%08ux\n",
	            g->shaderhi, g->shaderlo, g->tilerhi, g->tilerlo,
	            g->l2hi, g->l2lo, g->stackhi, g->stacklo);
	p = seprint(p, e, "threads %ud workgroup %ud barrier %ud\n",
	            g->threads, g->threadwg, g->threadbar);
	p = seprint(p, e, "power on response %s", g->responded ? "yes" : "no");
	if(g->why[0] != 0)
		p = seprint(p, e, " (%s)", g->why);
	p = seprint(p, e, "\n");
	if(g->jobok)
		p = seprint(p, e, "job pass value 0x%llux\n", g->jobval);
	else if(g->joberr[0] != 0)
		p = seprint(p, e, "job fail %s\n", g->joberr);
	return buf;
}

static char *
regstext(Gpu *g, char *buf, int nbuf)
{
	char *p, *e;
	static struct { char *name; u32 off; } reg[] = {
		{ "GPU_ID", GpuId },
		{ "L2_FEATURES", GpuL2Features },
		{ "CORE_FEATURES", GpuCoreFeatures },
		{ "TILER_FEATURES", GpuTilerFeatures },
		{ "MEM_FEATURES", GpuMemFeatures },
		{ "MMU_FEATURES", GpuMmuFeatures },
		{ "AS_PRESENT", GpuAsPresent },
		{ "JS_PRESENT", GpuJsPresent },
		{ "INT_RAWSTAT", GpuIntRawstat },
		{ "INT_MASK", GpuIntMask },
		{ "STATUS", GpuStatus },
		{ "FAULT_STATUS", GpuFaultStatus },
		{ "THREAD_MAX", GpuThreadMax },
		{ "SHADER_PRESENT_LO", GpuShaderPresentLo },
		{ "L2_PRESENT_LO", GpuL2PresentLo },
		{ "COHERENCY", GpuCoherency },
	};
	int i;

	p = buf;
	e = buf + nbuf;
	if(!g->mapped || !g->powered)
		return seprint(p, e, "unavailable\n");
	for(i = 0; i < (int)nelem(reg); i++)
		p = seprint(p, e, "%04ux %-20s 0x%08ux\n", reg[i].off,
		            reg[i].name, drd(&g->d, reg[i].off));
	return buf;
}

/* No interrupt handler yet: leave every source masked. */
static void
irqmask(Gpu *g)
{
	drw(&g->d, GpuIntMask, GpuIrqMaskAll);
	drw(&g->d, GpuIntClear, GpuIrqMaskAll);
}

/* A soft reset is the GPU's own reset, distinct from the CRU line. */
static void
softreset(Gpu *g)
{
	int i;

	drw(&g->d, GpuIntMask, GpuIrqMaskAll);
	drw(&g->d, GpuIntClear, GpuIrqMaskAll);
	drw(&g->d, GpuCmd, GpuCmdSoftReset);
	for(i = 0; i < 100; i++){
		if(drd(&g->d, GpuIntRawstat) & GpuIrqResetCompleted)
			break;
		sleep(1);
	}
	drw(&g->d, GpuIntClear, GpuIrqMaskAll);
	sleep(1);
}

/* MMU_AS(as) + off, and JS(slot) + off. */
#define ASOFF(as, off)	(MmuBase + ((as) << MmuAsShift) + (off))
#define JSOFF(slot, off) (JsBase + ((slot) * JsStride) + (off))

/* Wait for (reg & mask) == want, in milliseconds. */
static int
waitbits(Gpu *g, u32 off, u32 mask, u32 want, int ms)
{
	int i;

	for(i = 0; i < ms; i++){
		if((drd(&g->d, off) & mask) == want)
			return 0;
		sleep(1);
	}
	return -1;
}

/*
 * Power the L2, shader and tiler.  The present bitmaps were read with the
 * feature block; a write-value job needs at least L2, and the others are
 * powered now so the first shader job does not have to repeat this.
 */
static int
gpupower(Gpu *g)
{
	u32 l2, sh, ti;

	l2 = g->l2lo & 1;
	sh = g->shaderlo & 1;
	ti = g->tilerlo & 1;
	if(l2 == 0 || sh == 0 || ti == 0){
		setwhy(g, "no L2/shader/tiler present (%ux/%ux/%ux)",
		       g->l2lo, g->shaderlo, g->tilerlo);
		return -1;
	}

	/* G52 has HW_FEATURE_IDVS_GROUP_SIZE; Linux writes the default. */
	drw(&g->d, GpuJmConfig, 0xf << 16);

	drw(&g->d, L2PwronLo, l2);
	if(waitbits(g, L2ReadyLo, l2, l2, 100) < 0){
		setwhy(g, "L2 power-on timeout (ready %#ux)", drd(&g->d, L2ReadyLo));
		return -1;
	}
	drw(&g->d, ShaderPwronLo, sh);
	if(waitbits(g, ShaderReadyLo, sh, sh, 100) < 0){
		setwhy(g, "shader power-on timeout (ready %#ux)",
		       drd(&g->d, ShaderReadyLo));
		return -1;
	}
	drw(&g->d, TilerPwronLo, ti);
	if(waitbits(g, TilerReadyLo, ti, ti, 100) < 0){
		setwhy(g, "tiler power-on timeout (ready %#ux)",
		       drd(&g->d, TilerReadyLo));
		return -1;
	}
	return 0;
}

/* One 4 KiB, zeroed page from the #P DMA arena, with its physical address. */
static void *
dmazero(Gpu *g, u64 *pa)
{
	void *p;

	p = drvdma(&g->d, 4096, 4096, pa);
	if(p != nil)
		memset(p, 0, 4096);
	return p;
}

/*
 * Mali LPAE is always a four-level table, even for the G52's 35-bit VA:
 * the top level has one entry (pgd_bits = 0), then 512 entries per level.
 * A 4 KiB leaf lives at level 3; level shifts are 39, 30, 21, 12.
 */
static int
mmuidx(u64 va, int lvl)
{
	if(lvl == 0)
		return 0;
	return (va >> ((4 - lvl) * 9 + 3)) & 0x1ff;
}

static u64
tablepte(u64 pa)
{
	return (pa & ~0xfffULL) | 3;	/* TYPE_TABLE */
}

/*
 * A normal cached read/write page: TYPE_BLOCK, HAP read+write (Mali uses
 * the stage-2-like permission bits), attribute index 1 (the 0x8d write
 * allocate memattr), outer shareable.  Mali does not use the AF bit.
 */
static u64
pagepte(u64 pa)
{
	/*
	 * ARM_MALI_LPAE leaf as io-pgtable-arm builds it for
	 * IOMMU_READ|IOMMU_WRITE|IOMMU_CACHE: stage-2-like permissions
	 * (HAP bits 6:7), stage-1-like attributes.
	 *
	 *   TYPE_BLOCK (1) | ATTRIDX_CACHE (1 << 2) | HAP_READ (1 << 6)
	 *   | HAP_WRITE (1 << 7) | SH_OS (2 << 8)   ==  0x2c5
	 *
	 * HAP_READ|HAP_WRITE is what lets the GPU fetch the job header and
	 * write the exception status back into it.  Note that the stage-1
	 * nG/AP bits are meaningless here: setting AP_RDONLY (bit 7)
	 * actually clears HAP_WRITE, and the job then faults on its own
	 * header address.
	 */
	return (pa & ~0xfffULL) | 0x2c5ULL;
}

static void
mmumap(Gpu *g, u64 va, u64 pa)
{
	g->pgd[0] = tablepte(g->l1pa);
	g->pt1[mmuidx(va, 1)] = tablepte(g->l2pa);
	g->pt2[mmuidx(va, 2)] = tablepte(g->l3pa);
	g->pt3[mmuidx(va, 3)] = pagepte(pa);
}

/* Wait for the address space to be idle, issue a command, wait again. */
static int
mmucmd(Gpu *g, u32 cmd)
{
	if(waitbits(g, ASOFF(0, AsStatus), AsStatusActive, 0, 100) < 0)
		return -1;
	drw(&g->d, ASOFF(0, AsCommand), cmd);
	return waitbits(g, ASOFF(0, AsStatus), AsStatusActive, 0, 100);
}

static void
mmuclean(Gpu *g)
{
	drvdmacache(DrvClean, g->pgdpa, 4096);
	drvdmacache(DrvClean, g->l1pa, 4096);
	drvdmacache(DrvClean, g->l2pa, 4096);
	drvdmacache(DrvClean, g->l3pa, 4096);
	drvdmacache(DrvClean, g->jobpa, 4096);
	drvdmacache(DrvClean, g->datapa, 4096);
}

static int
mmuinit(Gpu *g)
{
	if(g->pgd != nil)
		return 0;

	g->pgd = dmazero(g, &g->pgdpa);
	g->pt1 = dmazero(g, &g->l1pa);
	g->pt2 = dmazero(g, &g->l2pa);
	g->pt3 = dmazero(g, &g->l3pa);
	g->job = dmazero(g, &g->jobpa);
	g->data = dmazero(g, &g->datapa);
	if(g->pgd == nil || g->pt1 == nil || g->pt2 == nil || g->pt3 == nil ||
	   g->job == nil || g->data == nil){
		setwhy(g, "cannot allocate GPU page tables or buffers");
		return -1;
	}

	mmumap(g, JobVa, g->jobpa);
	mmumap(g, DataVa, g->datapa);
	mmuclean(g);
	return 0;
}

static int
mmuprogram(Gpu *g)
{
	u64 transtab, memattr;

	/* PGD | READ_INNER | ADRMODE_TABLE.  The GPU is not coherent, so
	 * SHARE_OUTER (bit 4) is not set. */
	transtab = g->pgdpa | 0x7;
	memattr = 0x008d0088ULL;	/* NC 0x88, WB 0x8d, DEV 0x88 */

	if(mmucmd(g, AsCmdFlushMem) < 0){
		setwhy(g, "MMU flush before enable failed");
		return -1;
	}
	drw(&g->d, ASOFF(0, AsTranstabLo), (u32)transtab);
	drw(&g->d, ASOFF(0, AsTranstabHi), (u32)(transtab >> 32));
	drw(&g->d, ASOFF(0, AsMemattrLo), (u32)memattr);
	drw(&g->d, ASOFF(0, AsMemattrHi), (u32)(memattr >> 32));
	drw(&g->d, ASOFF(0, AsTranscfgLo), 0);	/* ADRMODE_LEGACY */
	drw(&g->d, ASOFF(0, AsTranscfgHi), 0);
	if(mmucmd(g, AsCmdUpdate) < 0){
		setwhy(g, "MMU update failed");
		return -1;
	}
	drw(&g->d, MmuIntClear, 0xffffffff);
	drw(&g->d, MmuIntMask, 0);		/* poll, no IRQ */
	return 0;
}

static void
gpudump(Gpu *g)
{
	int i;

	if(g->pgd == nil)
		return;
	print("gpufs: AS transtab %#ux/%#ux memattr %#ux/%#ux transcfg %#ux/%#ux status %#ux\n",
	      drd(&g->d, ASOFF(0, AsTranstabLo)), drd(&g->d, ASOFF(0, AsTranstabHi)),
	      drd(&g->d, ASOFF(0, AsMemattrLo)), drd(&g->d, ASOFF(0, AsMemattrHi)),
	      drd(&g->d, ASOFF(0, AsTranscfgLo)), drd(&g->d, ASOFF(0, AsTranscfgHi)),
	      drd(&g->d, ASOFF(0, AsStatus)));
	print("gpufs: PTE pgd %#llux l1 %#llux l2 %#llux l3[0] %#llux l3[1] %#llux\n",
	      g->pgd[0], g->pt1[mmuidx(JobVa, 1)],
	      g->pt2[mmuidx(JobVa, 2)], g->pt3[0], g->pt3[1]);
	print("gpufs: job");
	for(i = 0; i < 14; i++)
		print(" %#ux", (u32)g->job[i]);
	print("\n");
	print("gpufs: JS cfg %#ux headnext %#ux/%#ux head %#ux tail %#ux status %#ux state %#ux fault %#ux addr %#llux\n",
	      drd(&g->d, JSOFF(JobSlot, JsConfig)),
	      drd(&g->d, JSOFF(JobSlot, JsHeadLo)), drd(&g->d, JSOFF(JobSlot, JsHeadHi)),
	      drd(&g->d, JSOFF(JobSlot, 0x00)), drd(&g->d, JSOFF(JobSlot, 0x08)),
	      drd(&g->d, JSOFF(JobSlot, JsStatus)), drd(&g->d, 0x1010),
	      drd(&g->d, GpuFaultStatus),
	      ((u64)drd(&g->d, GpuFaultAddrHi) << 32) | drd(&g->d, GpuFaultAddrLo));
}

/* Drop stale GPU L2 lines so the job manager sees the CPU-written header. */
static void
gpucacheflush(Gpu *g)
{
	int i;

	drw(&g->d, GpuCmd, 0x08);	/* CLEAN_INV_CACHES */
	for(i = 0; i < 100; i++){
		if(drd(&g->d, GpuIntRawstat) & (1 << 17))
			break;
		sleep(1);
	}
	drw(&g->d, GpuIntClear, 1 << 17);
}

/* Submit one job at a GPU VA. */
static int
runjob(Gpu *g, int slot, u64 head, u32 cfg, u32 index, u32 type, u64 addr, u64 val, u32 *rawp)
{
	u32 raw;
	int i, done, err;

	done = err = 0;
	memset(g->job, 0, 4096);
	g->job[4] = 1 | (type << 1) | (index << 16);
	if(type == JobTypeWriteValue){
		g->job[8] = addr;
		g->job[9] = (u64)addr >> 32;
		g->job[10] = WriteValueImmediate64;
		g->job[12] = (u32)val;
		g->job[13] = (u32)(val >> 32);
	}
	drvdmacache(DrvClean, g->jobpa, 4096);
	gpucacheflush(g);

	drw(&g->d, JobIntClear, 0x70007);
	drw(&g->d, JobIntMask, 0x70007);
	drw(&g->d, JSOFF(slot, JsFlushId), drd(&g->d, GpuLatestFlush));
	drw(&g->d, JSOFF(slot, JsHeadLo), (u32)head);
	drw(&g->d, JSOFF(slot, JsHeadHi), (u32)(head >> 32));
	drw(&g->d, JSOFF(slot, JsAffinityLo), 1);
	drw(&g->d, JSOFF(slot, JsAffinityHi), 0);
	cfg |= (1 << 14);	/* FLUSH_REDUCTION, G52 */
	drw(&g->d, JSOFF(slot, JsConfig), cfg);
	drw(&g->d, JSOFF(slot, JsCommand), 1);

	for(i = 0; i < 1000; i++){
		raw = drd(&g->d, JobIntRawstat);
		if(raw & (1 << slot)){
			done = 1;
			break;
		}
		if(raw & (1 << (slot + 16))){
			err = 1;
			break;
		}
		sleep(1);
	}
	drw(&g->d, JobIntClear, (1 << slot) | (1 << (slot + 16)));
	if(rawp != nil)
		*rawp = raw;
	return done ? 0 : (err ? -1 : -2);
}

static void
identitytest(Gpu *g)
{
	u32 raw;
	int r;

	if(mmuinit(g) < 0)
		return;
	if(mmucmd(g, AsCmdFlushMem) < 0)
		return;
	drw(&g->d, ASOFF(0, AsTranstabLo), 2);	/* LPAE identity */
	drw(&g->d, ASOFF(0, AsTranstabHi), 0);
	drw(&g->d, ASOFF(0, AsMemattrLo), 0x008d0088);
	drw(&g->d, ASOFF(0, AsMemattrHi), 0);
	drw(&g->d, ASOFF(0, AsTranscfgLo), 0);
	drw(&g->d, ASOFF(0, AsTranscfgHi), 0);
	if(mmucmd(g, AsCmdUpdate) < 0)
		return;
	drw(&g->d, MmuIntClear, 0xffffffff);
	drw(&g->d, MmuIntMask, 0);

	r = runjob(g, 1, g->jobpa, 0x83300, 1, 1, 0, 0, &raw);
	print("gpufs: ident null %d raw %#ux js %#ux\n", r, raw,
	      drd(&g->d, JSOFF(1, JsStatus)));
	*(u64 *)g->data = 0;
	drvdmacache(DrvClean, g->datapa, 4096);
	r = runjob(g, 1, g->jobpa, 0x83300, 1, JobTypeWriteValue,
	           g->datapa, 0xcafebabedeadbeefULL, &raw);
	drvdmacache(DrvInval, g->datapa, 4096);
	print("gpufs: ident write %d raw %#ux js %#ux value %#llux\n", r, raw,
	      drd(&g->d, JSOFF(1, JsStatus)), *(u64 *)g->data);
}

/*
 * AArch64 4K page tables, as the Bifrost vendor driver uses for the job
 * manager: three levels from start_level 1, 32 entries in the PGD, leaf
 * PTEs with AF/SH_IS/ATTRINDX and TYPE_PAGE (0xf07 for cached RW).
 */
static u64
a64pte(u64 pa)
{
	return (pa & ~0xfffULL) | 0xe43ULL;	/* NC, SH_OS, AP_UNPRIV */
}

static int
mmuprogram_a64(Gpu *g)
{
	u64 transcfg;

	memset(g->pgd, 0, 4096);
	memset(g->pt2, 0, 4096);
	memset(g->pt3, 0, 4096);
	g->pgd[(JobVa >> 30) & 0x1f] = tablepte(g->l2pa);
	g->pt2[(JobVa >> 21) & 0x1ff] = tablepte(g->l3pa);
	g->pt3[(JobVa >> 12) & 0x1ff] = a64pte(g->jobpa);
	g->pt3[(DataVa >> 12) & 0x1ff] = a64pte(g->datapa);
	drvdmacache(DrvClean, g->pgdpa, 4096);
	drvdmacache(DrvClean, g->l2pa, 4096);
	drvdmacache(DrvClean, g->l3pa, 4096);

	transcfg = (2ULL << 24) | (1ULL << 30) | 6 | (20ULL << 6);
	if(mmucmd(g, AsCmdFlushMem) < 0)
		return -1;
	drw(&g->d, ASOFF(0, AsTranstabLo), (u32)g->pgdpa);
	drw(&g->d, ASOFF(0, AsTranstabHi), (u32)(g->pgdpa >> 32));
	drw(&g->d, ASOFF(0, AsMemattrLo), 0x8f4c);	/* 0 NC, 1 WB */
	drw(&g->d, ASOFF(0, AsMemattrHi), 0);
	drw(&g->d, ASOFF(0, AsTranscfgLo), (u32)transcfg);
	drw(&g->d, ASOFF(0, AsTranscfgHi), (u32)(transcfg >> 32));
	if(mmucmd(g, AsCmdUpdate) < 0)
		return -1;
	drw(&g->d, MmuIntClear, 0xffffffff);
	drw(&g->d, MmuIntMask, 0);
	return 0;
}

static void
identitymatrix(Gpu *g)
{
	static u32 cfgs[] = { 0x87300, 0x87b00, 0x83300, 0x83700 };
	u32 raw;
	int idx, c, r;

	if(mmuinit(g) < 0)
		return;
	if(mmuprogram_a64(g) < 0)
		return;

	for(idx = 0; idx < 2; idx++){
		for(c = 0; c < (int)nelem(cfgs); c++){
			r = runjob(g, 1, JobVa, cfgs[c], idx, 1, 0, 0, &raw);
			print("gpufs: matrix idx %d cfg %#ux null r %d raw %#ux js %#ux as %#ux addr %#llux\n",
			      idx, cfgs[c], r, raw,
			      drd(&g->d, JSOFF(1, JsStatus)),
			      drd(&g->d, ASOFF(0, 0x1c)),
			      ((u64)drd(&g->d, ASOFF(0, 0x24)) << 32) |
			      drd(&g->d, ASOFF(0, 0x20)));
			if(r == 0){
				*(u64 *)g->data = 0;
				drvdmacache(DrvClean, g->datapa, 4096);
				r = runjob(g, 1, JobVa, cfgs[c], idx,
				           JobTypeWriteValue, DataVa,
				           0xcafebabedeadbeefULL, &raw);
				drvdmacache(DrvInval, g->datapa, 4096);
				print("gpufs: matrix idx %d cfg %#ux write r %d raw %#ux js %#ux value %#llux\n",
				      idx, cfgs[c], r, raw,
				      drd(&g->d, JSOFF(1, JsStatus)),
				      *(u64 *)g->data);
			}
		}
	}

	*(u64 *)g->data = 0;
	drvdmacache(DrvClean, g->datapa, 4096);
	r = runjob(g, 1, JobVa, 0x83700, 1, JobTypeWriteValue, DataVa,
	           0xcafebabedeadbeefULL, &raw);
	drvdmacache(DrvInval, g->datapa, 4096);
	print("gpufs: a64 write r %d raw %#ux js %#ux value %#llux\n",
	      r, raw, drd(&g->d, JSOFF(1, JsStatus)), *(u64 *)g->data);
	r = runjob(g, 1, JobVa, 0x83700, 1, 3, 0, 0, &raw);
	print("gpufs: a64 flush r %d raw %#ux js %#ux\n", r, raw,
	      drd(&g->d, JSOFF(1, JsStatus)));
}

/* Dump the whole GPU control block so it can be compared with Linux. */
static void
dumpregs(Gpu *g)
{
	int i;

	for(i = 0; i < 0x100; i += 8)
		print("gpufs: r%03ux %#ux %#ux %#ux %#ux %#ux %#ux %#ux %#ux\n", i,
		      drd(&g->d, i + 0), drd(&g->d, i + 4), drd(&g->d, i + 8),
		      drd(&g->d, i + 12), drd(&g->d, i + 16), drd(&g->d, i + 20),
		      drd(&g->d, i + 24), drd(&g->d, i + 28));
}

/*
 * Set the GPU power-manager override bits that the Amlogic quirk applies to
 * every Mali-G52, then run the NULL job again.
 */
static void
pwrtest(Gpu *g)
{
	u32 raw;
	int k;

	print("gpufs: pwr before key %#ux ovr0 %#ux ovr1 %#ux\n",
	      drd(&g->d, 0x50), drd(&g->d, 0x54), drd(&g->d, 0x58));
	if(mmuinit(g) < 0)
		return;
	softreset(g);
	drw(&g->d, 0x50, 0x2968A819);
	drw(&g->d, 0x58, 0xfff | (0x20 << 16));
	print("gpufs: pwr after key %#ux ovr0 %#ux ovr1 %#ux\n",
	      drd(&g->d, 0x50), drd(&g->d, 0x54), drd(&g->d, 0x58));
	if(gpupower(g) < 0 || mmuprogram(g) < 0)
		return;

	memset(g->job, 0, 4096);
	g->job[4] = 1 | (1 << 1) | (1 << 16);
	drvdmacache(DrvClean, g->jobpa, 4096);
	gpucacheflush(g);

	drw(&g->d, JobIntClear, 0x70007);
	drw(&g->d, JobIntMask, 0x70007);
	drw(&g->d, JSOFF(1, JsHeadLo), (u32)JobVa);
	drw(&g->d, JSOFF(1, JsHeadHi), (u32)((u64)JobVa >> 32));
	drw(&g->d, JSOFF(1, JsAffinityLo), 1);
	drw(&g->d, JSOFF(1, JsAffinityHi), 0);
	drw(&g->d, JSOFF(1, JsConfig), 0x87300);
	drw(&g->d, JSOFF(1, JsFlushId), drd(&g->d, GpuLatestFlush));
	drw(&g->d, JSOFF(1, JsCommand), 1);

	raw = 0;
	for(k = 0; k < 100; k++){
		raw = drd(&g->d, JobIntRawstat);
		if(raw & ((1 << 1) | (1 << 17)))
			break;
		sleep(1);
	}
	print("gpufs: pwr raw %#ux js %#ux asfault %#ux\n", raw,
	      drd(&g->d, JSOFF(1, JsStatus)), drd(&g->d, ASOFF(0, 0x1c)));
	drw(&g->d, JobIntClear, 0x70007);
}

/*
 * A GPU clear into the VOP scanout buffer.
 *
 * Shape taken from a chain captured from Linux panfrost on this board: one
 * FRAGMENT job on JS0 with Next=0, and a framebuffer descriptor whose colour
 * render target carries the clear value.  No tiler job and no shader.
 */
typedef struct Fbinfo Fbinfo;
struct Fbinfo {
	u64 pa;
	u64 size;
	u32 width;
	u32 height;
	u32 stride;
};

static int
fbinfo(Fbinfo *fi)
{
	char buf[128];
	int fd, n;
	char *p;

	fi->pa = 0;
	fi->size = 0;
	fi->width = fi->height = fi->stride = 0;

	fd = open("/dev/display/phys", OREAD);
	if(fd < 0)
		return -1;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		return -1;
	buf[n] = 0;
	fi->pa = strtoull(buf, &p, 0);
	fi->size = strtoull(p, nil, 0);

	fd = open("/dev/display/ctl", OREAD);
	if(fd < 0)
		return -1;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		return -1;
	buf[n] = 0;
	if(strncmp(buf, "vop 1 xrgb8888 ", 15) != 0)
		return -1;
	p = buf + 15;
	fi->width = strtoul(p, &p, 10);
	fi->height = strtoul(p, &p, 10);
	fi->stride = strtoul(p, &p, 10);
	return (fi->pa != 0 && fi->size != 0 && fi->width != 0) ? 0 : -1;
}

static int mapgpu(Gpu *g, u64 va, u64 pa, u64 size);

/* Submit a job header that is already in GPU memory. */
static int
submitat(Gpu *g, int slot, u64 head, u32 cfg, u32 *rawp, u32 *statp)
{
	u32 raw;
	int i, done, err;

	done = err = 0;
	raw = 0;
	gpucacheflush(g);
	drw(&g->d, JobIntClear, 0x70007);
	drw(&g->d, JobIntMask, 0x70007);
	drw(&g->d, JSOFF(slot, JsFlushId), drd(&g->d, GpuLatestFlush));
	drw(&g->d, JSOFF(slot, JsHeadLo), (u32)head);
	drw(&g->d, JSOFF(slot, JsHeadHi), (u32)(head >> 32));
	drw(&g->d, JSOFF(slot, JsAffinityLo), 1);
	drw(&g->d, JSOFF(slot, JsAffinityHi), 0);
	drw(&g->d, JSOFF(slot, JsConfig), cfg);
	drw(&g->d, JSOFF(slot, JsCommand), 1);
	for(i = 0; i < 2000; i++){
		raw = drd(&g->d, JobIntRawstat);
		if(raw & (1 << slot)){ done = 1; break; }
		if(raw & (1 << (slot + 16))){ err = 1; break; }
		sleep(1);
	}
	if(rawp != nil)
		*rawp = raw;
	if(statp != nil)
		*statp = drd(&g->d, JSOFF(slot, JsStatus));
	drw(&g->d, JobIntClear, (1 << slot) | (1 << (slot + 16)));
	return done ? 0 : (err ? -1 : -2);
}

/*
 * Replay the captured Mesa draw chain verbatim.
 *
 * The pool mirrors the captured virtual layout, so every pointer the
 * descriptors carry is still valid: the sample table at 0xa000000, the
 * vertex renderer state at 0xa002000, both shaders at 0xa003000, the render
 * target at 0xa004000, the attribute buffer at 0xa008000 and the whole
 * descriptor block (framebuffer, draws, tiler context, both job headers) at
 * 0xa009000.  The tiler heap gets its own 128 KiB at 0x2000000.
 *
 * The heap is GPU-initialised: panfrost creates it PAN_BO_INVISIBLE and
 * writes nothing into it, so a zeroed buffer with bottom == base and a zero
 * polygon list is what the driver hands over.
 */
static void
gpudraw(Gpu *g)
{
	u32 *d, *rt, raw, stat;
	u8 *pool;
	u64 poolpa;
	int r;

	if(mmuinit(g) < 0)
		return;
	softreset(g);
	if(gpupower(g) < 0 || mmuprogram(g) < 0)
		return;

	if(g->drawbuf == nil){
		g->drawbuf = drvdma(&g->d, 0x40000, 4096, &g->drawpa);
		if(g->drawbuf == nil){
			print("gpufs: draw: no pool\n");
			return;
		}
	}
	if(g->heapbuf == nil){
		g->heapbuf = drvdma(&g->d, Heapsize, 4096, &g->heappa);
		if(g->heapbuf == nil){
			print("gpufs: draw: no tiler heap\n");
			return;
		}
	}
	pool = g->drawbuf;
	poolpa = g->drawpa;
	memset(pool, 0, 0x40000);

	memmove(pool + 0x0000, draw_000000, sizeof draw_000000);
	memmove(pool + 0x2000, draw_002000, sizeof draw_002000);
	memmove(pool + 0x3000, draw_003000, sizeof draw_003000);
	memmove(pool + 0x8000, draw_008000, sizeof draw_008000);
	memmove(pool + 0x9000, draw_009000, sizeof draw_009000);

	d = (u32 *)(pool + 0x9000);

	/* TILER_HEAP at +0x480.  Mesa hands the hardware size/base/bottom/top
	 * and nothing else; the heap is GPU-initialised. */
	d[0x480/4 + 0] = 0;
	d[0x480/4 + 1] = Heapsize;		/* size */
	d[0x480/4 + 2] = (u32)HeapVa;		/* base */
	d[0x480/4 + 3] = (u32)((u64)HeapVa >> 32);
	d[0x480/4 + 4] = (u32)HeapVa;		/* bottom */
	d[0x480/4 + 5] = (u32)((u64)HeapVa >> 32);
	d[0x480/4 + 6] = (u32)((u64)HeapVa + Heapsize);	/* top */
	d[0x480/4 + 7] = (u32)(((u64)HeapVa + Heapsize) >> 32);

	/* Move the framebuffer clear colour off the shader's red, so a red
	 * pixel can only come from the fragment shader. */
	d[0xB0/4 + 0] = 0xff00ff00;
	d[0xB0/4 + 1] = 0xff00ff00;
	d[0xB0/4 + 2] = 0xff00ff00;
	d[0xB0/4 + 3] = 0xff00ff00;

	/* TILER_CONTEXT at +0x4c0: the tiler writes polygon_list itself. */
	d[0x4C0/4 + 0] = 0;
	d[0x4C0/4 + 1] = 0;

	/* Fresh exception status for both job headers. */
	d[0x300/4] = 0;
	d[0x580/4] = 0;

	/* The descriptor region is one contiguous 2 MiB page-table entry. */
	memset(g->heapbuf, 0, Heapsize);
	if(mapgpu(g, 0xa000000, poolpa, 0x1a000) < 0 ||
	   mapgpu(g, HeapVa, g->heappa, Heapsize) < 0){
		print("gpufs: draw: cannot map the replay pool\n");
		return;
	}
	if(mmucmd(g, AsCmdFlushMem) < 0 || mmucmd(g, AsCmdUpdate) < 0){
		print("gpufs: draw: mmu update failed\n");
		return;
	}
	drvdmacache(DrvClean, poolpa, 0x1a000);
	drvdmacache(DrvClean, g->heappa, Heapsize);

	r = submitat(g, 1, 0xa009300, 0x87b00, &raw, &stat);
	print("gpufs: draw tiler %s raw %#ux js %#ux as %#ux addr %#llux\n",
	      r == 0 ? "PASS" : (r == -1 ? "FAIL" : "TIMEOUT"), raw, stat,
	      drd(&g->d, ASOFF(0, 0x1c)),
	      ((u64)drd(&g->d, ASOFF(0, 0x24)) << 32) | drd(&g->d, ASOFF(0, 0x20)));

	r = submitat(g, 0, 0xa009580, 0x87b00, &raw, &stat);
	print("gpufs: draw frag %s raw %#ux js %#ux as %#ux addr %#llux\n",
	      r == 0 ? "PASS" : (r == -1 ? "FAIL" : "TIMEOUT"), raw, stat,
	      drd(&g->d, ASOFF(0, 0x1c)),
	      ((u64)drd(&g->d, ASOFF(0, 0x24)) << 32) | drd(&g->d, ASOFF(0, 0x20)));

	drvdmacache(DrvInval, poolpa + 0x4000, 0x4000);
	rt = (u32 *)(pool + 0x4000);
	print("gpufs: draw pixel %#ux %#ux (row 1 %#ux, row 63 %#ux)\n",
	      rt[0], rt[1], rt[64], rt[63 * 64]);
}

/*
 * Map a physical range at va with 4 KiB leaves under 2 MiB tables.
 *
 * Mali LPAE tables hold 512 *64-bit* descriptors, so the level-3 pages must
 * be filled as u64.  Filling them as u32 writes half-width, double-spaced
 * entries: each hardware descriptor then covers 8 KiB of VA pointing at a
 * page twice as far along in PA, and the table only covers half the range.
 */
static int
mapgpu(Gpu *g, u64 va, u64 pa, u64 size)
{
	u64 tabs, tabs_pa, off;
	u64 *l3;
	void *tabs_cpu;
	int i, j, n, l2;

	n = (size + 2*1024*1024 - 1) / (2*1024*1024);
	l2 = (va >> 21) & 0x1ff;
	if(n <= 0 || n > 8 || l2 + n > 512 || size > FbMaxPages*4096)
		return -1;
	tabs_cpu = drvdma(&g->d, 4096*n, 4096, &tabs_pa);
	if(tabs_cpu == nil)
		return -1;
	tabs = tabs_pa;
	for(i = 0; i < n; i++){
		l3 = (u64 *)((u8 *)tabs_cpu + i*4096);
		memset(l3, 0, 4096);
		g->pt2[l2 + i] = tablepte(tabs + i*4096);
		for(j = 0; j < 512; j++){
			off = (u64)i*2*1024*1024 + (u64)j*4096;
			if(off >= size)
				break;
			l3[j] = pagepte(pa + off);
		}
		drvdmacache(DrvClean, tabs + i*4096, 4096);
	}
	drvdmacache(DrvClean, g->l2pa, 4096);
	return 0;
}

/* Read a few words back out of the scanout buffer through /dev/display/data. */
static void
fbpeek(u64 off)
{
	u32 v[4];
	int fd, n;

	v[0] = v[1] = v[2] = v[3] = 0;
	fd = open("/dev/display/data", OREAD);
	if(fd < 0){
		print("gpufs: peek offset %llud: cannot open: %r\n", off);
		return;
	}
	n = pread(fd, v, sizeof v, off);
	close(fd);
	print("gpufs: peek %llud n %d: %#ux %#ux %#ux %#ux\n",
	      off, n, v[0], v[1], v[2], v[3]);
}

static void
gpuclr(Gpu *g, u32 color, u32 cw, u32 ch, int tiled, u32 cfgarg, int local,
       int rx, int ry)
{
	Fbinfo fi;
	u32 *fbd, *rt, *job, *pl, *sl, raw, cfg;
	u64 fbdesc, fbdva, rtva, slva, hva;
	int i, slot = 0, done, err;
	u32 tw, th, bminx, bminy, bmaxx, bmaxy;

	USED(tiled);
	if(fbinfo(&fi) < 0){
		print("gpufs: clear: no scanout buffer\n");
		return;
	}
	if(rx < 0 || ry < 0 || (u32)rx >= fi.width || (u32)ry >= fi.height ||
	   rx + (int)(cw ? cw : fi.width) > (int)fi.width ||
	   ry + (int)(ch ? ch : fi.height) > (int)fi.height){
		rx = 0;
		ry = 0;
		cw = 0;
		ch = 0;
	}
	if(cw == 0 || ch == 0 || local){
		cw = cw ? cw : fi.width;
		ch = ch ? ch : fi.height;
	}
	if(local){
		fi.width = cw;
		fi.height = ch;
	}
	if(local){
		bminx = 0;
		bminy = 0;
	}
	else{
		bminx = (u32)rx;
		bminy = (u32)ry;
	}
	bmaxx = bminx + cw - 1;
	bmaxy = bminy + ch - 1;
	print("gpufs: clear pa %#llux size %#llux %ud x %ud stride %ud rect %d,%d %ud x %ud\n",
	      fi.pa, fi.size, fi.width, fi.height, fi.stride, rx, ry, cw, ch);

	if(mmuinit(g) < 0)
		return;
	softreset(g);
	if(gpupower(g) < 0 || mmuprogram(g) < 0)
		return;
	if(local){
		if(g->locbuf == nil){
			g->locbuf = drvdma(&g->d, 65536, 4096, &g->locpa);
			if(g->locbuf == nil){
				print("gpufs: clear: no local buffer\n");
				return;
			}
		}
		memset(g->locbuf, 0xee, 65536);
		if(mapgpu(g, LocalVa, g->locpa, 65536) < 0){
			print("gpufs: clear: cannot map local buffer\n");
			return;
		}
		fi.width = cw ? cw : 64;
		fi.height = ch ? ch : 64;
	}else if(mapgpu(g, FbVa, fi.pa, fi.size) < 0){
		print("gpufs: clear: cannot map scanout buffer\n");
		return;
	}
	if(mmucmd(g, AsCmdFlushMem) < 0 || mmucmd(g, AsCmdUpdate) < 0){
		print("gpufs: clear: mmu update failed\n");
		return;
	}

	memset(g->job, 0, 4096);
	memset(g->data, 0, 4096);

	/*
	 * Two layouts.  Modes 0 and 1 pack the framebuffer descriptor at the
	 * start of the job page with the job header at +0xc0.  Mode 2 gives
	 * the descriptor its own page at DataVa and starts the job at the
	 * head of the job page, which is how Linux lays the same chain out.
	 */
	if(local == 2){
		fbd = g->data;
		fbdva = DataVa;
		slva = DataVa + 0x400;
		hva = JobVa;
	}else{
		fbd = g->job;
		fbdva = JobVa;
		slva = DataVa;
		hva = JobVa + 0xc0;
	}
	rtva = local ? LocalVa : FbVa;

	/* Sample-locations table: (0,0) then 31 copies of (-128,-128). */
	sl = (u32 *)((u8 *)g->data + (local == 2 ? 0x400 : 0));
	for(i = 0; i < 32; i++)
		sl[i] = i == 0 ? 0 : 0x8080;
	drvdmacache(DrvClean, g->datapa, 4096);

	/* Framebuffer parameters, 64 bytes. */
	fbd[0] = 0;				/* pre/post frame: never */
	fbd[4] = (u32)slva;
	fbd[5] = (u32)(slva >> 32);
	fbd[8] = (fi.width - 1) | ((fi.height - 1) << 16);
	fbd[9] = bminx | (bminy << 16);		/* bound min */
	fbd[10] = bmaxx | (bmaxy << 16);	/* bound max */
	/* log2(1 sample), single pattern, MINUS_180_IN_0_OUT, log2(256),
	 * no downsample, rt_count-1, cbuf_alloc >> 10. */
	fbd[11] = (0 << 0) | (0 << 3) | (2 << 6) | (8 << 9) |
		  (0 << 13) | (0 << 16) | (0 << 19) | (1 << 24);
	/* Z internal format is D24 (1) even with no depth buffer; Linux's
	 * captured FBD has 0x00010000 here.  No ZS enables, no CRC. */
	fbd[12] = 1 << 16;
	fbd[13] = 0;				/* z clear */
	fbd[14] = 0;				/* tiler */
	fbd[15] = 0;

	/* Colour render target at 0x80, linear.  The captured Linux FBD
	 * uses row stride = width*4 and surface stride 0. */
	rt = fbd + 0x80/4;
	rt[0] = (1 << 26);			/* internal format R8G8B8A8 */
	rt[1] = (1 << 0) |			/* write enable */
		(19 << 3) |			/* writeback R8G8B8A8 */
		(2 << 8) |			/* block format linear */
		(0 << 12) |			/* single sample */
		(0 << 14) |			/* sRGB */
		(1 << 15) |			/* dithering */
		(1672 << 16) |			/* swizzle RGBA */
		(1 << 31);			/* clean tile write */
	rt[8] = (u32)rtva;
	rt[9] = (u32)(rtva >> 32);
	/* Row stride: the local scratch buffer is tight, the scanout buffer
	 * is not, so a partial clear must keep the real stride. */
	rt[10] = local ? (u32)cw * 4 : (u32)fi.stride;
	rt[11] = 0;				/* surface stride */
	rt[12] = color;
	rt[13] = color;
	rt[14] = color;
	rt[15] = color;

	/* Fragment job: header then payload, 64 bytes. */
	job = g->job + (local == 2 ? 0 : 0xc0/4);
	fbdesc = fbdva | 1;			/* type=1, no CRC, 1 RT */
	job[4] = 1 | (9 << 1) | (1 << 16);	/* is64b, FRAGMENT, index 1 */
	job[6] = 0;
	job[7] = 0;
	pl = job + 8;
	/* The job walks tiles, so the payload range is in 16x16 tiles. */
	tw = (bmaxx >> TileShift) - (bminx >> TileShift) + 1;
	th = (bmaxy >> TileShift) - (bminy >> TileShift) + 1;
	pl[0] = (bminx >> TileShift) | ((bminy >> TileShift) << 16);
	pl[1] = ((bmaxx >> TileShift) | ((bmaxy >> TileShift) << 16));
	pl[2] = (u32)fbdesc;
	pl[3] = (u32)(fbdesc >> 32);
	pl[4] = 0;
	pl[5] = 0;
	pl[6] = 0;
	pl[7] = 0;

	drvdmacache(DrvClean, g->jobpa, 4096);
	drvdmacache(DrvClean, g->datapa, 4096);
	gpucacheflush(g);

	/* Linux's post-submit JS0_CONFIG reads back 0x7b00: AS 0,
	 * pri 8, start/end clean+invalidate, flush reduction and the
	 * job-chain flag. */
	cfg = cfgarg ? cfgarg : 0x87b00;
	/* Linux re-enables the address space for every job. */
	if(mmuprogram(g) < 0)
		print("gpufs: clear: mmu re-enable failed\n");
	drw(&g->d, JobIntClear, 0x70007);
	drw(&g->d, JobIntMask, 0x70007);
	drw(&g->d, JSOFF(slot, JsHeadLo), (u32)hva);
	drw(&g->d, JSOFF(slot, JsHeadHi), (u32)(hva >> 32));
	drw(&g->d, JSOFF(slot, JsAffinityLo), 1);
	drw(&g->d, JSOFF(slot, JsAffinityHi), 0);
	drw(&g->d, JSOFF(slot, JsConfig), cfg);
	drw(&g->d, JSOFF(slot, JsFlushId), drd(&g->d, GpuLatestFlush));
	drw(&g->d, JSOFF(slot, JsCommand), 1);

	done = err = 0;
	raw = 0;
	for(i = 0; i < 1000; i++){
		raw = drd(&g->d, JobIntRawstat);
		if(raw & (1 << slot)){ done = 1; break; }
		if(raw & (1 << (slot + 16))){ err = 1; break; }
		sleep(1);
	}
	drw(&g->d, JobIntClear, (1 << slot) | (1 << (slot + 16)));
	print("gpufs: clear tiles %ud x %ud stride %ud %s raw %#ux js %#ux as %#ux addr %#llux\n",
	      tw, th, rt[10], done ? "PASS" : (err ? "FAIL" : "TIMEOUT"), raw,
	      drd(&g->d, JSOFF(slot, JsStatus)),
	      drd(&g->d, ASOFF(0, 0x1c)),
	      ((u64)drd(&g->d, ASOFF(0, 0x24)) << 32) | drd(&g->d, ASOFF(0, 0x20)));
	if(local){
		u32 *lw = g->locbuf;
		drvdmacache(DrvInval, g->locpa, 65536);
		print("gpufs: local pixel %#ux %s (also %#ux %#ux, last row %#ux)\n",
		      lw[0], lw[0] == color ? "PASS" : "FAIL",
		      lw[1], lw[cw],
		      lw[(ch - 1) * cw]);
	}
	if(!done){
		print("gpufs: fbd %#llux", fbdva);
		for(i = 0; i < 16; i++)
			print(" %#ux", fbd[i]);
		print("\ngpufs: rt %#llux", rtva);
		for(i = 0; i < 16; i++)
			print(" %#ux", rt[i]);
		print("\ngpufs: job %#llux", hva);
		for(i = 0; i < 16; i++)
			print(" %#ux", job[i]);
		print("\n");
	}
}

/*
 * Submit a NULL job with an arbitrary leaf attribute word, so the permission
 * and cache encodings can be swept on hardware.
 */
static void
leaftest(Gpu *g, u32 bits)
{
	u32 raw;
	int k;

	if(mmuinit(g) < 0)
		return;
	softreset(g);
	if(gpupower(g) < 0 || mmuprogram(g) < 0)
		return;

	g->pt3[(JobVa >> 12) & 0x1ff] = (g->jobpa & ~0xfffULL) | (u64)bits;
	g->pt3[(DataVa >> 12) & 0x1ff] = (g->datapa & ~0xfffULL) | (u64)bits;
	drvdmacache(DrvClean, g->l3pa, 4096);
	if(mmucmd(g, AsCmdFlushMem) < 0 || mmucmd(g, AsCmdUpdate) < 0)
		print("gpufs: leaf mmu update failed\n");
	print("gpufs: leaf %#ux -> pte %#llux\n", bits,
	      g->pt3[(JobVa >> 12) & 0x1ff]);

	memset(g->job, 0, 4096);
	g->job[4] = 1 | (1 << 1) | (1 << 16);
	drvdmacache(DrvClean, g->jobpa, 4096);
	gpucacheflush(g);

	drw(&g->d, JobIntClear, 0x70007);
	drw(&g->d, JobIntMask, 0x70007);
	drw(&g->d, JSOFF(1, JsHeadLo), (u32)JobVa);
	drw(&g->d, JSOFF(1, JsHeadHi), (u32)((u64)JobVa >> 32));
	drw(&g->d, JSOFF(1, JsAffinityLo), 1);
	drw(&g->d, JSOFF(1, JsAffinityHi), 0);
	drw(&g->d, JSOFF(1, JsConfig), 0x87300);
	drw(&g->d, JSOFF(1, JsFlushId), drd(&g->d, GpuLatestFlush));
	drw(&g->d, JSOFF(1, JsCommand), 1);

	raw = 0;
	for(k = 0; k < 100; k++){
		raw = drd(&g->d, JobIntRawstat);
		if(raw & ((1 << 1) | (1 << 17)))
			break;
		sleep(1);
	}
	print("gpufs: leaf %#ux raw %#ux js %#ux asfault %#ux addr %#llux mmuint %#ux\n",
	      bits, raw, drd(&g->d, JSOFF(1, JsStatus)),
	      drd(&g->d, ASOFF(0, 0x1c)),
	      ((u64)drd(&g->d, ASOFF(0, 0x24)) << 32) | drd(&g->d, ASOFF(0, 0x20)),
	      drd(&g->d, MmuIntRawstat));
	drw(&g->d, JobIntClear, 0x70007);
}

/*
 * Same job, but the job and data leaves use AttrIndx 0 (the implementation
 * defined/non-cacheable attribute) instead of 1 (write-allocate).
 */
static void
nctest(Gpu *g)
{
	u32 raw;
	u64 va;
	int k;

	if(mmuinit(g) < 0)
		return;
	softreset(g);
	if(gpupower(g) < 0 || mmuprogram(g) < 0)
		return;

	for(va = JobVa; va <= DataVa; va += 4096){
		u64 pa = va == JobVa ? g->jobpa : g->datapa;
		g->pt3[(va >> 12) & 0x1ff] = (pa & ~0xfffULL) | 0x2c1ULL;
	}
	drvdmacache(DrvClean, g->l3pa, 4096);
	if(mmucmd(g, AsCmdFlushMem) < 0 || mmucmd(g, AsCmdUpdate) < 0)
		print("gpufs: nc mmu update failed\n");
	print("gpufs: nc l3 %#llux %#llux memattr %#ux\n",
	      g->pt3[(JobVa >> 12) & 0x1ff], g->pt3[(DataVa >> 12) & 0x1ff],
	      drd(&g->d, ASOFF(0, AsMemattrLo)));

	memset(g->job, 0, 4096);
	g->job[4] = 1 | (1 << 1) | (1 << 16);
	drvdmacache(DrvClean, g->jobpa, 4096);
	gpucacheflush(g);

	drw(&g->d, JobIntClear, 0x70007);
	drw(&g->d, JobIntMask, 0x70007);
	drw(&g->d, JSOFF(1, JsHeadLo), (u32)JobVa);
	drw(&g->d, JSOFF(1, JsHeadHi), (u32)((u64)JobVa >> 32));
	drw(&g->d, JSOFF(1, JsAffinityLo), 1);
	drw(&g->d, JSOFF(1, JsAffinityHi), 0);
	drw(&g->d, JSOFF(1, JsConfig), 0x87300);
	drw(&g->d, JSOFF(1, JsFlushId), drd(&g->d, GpuLatestFlush));
	drw(&g->d, JSOFF(1, JsCommand), 1);

	raw = 0;
	for(k = 0; k < 100; k++){
		raw = drd(&g->d, JobIntRawstat);
		if(raw & ((1 << 1) | (1 << 17)))
			break;
		sleep(1);
	}
	print("gpufs: nc raw %#ux js %#ux asfault %#ux asaddr %#llux mmuint %#ux\n",
	      raw, drd(&g->d, JSOFF(1, JsStatus)), drd(&g->d, ASOFF(0, 0x1c)),
	      ((u64)drd(&g->d, ASOFF(0, 0x24)) << 32) | drd(&g->d, ASOFF(0, 0x20)),
	      drd(&g->d, MmuIntRawstat));
	drw(&g->d, JobIntClear, 0x70007);
}

/*
 * Submit a NULL job whose page-table entry has been cleared.  A translation
 * fault proves the job manager fetches the header through our tables; another
 * DATA_INVALID_FAULT means it never translates the head address at all.
 */
static void
unmaptest(Gpu *g)
{
	u32 raw;
	int k;

	if(mmuinit(g) < 0)
		return;
	softreset(g);
	if(gpupower(g) < 0 || mmuprogram(g) < 0)
		return;

	/* Drop the leaf for JobVa, keep DataVa mapped. */
	g->pt3[(JobVa >> 12) & 0x1ff] = 0;
	drvdmacache(DrvClean, g->l3pa, 4096);
	if(mmucmd(g, AsCmdFlushMem) < 0)
		print("gpufs: unmap flush failed\n");
	print("gpufs: unmap l3 %#llux (cleared)\n",
	      g->pt3[(JobVa >> 12) & 0x1ff]);

	memset(g->job, 0, 4096);
	g->job[4] = 1 | (1 << 1) | (1 << 16);
	drvdmacache(DrvClean, g->jobpa, 4096);
	gpucacheflush(g);

	drw(&g->d, JobIntClear, 0x70007);
	drw(&g->d, JobIntMask, 0x70007);
	drw(&g->d, JSOFF(1, JsHeadLo), (u32)JobVa);
	drw(&g->d, JSOFF(1, JsHeadHi), (u32)((u64)JobVa >> 32));
	drw(&g->d, JSOFF(1, JsAffinityLo), 1);
	drw(&g->d, JSOFF(1, JsAffinityHi), 0);
	drw(&g->d, JSOFF(1, JsConfig), 0x87300);
	drw(&g->d, JSOFF(1, JsFlushId), drd(&g->d, GpuLatestFlush));
	drw(&g->d, JSOFF(1, JsCommand), 1);

	raw = 0;
	for(k = 0; k < 100; k++){
		raw = drd(&g->d, JobIntRawstat);
		if(raw & ((1 << 1) | (1 << 17)))
			break;
		sleep(1);
	}
	print("gpufs: unmap raw %#ux js %#ux mmuint %#ux mmustat %#ux asfault %#ux asaddr %#llux\n",
	      raw, drd(&g->d, JSOFF(1, JsStatus)), drd(&g->d, MmuIntRawstat),
	      drd(&g->d, 0x200c), drd(&g->d, ASOFF(0, 0x1c)),
	      ((u64)drd(&g->d, ASOFF(0, 0x24)) << 32) | drd(&g->d, ASOFF(0, 0x20)));
	drw(&g->d, JobIntClear, 0x70007);
}

/*
 * Full state dump around one NULL submission.  The GPU writes the exception
 * status back into the job header on a fault, so reading the header after the
 * fault says whether the JM ever reached it.
 */
static void
diagtest(Gpu *g, int slot, u32 type, u32 cfg, u32 hdr)
{
	u32 raw;
	int i, a, k;

	if(slot < 0 || slot > 2)
		slot = 1;
	if(mmuinit(g) < 0)
		return;

	/* Soft reset clears JS_STATUS, then power and MMU are reprogrammed. */
	softreset(g);
	if(gpupower(g) < 0 || mmuprogram(g) < 0)
		return;

	print("gpufs: diag jmcfg %#ux jsf %#ux/%#ux/%#ux flush %#ux status %#ux\n",
	      drd(&g->d, GpuJmConfig), drd(&g->d, 0xc0), drd(&g->d, 0xc4),
	      drd(&g->d, 0xc8), drd(&g->d, GpuLatestFlush),
	      drd(&g->d, GpuStatus));
	print("gpufs: diag jobpa %#llux datapa %#llux pgd %#llux l2 %#llux l3 %#llux\n",
	      g->jobpa, g->datapa, g->pgd[0], g->pt2[(JobVa >> 21) & 0x1ff],
	      g->pt3[(JobVa >> 12) & 0x1ff]);
	print("gpufs: diag as0 transtab %#ux/%#ux memattr %#ux/%#ux transcfg %#ux/%#ux status %#ux\n",
	      drd(&g->d, ASOFF(0, AsTranstabLo)), drd(&g->d, ASOFF(0, AsTranstabHi)),
	      drd(&g->d, ASOFF(0, AsMemattrLo)), drd(&g->d, ASOFF(0, AsMemattrHi)),
	      drd(&g->d, ASOFF(0, AsTranscfgLo)), drd(&g->d, ASOFF(0, AsTranscfgHi)),
	      drd(&g->d, ASOFF(0, AsStatus)));
	for(i = 0; i < 3; i++)
		print("gpufs: diag pre slot %d status %#ux cmdnext %#ux cfg %#ux cfgnext %#ux headnext %#ux/%#ux tail %#ux/%#ux\n",
		      i, drd(&g->d, JSOFF(i, JsStatus)), drd(&g->d, JSOFF(i, 0x60)),
		      drd(&g->d, JSOFF(i, 0x18)), drd(&g->d, JSOFF(i, 0x58)),
		      drd(&g->d, JSOFF(i, 0x40)), drd(&g->d, JSOFF(i, 0x44)),
		      drd(&g->d, JSOFF(i, 0x08)), drd(&g->d, JSOFF(i, 0x0c)));

	memset(g->job, 0, 4096);
	g->job[4] = hdr != 0 ? hdr : (1 | (type << 1) | (1 << 16));
	drvdmacache(DrvClean, g->jobpa, 4096);
	/* If the clean did not reach RAM, this invalidate drops the only copy
	 * of the header and the read below returns zeros. */
	drvdmacache(DrvInval, g->jobpa, 4096);
	print("gpufs: diag header after clean+inval %#ux %#ux %#ux %#ux %#ux %#ux %#ux %#ux\n",
	      g->job[0], g->job[1], g->job[2], g->job[3],
	      g->job[4], g->job[5], g->job[6], g->job[7]);
	gpucacheflush(g);
	print("gpufs: diag type %d sent %#ux %#ux %#ux %#ux %#ux %#ux %#ux %#ux\n",
	      (int)type, g->job[0], g->job[1], g->job[2], g->job[3],
	      g->job[4], g->job[5], g->job[6], g->job[7]);

	drw(&g->d, JobIntClear, 0x70007);
	drw(&g->d, JobIntMask, 0x70007);
	drw(&g->d, JSOFF(slot, JsHeadLo), (u32)JobVa);
	drw(&g->d, JSOFF(slot, JsHeadHi), (u32)((u64)JobVa >> 32));
	drw(&g->d, JSOFF(slot, JsAffinityLo), 1);
	drw(&g->d, JSOFF(slot, JsAffinityHi), 0);
	drw(&g->d, JSOFF(slot, JsConfig), cfg);
	drw(&g->d, JSOFF(slot, JsFlushId), drd(&g->d, GpuLatestFlush));
	drw(&g->d, JSOFF(slot, JsCommand), 1);

	raw = 0;
	for(k = 0; k < 200; k++){
		raw = drd(&g->d, JobIntRawstat);
		if(raw & ((1 << slot) | (1 << (slot + 16))))
			break;
		sleep(1);
	}
	print("gpufs: diag cfg %#ux slot %d raw %#ux status %#ux cmdnext %#ux cmd %#ux cfg %#ux cfgnext %#ux\n",
	      cfg, slot, raw, drd(&g->d, JSOFF(slot, JsStatus)),
	      drd(&g->d, JSOFF(slot, 0x60)), drd(&g->d, JSOFF(slot, 0x20)),
	      drd(&g->d, JSOFF(slot, 0x18)), drd(&g->d, JSOFF(slot, 0x58)));
	print("gpufs: diag slot %d head %#ux/%#ux tail %#ux/%#ux headnext %#ux/%#ux\n",
	      slot, drd(&g->d, JSOFF(slot, 0x00)), drd(&g->d, JSOFF(slot, 0x04)),
	      drd(&g->d, JSOFF(slot, 0x08)), drd(&g->d, JSOFF(slot, 0x0c)),
	      drd(&g->d, JSOFF(slot, 0x40)), drd(&g->d, JSOFF(slot, 0x44)));
	print("gpufs: diag got %#ux %#ux %#ux %#ux %#ux %#ux %#ux %#ux\n",
	      g->job[0], g->job[1], g->job[2], g->job[3],
	      g->job[4], g->job[5], g->job[6], g->job[7]);
	print("gpufs: diag gpuint %#ux stat %#ux fault %#ux addr %#llux mmuint %#ux stat %#ux\n",
	      drd(&g->d, GpuIntRawstat), drd(&g->d, GpuIntStat),
	      drd(&g->d, GpuFaultStatus),
	      ((u64)drd(&g->d, GpuFaultAddrHi) << 32) | drd(&g->d, GpuFaultAddrLo),
	      drd(&g->d, MmuIntRawstat), drd(&g->d, 0x200c));
	for(a = 0; a < 8; a++)
		print("gpufs: diag as %d fault %#ux status %#ux addr %#llux\n", a,
		      drd(&g->d, ASOFF(a, 0x1c)), drd(&g->d, ASOFF(a, AsStatus)),
		      ((u64)drd(&g->d, ASOFF(a, 0x24)) << 32) | drd(&g->d, ASOFF(a, 0x20)));
	drw(&g->d, JobIntClear, (1 << slot) | (1 << (slot + 16)));
}

/* Two NULL jobs linked by Next: the minimal chain Linux would submit. */
static void
chaintest(Gpu *g, u32 cfg)
{
	u32 *j2, raw;
	int i, done, err;

	if(mmuinit(g) < 0 || mmuprogram_a64(g) < 0)
		return;

	memset(g->job, 0, 4096);
	g->job[4] = 1 | (1 << 1) | (1 << 16);	/* NULL, index 1 */
	g->job[6] = JobVa + 0x40;		/* Next */
	g->job[7] = 0;
	j2 = (u32 *)((u8 *)g->job + 0x40);
	j2[4] = 1 | (1 << 1) | (2 << 16);	/* NULL, index 2 */
	j2[6] = 0;
	j2[7] = 0;
	drvdmacache(DrvClean, g->jobpa, 4096);
	gpucacheflush(g);

	drw(&g->d, JobIntClear, 0x70007);
	drw(&g->d, JobIntMask, 0x70007);
	drw(&g->d, JSOFF(1, JsFlushId), drd(&g->d, GpuLatestFlush));
	drw(&g->d, JSOFF(1, JsHeadLo), (u32)JobVa);
	drw(&g->d, JSOFF(1, JsHeadHi), (u32)((u64)JobVa >> 32));
	drw(&g->d, JSOFF(1, JsAffinityLo), 1);
	drw(&g->d, JSOFF(1, JsAffinityHi), 0);
	drw(&g->d, JSOFF(1, JsConfig), cfg | (1 << 14));
	drw(&g->d, JSOFF(1, JsCommand), 1);

	done = err = 0;
	for(i = 0; i < 1000; i++){
		raw = drd(&g->d, JobIntRawstat);
		if(raw & (1 << 1)){ done = 1; break; }
		if(raw & (1 << 17)){ err = 1; break; }
		sleep(1);
	}
	drw(&g->d, JobIntClear, 0x20002);
	print("gpufs: chain cfg %#ux %s raw %#ux js %#ux as %#ux addr %#llux\n",
	      cfg, done ? "PASS" : (err ? "FAIL" : "TIMEOUT"), raw,
	      drd(&g->d, JSOFF(1, JsStatus)),
	      drd(&g->d, ASOFF(0, 0x1c)),
	      ((u64)drd(&g->d, ASOFF(0, 0x24)) << 32) | drd(&g->d, ASOFF(0, 0x20)));
}

/* A NULL job on one slot: no payload, no shader, just the queue. */
static void
nulltest(Gpu *g, int slot)
{
	u32 raw;
	int i, done, err;

	if(slot < 0 || slot > 2)
		return;
	if(mmuinit(g) < 0 || mmuprogram(g) < 0)
		return;

	done = err = 0;
	memset(g->job, 0, 4096);
	g->job[4] = 1 | (1 << 1) | (1 << 16);	/* NULL, index 1 */
	drvdmacache(DrvClean, g->jobpa, 4096);
	gpucacheflush(g);

	drw(&g->d, JobIntClear, 0xffffffff);
	drw(&g->d, JobIntMask, 0);
	drw(&g->d, JSOFF(slot, JsHeadLo), (u32)JobVa);
	drw(&g->d, JSOFF(slot, JsHeadHi), (u32)((u64)JobVa >> 32));
	drw(&g->d, JSOFF(slot, JsAffinityLo), 1);
	drw(&g->d, JSOFF(slot, JsAffinityHi), 0);
	drw(&g->d, JSOFF(slot, JsConfig), 0x87b00);
	drw(&g->d, JSOFF(slot, JsCommand), 1);

	for(i = 0; i < 1000; i++){
		raw = drd(&g->d, JobIntRawstat);
		if(raw & (1 << slot)){
			done = 1;
			break;
		}
		if(raw & (1 << (slot + 16))){
			err = 1;
			break;
		}
		sleep(1);
	}
	drw(&g->d, JobIntClear, (1 << slot) | (1 << (slot + 16)));
	print("gpufs: null slot %d %s raw %#ux js %#ux head %#ux tail %#ux\n",
	      slot, done ? "PASS" : (err ? "FAIL" : "TIMEOUT"), raw,
	      drd(&g->d, JSOFF(slot, JsStatus)),
	      drd(&g->d, JSOFF(slot, 0x00)), drd(&g->d, JSOFF(slot, 0x08)));
}

/*
 * The first job: a WRITE_VALUE header at JobVa, writing 0xcafebabedeadbeef
 * to DataVa.  No shader, no tiler; this proves the page tables, the job
 * manager and the completion path before any compiler work.
 */
static void
jobtest(Gpu *g)
{
	u32 raw, cfg;
	int i, done, err;

	done = err = 0;
	g->jobok = 0;
	g->jobval = 0;
	print("gpufs: test mmuinit\n");
	if(mmuinit(g) < 0)
		return;
	print("gpufs: test mmuprogram\n");
	if(mmuprogram(g) < 0)
		return;
	print("gpufs: test submit slot %d\n", JobSlot);

	/* Header: Is64b, Type=WRITE_VALUE.  Payload at word 8. */
	g->job[4] = 1 | (JobTypeWriteValue << 1) | (1 << 16);
	g->job[8] = DataVa;
	g->job[9] = (u64)DataVa >> 32;
	g->job[10] = WriteValueImmediate64;
	g->job[12] = 0xdeadbeef;
	g->job[13] = 0xcafebabe;
	*(u64 *)g->data = 0;
	drvdmacache(DrvClean, g->jobpa, 4096);
	drvdmacache(DrvClean, g->datapa, 4096);

	drw(&g->d, JobIntClear, 0xffffffff);
	drw(&g->d, JobIntMask, 0);
	drw(&g->d, JSOFF(JobSlot, JsHeadLo), (u32)JobVa);
	drw(&g->d, JSOFF(JobSlot, JsHeadHi), (u32)((u64)JobVa >> 32));
	drw(&g->d, JSOFF(JobSlot, JsAffinityLo), 1);
	drw(&g->d, JSOFF(JobSlot, JsAffinityHi), 0);
	cfg = (8 << 16) | (3 << 8) | (3 << 12) | (1 << 14) | (1 << 11);
	drw(&g->d, JSOFF(JobSlot, JsConfig), cfg);
	drw(&g->d, JSOFF(JobSlot, JsCommand), 1);
	print("gpufs: test poll\n");

	for(i = 0; i < 1000; i++){
		raw = drd(&g->d, JobIntRawstat);
		if(raw & (1 << JobSlot)){
			done = 1;
			break;
		}
		if(raw & (1 << (JobSlot + 16))){
			err = 1;
			break;
		}
		sleep(1);
	}
	drw(&g->d, JobIntClear, (1 << JobSlot) | (1 << (JobSlot + 16)));

	if(!done && !err){
		snprint(g->joberr, sizeof g->joberr,
		        "job timeout raw %#ux js %#ux mmu %#ux as %#ux",
		        raw, drd(&g->d, JSOFF(JobSlot, JsStatus)),
		        drd(&g->d, MmuIntRawstat),
		        drd(&g->d, ASOFF(0, AsStatus)));
		setwhy(g, "%s", g->joberr);
		gpudump(g);
		return;
	}
	if(err){
		snprint(g->joberr, sizeof g->joberr,
		        "job error raw %#ux js %#ux head %#ux tail %#ux mmu %#ux as %#ux",
		        raw, drd(&g->d, JSOFF(JobSlot, JsStatus)),
		        drd(&g->d, JSOFF(JobSlot, 0x00)), drd(&g->d, JSOFF(JobSlot, 0x08)),
		        drd(&g->d, MmuIntRawstat),
		        drd(&g->d, ASOFF(0, AsStatus)));
		setwhy(g, "%s", g->joberr);
		gpudump(g);
		return;
	}

	drvdmacache(DrvInval, g->datapa, 4096);
	g->jobval = *(u64 *)g->data;
	g->jobok = g->jobval == 0xcafebabedeadbeefULL;
	if(!g->jobok){
		snprint(g->joberr, sizeof g->joberr,
		        "job wrote %#llux", g->jobval);
		setwhy(g, "%s", g->joberr);
	}
}

static void
fsread(Req *r, void *aux)
{
	Gpu *g;
	char buf[1024];
	char *name;

	g = aux;
	name = drvfile(r)->name;
	if(strcmp(name, "status") == 0){
		qlock(&g->lk);
		statustext(g, buf, sizeof buf);
		qunlock(&g->lk);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	if(strcmp(name, "regs") == 0){
		qlock(&g->lk);
		regstext(g, buf, sizeof buf);
		qunlock(&g->lk);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	respond(r, "read not permitted");
}

static void
fswrite(Req *r, void *aux)
{
	Gpu *g;
	char cmd[32];
	int n;

	g = aux;
	n = r->ifcall.count;
	if(n > (int)sizeof cmd - 1)
		n = sizeof cmd - 1;
	memmove(cmd, r->ifcall.data, n);
	cmd[n] = 0;
	while(n > 0 && (cmd[n-1] == '\n' || cmd[n-1] == '\r' ||
	        cmd[n-1] == ' ' || cmd[n-1] == '\t'))
		cmd[--n] = 0;

	qlock(&g->lk);
	if(strcmp(cmd, "power on") == 0){
		if(g->powered || pmuon(g) == 0){
			ident(g);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	if(strcmp(cmd, "power off") == 0){
		pmuoff(g);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		qunlock(&g->lk);
		return;
	}
	if(strcmp(cmd, "reset") == 0){
		if(g->powered){
			softreset(g);
			irqmask(g);
			ident(g);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, "power is off");
		qunlock(&g->lk);
		return;
	}
	if(strcmp(cmd, "probe") == 0){
		if(g->powered)
			ident(g);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		qunlock(&g->lk);
		return;
	}
	if(strcmp(cmd, "matrix") == 0){
		print("gpufs: ctl matrix\n");
		if(g->powered || pmuon(g) == 0){
			if(gpupower(g) == 0)
				identitymatrix(g);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	if(strcmp(cmd, "dump") == 0){
		print("gpufs: ctl dump\n");
		dumpregs(g);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		qunlock(&g->lk);
		return;
	}
	if(strncmp(cmd, "peek", 4) == 0){
		print("gpufs: ctl peek\n");
		fbpeek(cmd[4] == ' ' ? strtoull(cmd + 5, nil, 0) : 0);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		qunlock(&g->lk);
		return;
	}
	if(strcmp(cmd, "pwr") == 0){
		print("gpufs: ctl pwr\n");
		if(g->powered || pmuon(g) == 0){
			pwrtest(g);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	if(strncmp(cmd, "leaf", 4) == 0){
		u32 bits = 0x2c5;

		if(cmd[4] == ' ')
			bits = strtoul(cmd + 5, nil, 0);
		print("gpufs: ctl leaf %#ux\n", bits);
		if(g->powered || pmuon(g) == 0){
			leaftest(g, bits);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	if(strcmp(cmd, "nc") == 0){
		print("gpufs: ctl nc\n");
		if(g->powered || pmuon(g) == 0){
			nctest(g);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	if(strcmp(cmd, "unmap") == 0){
		print("gpufs: ctl unmap\n");
		if(g->powered || pmuon(g) == 0){
			unmaptest(g);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	if(strncmp(cmd, "clear", 5) == 0){
		u32 color = 0xff0000ff, cw = 0, ch = 0, tiled = 0, cfgarg = 0, local = 0;
		char *q;

		if(cmd[5] == ' '){
			color = strtoul(cmd + 6, &q, 0);
			cw = strtoul(q, &q, 0);
			ch = strtoul(q, &q, 0);
			tiled = strtoul(q, &q, 0);
			cfgarg = strtoul(q, &q, 0);
			local = strtoul(q, &q, 0);
		}
		print("gpufs: ctl clear %#ux %ud %ud tiled %ud cfg %#ux local %ud\n",
		      color, cw, ch, tiled, cfgarg, local);
		if(g->powered || pmuon(g) == 0){
			gpuclr(g, color, cw, ch, tiled, cfgarg, local, 0, 0);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	if(strncmp(cmd, "fill", 4) == 0){
		u32 color = 0xff00ff00;
		int fx = 0, fy = 0;
		u32 fw = 0, fh = 0;
		char *q;

		if(cmd[4] == ' '){
			fx = strtoul(cmd + 5, &q, 0);
			fy = strtoul(q, &q, 0);
			fw = strtoul(q, &q, 0);
			fh = strtoul(q, &q, 0);
			color = strtoul(q, &q, 0);
		}
		print("gpufs: ctl fill %d,%d %ud x %ud %#ux\n", fx, fy, fw, fh, color);
		if(g->powered || pmuon(g) == 0){
			gpuclr(g, color, fw, fh, 0, 0, 0, fx, fy);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	if(strcmp(cmd, "draw") == 0){
		print("gpufs: ctl draw\n");
		if(g->powered || pmuon(g) == 0){
			gpudraw(g);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	if(strncmp(cmd, "diag", 4) == 0){
		int slot;
		u32 type, cfg, hdr;
		char *p;

		slot = 1;
		type = 1;
		cfg = 0x87300;
		hdr = 0;
		p = cmd;
		while(*p != 0 && *p != ' ')
			p++;			/* end of "diag" */
		if(*p != 0){
			slot = atoi(p + 1);
			p++;			/* first digit of slot */
			while(*p >= '0' && *p <= '9')
				p++;
			if(*p == ' '){
				type = atoi(p + 1);
				p++;
				while(*p != 0 && *p != ' ')
					p++;
				if(*p == ' '){
					cfg = strtoul(p + 1, nil, 0);
					p++;
					while(*p != 0 && *p != ' ')
						p++;
					if(*p == ' ')
						hdr = strtoul(p + 1, nil, 0);
				}
			}
		}
		print("gpufs: ctl cmd <%s> slot %d type %d cfg %#ux hdr %#ux\n",
		      cmd, slot, (int)type, cfg, hdr);
		if(g->powered || pmuon(g) == 0){
			diagtest(g, slot, type, cfg, hdr);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	if(strcmp(cmd, "chain") == 0){
		print("gpufs: ctl chain\n");
		if(g->powered || pmuon(g) == 0){
			if(gpupower(g) == 0){
				chaintest(g, 0x87300);
				chaintest(g, 0x87b00);
			}
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	if(strcmp(cmd, "identity") == 0){
		print("gpufs: ctl identity\n");
		if(g->powered || pmuon(g) == 0){
			if(gpupower(g) == 0)
				identitytest(g);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	if(strncmp(cmd, "null", 4) == 0){
		int slot;

		slot = cmd[4] == ' ' ? atoi(cmd + 5) : 1;
		print("gpufs: ctl null slot %d\n", slot);
		if(g->powered || pmuon(g) == 0){
			if(gpupower(g) == 0)
				nulltest(g, slot);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	if(strcmp(cmd, "test") == 0){
		print("gpufs: ctl test power\n");
		if(g->powered || pmuon(g) == 0){
			if(gpupower(g) == 0){
				print("gpufs: ctl test job\n");
				jobtest(g);
			}
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
		}else
			respond(r, g->why);
		qunlock(&g->lk);
		return;
	}
	qunlock(&g->lk);
	respond(r, "unknown command");
}

static DrvFile files[] = {
	{ "status",	0444,	&gpu,	fsread,	nil },
	{ "regs",	0444,	&gpu,	fsread,	nil },
	{ "ctl",	0666,	&gpu,	nil,	fswrite },
};

static void
usage(void)
{
	fprint(2, "usage: gpufs [-i|-t]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	DrvSrv *s;
	int info, test;

	info = 0;
	test = 0;
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

	if(drvfind(&gpu.d, "gpu", 0) < 0){
		if(info){
			print("gpufs: no gpu in #P\n");
			exits("nogpu");
		}
		drvfail("gpufs: no GPU in #P");
	}
	if(drvmap(&gpu.d) < 0){
		setwhy(&gpu, "cannot map %s: %r", gpu.d.name);
	}else{
		gpu.mapped = 1;
		if(pmuon(&gpu) == 0){
			softreset(&gpu);
			irqmask(&gpu);
			ident(&gpu);
			if(test){
				print("gpufs: test power\n");
				if(gpupower(&gpu) == 0){
					print("gpufs: test mmu/job\n");
					jobtest(&gpu);
				}
			}
		}
	}

	if(info || test){
		char buf[1024];

		statustext(&gpu, buf, sizeof buf);
		print("%s", buf);
		if(test){
			if(gpu.jobok)
				print("HARVEY-ARM64 GPU JOB PASS value 0x%llux\n",
				      gpu.jobval);
			else
				print("HARVEY-ARM64 GPU JOB FAIL %s\n",
				      gpu.joberr[0] ? gpu.joberr :
				      (gpu.why[0] ? gpu.why : "not run"));
		}
		exits(gpu.responded ? nil : "noprobe");
	}

	if(gpu.responded)
		print("HARVEY-ARM64 GPU PROBE %s id 0x%ux rev 0x%ux\n",
		      model(gpu.id), gpu.id, gpu.rev);
	else
		print("HARVEY-ARM64 GPU PROBE FAIL %s\n",
		      gpu.why[0] ? gpu.why : "no response");
	s = drvsrv("gpu", files, nelem(files));
	if(s == nil)
		drvfail("gpufs: cannot build 9P tree");
	drvsrvpost(s);
	drvhealthrun("gpufs");
}
