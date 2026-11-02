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

	GpuCmdSoftReset = 0x01,
	GpuIrqResetCompleted = 1<<8,
	GpuIrqMaskAll	= 0x00030f87,	/* fault, multiple, reset, power,
					 * perfcnt, clean caches */
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
	p = seprint(p, e, "present shaders 0x%08ux%08ux tiler 0x%08ux%08ux l2 0x%08ux%08ux stack 0x%08ux%08ux\n",
	            g->shaderhi, g->shaderlo, g->tilerhi, g->tilerlo,
	            g->l2hi, g->l2lo, g->stackhi, g->stacklo);
	p = seprint(p, e, "threads %ud workgroup %ud barrier %ud\n",
	            g->threads, g->threadwg, g->threadbar);
	p = seprint(p, e, "power on response %s", g->responded ? "yes" : "no");
	if(g->why[0] != 0)
		p = seprint(p, e, " (%s)", g->why);
	p = seprint(p, e, "\n");
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
	fprint(2, "usage: gpufs [-i]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	DrvSrv *s;
	int info;

	info = 0;
	ARGBEGIN{
	case 'i':
		info = 1;
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
		}
	}

	if(info){
		char buf[1024];

		statustext(&gpu, buf, sizeof buf);
		print("%s", buf);
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
