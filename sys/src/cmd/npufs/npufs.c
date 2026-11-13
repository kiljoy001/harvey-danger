/*
 * npufs - the RK3566 NPU (NVDLA-derived "RKNPU") as a 9P service.
 *
 * The first rung, like gpufs: bring the block out of reset and read it.
 * The NPU hangs off its own rail (RK809 DCDC_REG4, vdd_npu) and its own
 * power domain (PD_NPU), and U-Boot leaves the rail disabled and at
 * 500 mV.  With the rail off the PMU never acknowledges the NIU idle
 * release and any access to 0xfde40000 aborts the bus, which is why this
 * driver asks pmicfs for the rail before touching the registers.
 *
 * Files:
 *
 *	status	model, power, clock and interrupt state
 *	regs	the PC/status register window, decoded
 *	ctl	"power", "off", "reset", "read <off>", "write <off> <val>"
 *
 * Not here yet: the IOMMU grants, the regcmd/task ring, and job
 * submission.  The register map is from drivers/rknpu/include/rknpu_ioctl.h.
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

	/* RK3568 PMU controller: power 0xa0, idle request 0x50, status
	 * 0x98, ack 0x60, idle 0x68.  PD_NPU is power bit 1 and idle
	 * request/ack bit 2. */
	PmuPwr		= 0xa0,
	PmuReq		= 0x50,
	PmuStatus	= 0x98,
	PmuAck		= 0x60,
	PmuIdle		= 0x68,

	PdNpu		= 1<<1,
	PdNpuReq	= 1<<2,

	/* CRU: NPU clocks and resets (NPU clock rate itself is SCMI's). */
	CruClksel7	= 0x11c,
	CruClksel8	= 0x120,
	CruGate3	= 0x30c,
	CruSoftrst2	= 0x408,

	/* rknpu register window (drivers/rknpu/include/rknpu_ioctl.h). */
	RegVersion	= 0x000,
	RegVersionNum	= 0x004,
	RegPcOpEn	= 0x008,
	RegPcDataAddr	= 0x010,
	RegPcDataAmount	= 0x014,
	RegIntMask	= 0x020,
	RegIntClear	= 0x024,
	RegIntStatus	= 0x028,
	RegIntRaw	= 0x02c,
	RegPcTaskControl= 0x030,
	RegPcDmaBase	= 0x034,
	RegPcTaskStatus	= 0x03c,
	RegEnableMask	= 0xf008,

	/* Regcmd trailer: the PC fetches regcfg_amount + 4 words, the last
	 * four being next-address, register-amounts, the 0041 magic and the
	 * broadcast operation-enable. */
	PcTarget	= 0x100 + 1,
	CnaTarget	= 0x200 + 1,
	CoreTarget	= 0x800 + 1,
	DpuTarget	= 0x1000 + 1,
	RdmaTarget	= 0x2000 + 1,
	GlobTarget	= 0x81,
	PcRegAmounts	= 0x014,
	PcOpEnReg	= 0x008,
	CnaDcompRegnum	= 0x1104,
	CnaDcompCtrl	= 0x1100,

	/* CNA block (rocket_ref/registers.xml). */
	CnaOpEn		= 0x1008,
	CnaConvCon1	= 0x100c,
	CnaConvCon2	= 0x1010,
	CnaConvCon3	= 0x1014,
	CnaDataSize0	= 0x1020,
	CnaDataSize1	= 0x1024,
	CnaDataSize2	= 0x1028,
	CnaDataSize3	= 0x102c,
	CnaWeightSize0	= 0x1030,
	CnaWeightSize1	= 0x1034,
	CnaWeightSize2	= 0x1038,
	CnaCbufCon0	= 0x1040,
	CnaCbufCon1	= 0x1044,
	CnaCvtCon0	= 0x104c,
	CnaCvtCon1	= 0x1050,
	CnaCvtCon5	= 0x1180,
	CnaPadCon0	= 0x1068,
	CnaPadCon1	= 0x1184,
	CnaFeatureAddr	= 0x1070,
	CnaDmaCon0	= 0x1078,
	CnaDmaCon1	= 0x107c,
	CnaDmaCon2	= 0x1080,
	CnaFcSize0	= 0x1084,
	CnaFcSize1	= 0x1088,
	CnaDcompAddr0	= 0x1110,
	CnaDcompAmount0	= 0x1140,

	/* CORE, DPU and DPU_RDMA. */
	CoreMiscCfg	= 0x3010,
	CoreDataoutSize0= 0x3014,
	CoreDataoutSize1= 0x3018,
	CoreClipTruncate= 0x301c,
	DpuFeatureModeCfg= 0x400c,
	DpuDataFormat	= 0x4010,
	DpuOffsetPend	= 0x4014,
	DpuDstBaseAddr	= 0x4020,
	DpuDstSurfStride= 0x4024,
	DpuCubeWidth	= 0x4030,
	DpuCubeHeight	= 0x4034,
	DpuCubeNotch	= 0x4038,
	DpuCubeChannel	= 0x403c,
	DpuBsCfg	= 0x4040,
	DpuBsOwCfg	= 0x4050,
	DpuBsOwOp	= 0x4054,
	DpuWdmaSize0	= 0x4058,
	DpuWdmaSize1	= 0x405c,
	DpuBnCfg	= 0x4060,
	DpuEwCfg	= 0x4070,
	DpuOutCvtOffset	= 0x4080,
	DpuOutCvtScale	= 0x4084,
	DpuOutCvtShift	= 0x4088,
	RdmaCubeWidth	= 0x500c,
	RdmaCubeHeight	= 0x5010,
	RdmaCubeChannel	= 0x5014,
	RdmaSrcBaseAddr	= 0x5018,
	RdmaBrdmaCfg	= 0x501c,
	RdmaBsBaseAddr	= 0x5020,
	RdmaErdmaCfg	= 0x5034,
	RdmaEwBaseAddr	= 0x5038,
	RdmaEwSurfStride= 0x5040,
	RdmaFeatureModeCfg=0x5044,
	RdmaSrcDmaCfg	= 0x5048,

	/* NPU IOMMU (rockchip,iommu-v2 at 0xfde4b000). */
	MmuBase		= 0xfde4b000,
	MmuDteaddr	= 0x00,
	MmuStatus	= 0x04,
	MmuCommand	= 0x08,
	MmuFaultaddr	= 0x0c,
	MmuZapone	= 0x10,
	MmuIntraw	= 0x14,
	MmuIntclear	= 0x18,
	MmuIntmask	= 0x1c,
	MmuAutogating	= 0x24,
	MmuPaging	= 1<<0,
	MmuFaultbit	= 1<<1,
	MmuAutogateNofetch = 1u<<31,
	MmuEnable	= 0,
	MmuDisable	= 1,
	MmuZap		= 4,
	MmuReset	= 6,
};

/* struct rknpu_task, 40 bytes (drivers/rknpu/include/rknpu_ioctl.h). */
typedef struct Task Task;
struct Task {
	u32	flags;
	u32	op_idx;
	u32	enable_mask;
	u32	int_mask;
	u32	int_clear;
	u32	int_status;
	u32	regcfg_amount;
	u32	regcfg_offset;
	u64	regcmd_addr;
};

typedef struct Npu Npu;
struct Npu {
	Drv	d;
	Drv	pmu;
	Drv	cru;
	Drv	mmu;
	int	havemmu;
	int	iommue;
	void	*dt;
	void	*pt;
	u64	dtpa;
	u64	ptpa;
	int	mapped;
	int	powered;
	int	responded;
	u32	version;
	u32	versionnum;
	u32	identmask;
	char	rail[96];
	char	why[256];
	QLock	lk;

	void	*task;		/* task array */
	void	*cmd;		/* regcmd stream */
	void	*scratch;	/* safe DMA target for block tests */
	u64	taskpa;
	u64	cmdpa;
	u64	scratchpa;
	void	*dummy;
	u64	dummypa;
	u32	lastraw;
	u32	lastdcomp;
};

static Npu npu;

static void
setwhy(Npu *n, char *fmt, ...)
{
	va_list a;

	if(n->why[0] != 0)
		return;
	va_start(a, fmt);
	vseprint(n->why, n->why + sizeof n->why, fmt, a);
	va_end(a);
}

/*
 * One pmicfs command.  The service is a channel in /srv, so mount it in
 * this process's namespace first (the same thing pmicfs does for i2cfs).
 * The ctl handler buffers until a newline and leaves its result for the
 * next read.
 */
static void
mountpmic(void)
{
	int fd;

	if(rfork(RFNAMEG) < 0)
		return;
	create("/mnt/pmic", OREAD, DMDIR|0700);
	fd = open("/srv/pmic", ORDWR);
	if(fd < 0)
		return;
	if(mount(fd, -1, "/mnt/pmic", MREPL, "") < 0)
		print("npufs: mount /srv/pmic: %r\n");
	close(fd);
}

static int
pmictl(char *cmd, char *res, int nres)
{
	char buf[96];
	int fd, n;

	fd = open("/mnt/pmic/ctl", ORDWR);
	if(fd < 0)
		return -1;
	snprint(buf, sizeof buf, "%s\n", cmd);
	if(write(fd, buf, strlen(buf)) < 0){
		close(fd);
		return -1;
	}
	n = read(fd, res, nres - 1);
	close(fd);
	if(n < 0)
		return -1;
	res[n] = 0;
	while(n > 0 && (res[n-1] == '\n' || res[n-1] == '\r'))
		res[--n] = 0;
	return 0;
}

/* vdd_npu is a separate RK809 buck; 900 mV is what the vendor DT asks for. */
static int
railup(Npu *n)
{
	char r[96];

	if(pmictl("set vdd_npu 900", r, sizeof r) < 0){
		setwhy(n, "no pmic service: %r");
		return -1;
	}
	if(pmictl("on vdd_npu", r, sizeof r) < 0){
		setwhy(n, "cannot enable vdd_npu: %r");
		return -1;
	}
	snprint(n->rail, sizeof n->rail, "vdd_npu 900 mV");
	if(r[0] != 0)
		snprint(n->rail, sizeof n->rail, "%s", r);
	sleep(20);			/* regulator-ramp-delay is 6 ms */
	return 0;
}

static int
pmuon(Npu *n)
{
	int i;

	if(n->powered)
		return 0;
	if(!n->mapped){
		setwhy(n, "registers not mapped");
		return -1;
	}
	if(railup(n) < 0)
		return -1;

	/*
	 * Clocks and resets first.  The NPU NIU holds its idle ack until
	 * its hclk/aclk actually run, so releasing the request before the
	 * gates open leaves the domain stuck in idle.
	 */
	drw(&n->cru, CruClksel7, 0x81cf0000u | (1u<<6) | 1u);
	drw(&n->cru, CruClksel8, 0x00ff0000u | 0x33u);
	drw(&n->cru, CruGate3, 0x1fff0000u);
	drw(&n->cru, CruSoftrst2, (0x1800u<<16) | 0x1800u);
	for(i = 0; i < 1000; i++)
		;
	drw(&n->cru, CruSoftrst2, (0x1800u<<16) | 0u);

	/* Power on PD_NPU: high-word mask, clear power bit 1. */
	drw(&n->pmu, PmuPwr, PdNpu << 16);
	for(i = 0; i < 1000; i++){
		if((drd(&n->pmu, PmuStatus) & PdNpu) == 0)
			break;
		sleep(1);
	}
	if(i == 1000){
		setwhy(n, "PD_NPU power timeout (status %#ux)",
		       drd(&n->pmu, PmuStatus));
		return -1;
	}

	/* Leave idle: clear request bit 2, wait for ack and idle to clear. */
	drw(&n->pmu, PmuReq, PdNpuReq << 16);
	for(i = 0; i < 1000; i++){
		if((drd(&n->pmu, PmuAck) & PdNpuReq) == 0 &&
		   (drd(&n->pmu, PmuIdle) & PdNpuReq) == 0)
			break;
		sleep(1);
	}
	if(i == 1000){
		setwhy(n, "PD_NPU idle timeout (rail %s, ack %#ux idle %#ux)",
		       n->rail, drd(&n->pmu, PmuAck), drd(&n->pmu, PmuIdle));
		return -1;
	}
	n->powered = 1;
	return 0;
}

static void
pmuoff(Npu *n)
{
	int i;

	if(!n->powered)
		return;
	drw(&n->pmu, PmuReq, PdNpuReq | (PdNpuReq << 16));
	for(i = 0; i < 1000; i++){
		if((drd(&n->pmu, PmuAck) & PdNpuReq) != 0)
			break;
		sleep(1);
	}
	drw(&n->pmu, PmuPwr, PdNpu | (PdNpu << 16));
	n->powered = 0;
}

/*
 * The VERSION register reads zero on this part; the reset value of
 * INT_MASK (0x8001ffff) is the reliable sign that the block answers.
 */
static void
ident(Npu *n)
{
	if(!n->powered){
		n->responded = 0;
		return;
	}
	n->version = drd(&n->d, RegVersion);
	n->versionnum = drd(&n->d, RegVersionNum);
	n->identmask = drd(&n->d, RegIntMask);
	n->responded = (n->identmask != 0 && n->identmask != ~0u);
	if(!n->responded)
		setwhy(n, "no register response (INT_MASK %#ux)", n->identmask);
}

/*
 * One PC-mode task.  The stream writes CNA_DCOMP_REGNUM, a plain register
 * with no side effects, so success is visible as a readback, followed by
 * the four-word trailer.  With every configuration register at zero the
 * compute blocks have no work, and any error still shows in INT_RAW.
 */
static void
emit(u64 *w, int *i, u32 target, u32 reg, u32 val)
{
	w[(*i)++] = ((u64)target<<48) | ((u64)val<<16) | (reg & 0xffff);
}

static void
softreset(Npu *n)
{
	drw(&n->cru, CruSoftrst2, (0x1800u<<16) | 0x1800u);
	sleep(1);
	drw(&n->cru, CruSoftrst2, (0x1800u<<16) | 0u);
	sleep(1);
	n->iommue = 0;		/* the block reset may have cleared paging */
}

static u32
targetof(u32 reg)
{
	return (1u << ((((reg)>>12) & 0xf) + 8)) | 1;
}

/*
 * Address operands in a regcmd are relative to PC_DMA_BASE_ADDR (the task
 * array).  A write of x to an address register reads back x + taskpa; this
 * turns a physical address into the operand that produces it.
 */
static u32
reloc(Npu *n, u64 pa)
{
	return (u32)(pa - n->taskpa);
}

static u32
mmupa(u64 pa)
{
	u64 v;

	v = pa & 0xfffff000ULL;
	v |= (pa & 0xf00000000ULL) >> 24;
	v |= (pa & 0xf000000000ULL) >> 32;
	return (u32)v;
}

static void
mmumap(Npu *n, u64 base, u64 size)
{
	u64 a;

	for(a = base; a < base + size; a += 4096)
		((u32 *)n->pt)[(a>>12) & 0x3ff] = mmupa(a) | 7;
}

/*
 * Identity-map the DMA buffers in the NPU's 4 GiB IOVA space and turn
 * paging on.  One page table covers the arena's 4 MiB directory slot;
 * every other IOVA now faults instead of reaching memory.
 */
static int
iommuon(Npu *n)
{
	u32 dtei;
	int i;

	if(n->iommue)
		return 0;
	if(!n->havemmu){
		setwhy(n, "no NPU IOMMU in #P");
		return -1;
	}
	if(n->task == nil || n->cmd == nil || n->scratch == nil){
		setwhy(n, "allocate the DMA buffers first");
		return -1;
	}
	if(n->dt == nil){
		n->dt = drvdma(&n->d, 4096, 4096, &n->dtpa);
		n->pt = drvdma(&n->d, 4096, 4096, &n->ptpa);
		if(n->dt == nil || n->pt == nil){
			setwhy(n, "no DMA for the IOMMU tables");
			return -1;
		}
	}
	dtei = (u32)(n->taskpa >> 22) & 0x3ff;
	if(((n->cmdpa >> 22) & 0x3ff) != dtei ||
	   ((n->scratchpa >> 22) & 0x3ff) != dtei){
		setwhy(n, "DMA buffers span IOMMU directory entries");
		return -1;
	}
	memset(n->dt, 0, 4096);
	memset(n->pt, 0, 4096);
	((u32 *)n->dt)[dtei] = mmupa(n->ptpa) | 1;
	mmumap(n, n->taskpa, 4096);
	mmumap(n, n->cmdpa, 4096);
	mmumap(n, n->scratchpa, 0x4000);
	drvdmacache(DrvClean, n->dtpa, 4096);
	drvdmacache(DrvClean, n->ptpa, 4096);

	drw(&n->mmu, MmuCommand, MmuReset);
	for(i = 0; i < 1000 && drd(&n->mmu, MmuDteaddr) != 0; i++)
		sleep(1);
	drw(&n->mmu, MmuDteaddr, mmupa(n->dtpa) | 1);
	drw(&n->mmu, MmuCommand, MmuZap);
	drw(&n->mmu, MmuIntmask, 3);
	drw(&n->mmu, MmuAutogating,
	    drd(&n->mmu, MmuAutogating) | MmuAutogateNofetch);
	drw(&n->mmu, MmuCommand, MmuEnable);
	for(i = 0; i < 1000 && (drd(&n->mmu, MmuStatus) & MmuPaging) == 0; i++)
		sleep(1);
	if((drd(&n->mmu, MmuStatus) & MmuPaging) == 0){
		setwhy(n, "IOMMU enable timed out (status %#ux)",
		       drd(&n->mmu, MmuStatus));
		return -1;
	}
	n->iommue = 1;
	return 0;
}

static void
iommuoff(Npu *n)
{
	int i;

	if(!n->iommue)
		return;
	drw(&n->mmu, MmuCommand, MmuDisable);
	for(i = 0; i < 1000 && (drd(&n->mmu, MmuStatus) & MmuPaging) != 0; i++)
		sleep(1);
	n->iommue = 0;
}

static int
jobtest(Npu *n)
{
	Task *t;
	u64 *c, nc;
	int i, raw;

	if(pmuon(n) < 0)
		return -1;
	if(n->task == nil){
		n->task = drvdma(&n->d, 4096, 64, &n->taskpa);
		n->cmd = drvdma(&n->d, 4096, 64, &n->cmdpa);
	}
	if(n->scratch == nil)
		n->scratch = drvdma(&n->d, 0x4000, 64, &n->scratchpa);
	if(n->task == nil || n->cmd == nil || n->scratch == nil){
		setwhy(n, "no DMA arena for the task ring");
		return -1;
	}
	if(iommuon(n) < 0)
		return -1;
	memset(n->task, 0, 4096);
	memset(n->cmd, 0, 4096);

	c = n->cmd;
	nc = 0;
	c[nc++] = ((u64)CnaTarget<<48) | ((u64)0xdeadbeef<<16) | CnaDcompRegnum;
	/* trailer: next stream, register amounts, 0041 magic, op-enable */
	c[nc++] = 0;
	c[nc++] = ((u64)PcTarget<<48) | PcRegAmounts;
	c[nc++] = 0x0041000000000000ULL;
	/*
	 * Deliberately not the broadcast op-enable: that starts CNA/DPU
	 * with every address register at zero, and on a live machine the
	 * resulting DMA can scribble over low physical memory.  Writing a
	 * second harmless CNA register keeps the four-word trailer shape.
	 */
	c[nc++] = ((u64)CnaTarget<<48) | ((u64)0<<16) | CnaDcompCtrl;

	t = n->task;
	t->int_mask = 0x1ffff;
	t->int_clear = 0x1ffff;
	t->regcfg_amount = nc - 4;	/* register words, trailer excluded */
	t->regcmd_addr = n->cmdpa;

	drvdmacache(DrvClean, n->cmdpa, 4096);
	drvdmacache(DrvClean, n->taskpa, 4096);

	drw(&n->d, RegIntClear, 0x1ffff);
	drw(&n->d, RegIntMask, 0x1ffff);
	drw(&n->d, RegPcDataAddr, (u32)n->cmdpa);
	drw(&n->d, RegPcDataAmount, t->regcfg_amount + 3);
	drw(&n->d, RegPcTaskControl, (6<<12) | 1);
	drw(&n->d, RegPcDmaBase, (u32)n->taskpa);
	drw(&n->d, RegPcOpEn, 1);
	drw(&n->d, RegPcOpEn, 0);

	raw = 0;
	for(i = 0; i < 200; i++){
		raw = drd(&n->d, RegIntRaw);
		if(raw != 0)
			break;
		sleep(1);
	}
	n->lastraw = raw;
	n->lastdcomp = drd(&n->d, CnaDcompRegnum);
	drw(&n->d, RegIntClear, 0x1ffff);

	print("npufs: test cmd %#llux task %#llux amount %ud raw %#ux status %#ux pc %#ux dcomp %#ux\n",
	      n->cmdpa, n->taskpa, t->regcfg_amount + 3, raw,
	      drd(&n->d, RegIntStatus), drd(&n->d, RegPcTaskStatus),
	      n->lastdcomp);
	if(n->lastdcomp != 0xdeadbeef)
		setwhy(n, "task did not run (dcomp %#ux raw %#ux)",
		       n->lastdcomp, raw);
	else
		n->why[0] = 0;
	return n->lastdcomp == 0xdeadbeef ? 0 : -1;
}

/*
 * One regcmd writing val to off, then read the register back.  This is how
 * the address-field encoding gets characterised without rebuilding.
 */
static int
wtest(Npu *n, u32 off, u32 val)
{
	Task *t;
	u64 *w;
	int i, raw;
	u32 back;

	if(pmuon(n) < 0)
		return -1;
	if(n->task == nil){
		n->task = drvdma(&n->d, 4096, 64, &n->taskpa);
		n->cmd = drvdma(&n->d, 4096, 64, &n->cmdpa);
	}
	if(n->scratch == nil)
		n->scratch = drvdma(&n->d, 0x4000, 64, &n->scratchpa);
	if(n->task == nil || n->cmd == nil || n->scratch == nil){
		setwhy(n, "no DMA arena");
		return -1;
	}
	softreset(n);
	if(iommuon(n) < 0)
		return -1;
	memset(n->task, 0, 4096);
	memset(n->cmd, 0, 4096);

	w = n->cmd;
	i = 0;
	emit(w, &i, targetof(off), off, val);
	w[i++] = 0;
	emit(w, &i, PcTarget, PcRegAmounts, 0);
	w[i++] = 0x0041000000000000ULL;
	emit(w, &i, CnaTarget, CnaDcompCtrl, 0);

	t = n->task;
	t->int_mask = 0;
	t->int_clear = 0x1ffff;
	t->regcfg_amount = i - 4;
	t->regcmd_addr = n->cmdpa;
	drvdmacache(DrvClean, n->cmdpa, 4096);
	drvdmacache(DrvClean, n->taskpa, 4096);

	drw(&n->d, RegIntClear, 0x1ffff);
	drw(&n->d, RegIntMask, 0);
	drw(&n->d, RegPcDataAddr, (u32)n->cmdpa);
	drw(&n->d, RegPcDataAmount, t->regcfg_amount + 3);
	drw(&n->d, RegPcTaskControl, (6<<12) | 1);
	drw(&n->d, RegPcDmaBase, (u32)n->taskpa);
	drw(&n->d, RegPcOpEn, 1);
	drw(&n->d, RegPcOpEn, 0);
	for(i = 0; i < 200 && (raw = drd(&n->d, RegIntRaw)) == 0; i++)
		sleep(1);
	back = drd(&n->d, off);
	print("npufs: wtest %#x <- %#x reads %#x raw %#x task %#x\n",
	      off, val, back, raw, drd(&n->d, RegPcTaskStatus));
	drw(&n->d, RegIntClear, 0x1ffff);
	return 0;
}

static void
emitcna(Npu *n, u64 *w, int *i, int start)
{
	emit(w, i, CnaTarget, CnaCbufCon0, (1<<4)|1);	/* 1 weight, 1 data bank */
	emit(w, i, CnaTarget, CnaCbufCon1, 1);		/* one data entry */
	emit(w, i, CnaTarget, CnaConvCon1, 0);
	emit(w, i, CnaTarget, CnaConvCon2, 52<<4);	/* feature grains */
	emit(w, i, CnaTarget, CnaConvCon3, (1<<3)|1);	/* stride 1x1 */
	emit(w, i, CnaTarget, CnaDataSize0, (1<<16)|1);
	emit(w, i, CnaTarget, CnaDataSize1, 32);	/* real 0, aligned 32 */
	emit(w, i, CnaTarget, CnaDataSize2, 1);
	emit(w, i, CnaTarget, CnaDataSize3, 1);
	emit(w, i, CnaTarget, CnaWeightSize0, 1);
	emit(w, i, CnaTarget, CnaWeightSize1, 1);
	emit(w, i, CnaTarget, CnaWeightSize2, (1<<24)|(1<<16)|1);
	emit(w, i, CnaTarget, CnaCvtCon0, 0xb);	/* bypass */
	emit(w, i, CnaTarget, CnaCvtCon1, 1<<16);	/* scale 1 */
	emit(w, i, CnaTarget, CnaCvtCon5, 0xffff);
	emit(w, i, CnaTarget, CnaPadCon0, 0);
	emit(w, i, CnaTarget, CnaPadCon1, 0x80);
	emit(w, i, CnaTarget, CnaFeatureAddr, reloc(n, n->scratchpa));
	emit(w, i, CnaTarget, CnaDmaCon0, (15<<16)|15);
	emit(w, i, CnaTarget, CnaDmaCon1, 1);
	emit(w, i, CnaTarget, CnaDmaCon2, 0);
	emit(w, i, CnaTarget, CnaFcSize0, (1<<16)|1);
	emit(w, i, CnaTarget, CnaFcSize1, 32);
	emit(w, i, CnaTarget, CnaDcompCtrl, 0);
	emit(w, i, CnaTarget, CnaDcompRegnum, 0);
	emit(w, i, CnaTarget, CnaDcompAddr0, reloc(n, n->scratchpa + 0x200));
	emit(w, i, CnaTarget, CnaDcompAmount0, 0);
	if(start)
		emit(w, i, CnaTarget, CnaOpEn, 1);
}

/*
 * Start only the CNA, with every address it can reach inside one scratch
 * buffer.  A 1x1x1 int8 convolution is the smallest job that makes the
 * block read input and weights; the point is the interrupt, not the maths.
 */
static int
cnatest(Npu *n, int broadcast)
{
	Task *t;
	u64 *w;
	u8 *in, *wt;
	int i, raw, done;

	if(pmuon(n) < 0)
		return -1;
	if(n->task == nil){
		n->task = drvdma(&n->d, 4096, 64, &n->taskpa);
		n->cmd = drvdma(&n->d, 4096, 64, &n->cmdpa);
	}
	if(n->scratch == nil)
		n->scratch = drvdma(&n->d, 0x4000, 64, &n->scratchpa);
	if(n->task == nil || n->cmd == nil || n->scratch == nil){
		setwhy(n, "no DMA arena for the CNA test");
		return -1;
	}
	if(iommuon(n) < 0)
		return -1;
	memset(n->task, 0, 4096);
	memset(n->cmd, 0, 4096);
	memset(n->scratch, 0, 0x4000);
	in = n->scratch;
	wt = (u8 *)n->scratch + 0x200;
	in[0] = 2;
	wt[0] = 3;

	w = n->cmd;
	i = 0;
	emitcna(n, w, &i, !broadcast);
	w[i++] = 0;
	emit(w, &i, PcTarget, PcRegAmounts, 0);
	w[i++] = 0x0041000000000000ULL;
	if(broadcast)
		emit(w, &i, GlobTarget, PcOpEnReg, 0x1d);
	else
		emit(w, &i, CnaTarget, CnaDcompCtrl, 0);

	t = n->task;
	t->int_mask = broadcast ? 0x1ffff : 0xf;
	t->int_clear = 0x1ffff;
	t->regcfg_amount = i - 4;
	t->regcmd_addr = n->cmdpa;

	drvdmacache(DrvClean, n->cmdpa, 4096);
	drvdmacache(DrvClean, n->taskpa, 4096);
	drvdmacache(DrvClean, n->scratchpa, 0x4000);

	drw(&n->d, RegIntClear, 0x1ffff);
	drw(&n->d, RegIntMask, broadcast ? 0x1ffff : 0xf);
	if(n->iommue){
		drw(&n->mmu, MmuIntclear, 3);
		drw(&n->mmu, MmuCommand, 5);	/* fault done */
	}
	drw(&n->d, RegPcDataAddr, (u32)n->cmdpa);
	drw(&n->d, RegPcDataAmount, t->regcfg_amount + 3);
	drw(&n->d, RegPcTaskControl, (6<<12) | 1);
	drw(&n->d, RegPcDmaBase, (u32)n->taskpa);
	drw(&n->d, RegPcOpEn, 1);
	drw(&n->d, RegPcOpEn, 0);

	raw = 0;
	for(i = 0; i < 500; i++){
		raw = drd(&n->d, RegIntRaw);
		if(raw & 0xf)
			break;
		sleep(1);
	}
	done = (raw & 0xf) != 0;
	n->lastraw = raw;
	drw(&n->d, RegIntClear, 0x1ffff);
	if(n->iommue){
		/* A fault stalls the bus; acknowledge it so the next job runs. */
		drw(&n->mmu, MmuIntclear, 3);
		drw(&n->mmu, MmuCommand, 5);
	}
	drvdmacache(DrvInval, n->scratchpa, 0x4000);
	print("npufs: %s raw %#ux status %#ux pc %#ux scratch %#llux out %02ux",
	      broadcast ? "bcna" : "cna", raw, drd(&n->d, RegIntStatus),
	      drd(&n->d, RegPcTaskStatus), n->scratchpa,
	      ((u8 *)n->scratch)[0x1000]);
	if(n->iommue)
		print(" iommu raw %#ux fault %#ux",
		      drd(&n->mmu, MmuIntraw), drd(&n->mmu, MmuFaultaddr));
	print("\n");
	if(!done)
		setwhy(n, "CNA did not signal (raw %#ux)", raw);
	else
		n->why[0] = 0;
	return done ? 0 : -1;
}

/*
 * The whole pipeline for a 1x1x1 int8 convolution.  Every address operand
 * is task-relative and every target lies in the scratch buffer; the IOMMU
 * still guards the rest of the address space.
 */
static int
convtest(Npu *n)
{
	Task *t;
	u64 *w;
	u8 *in, *wt;
	int i, raw, done;

	if(pmuon(n) < 0)
		return -1;
	if(n->task == nil){
		n->task = drvdma(&n->d, 4096, 64, &n->taskpa);
		n->cmd = drvdma(&n->d, 4096, 64, &n->cmdpa);
	}
	if(n->scratch == nil)
		n->scratch = drvdma(&n->d, 0x4000, 64, &n->scratchpa);
	if(n->task == nil || n->cmd == nil || n->scratch == nil){
		setwhy(n, "no DMA arena for the conv test");
		return -1;
	}
	softreset(n);
	if(iommuon(n) < 0)
		return -1;
	memset(n->task, 0, 4096);
	memset(n->cmd, 0, 4096);
	memset(n->scratch, 0, 0x4000);
	in = n->scratch;
	wt = (u8 *)n->scratch + 0x200;
	in[0] = 2;
	wt[0] = 3;

	w = n->cmd;
	i = 0;
	emitcna(n, w, &i, 0);
	/* CORE */
	emit(w, &i, CoreTarget, CoreMiscCfg, 1);
	emit(w, &i, CoreTarget, CoreDataoutSize0, 0);
	emit(w, &i, CoreTarget, CoreDataoutSize1, 0);
	emit(w, &i, CoreTarget, CoreClipTruncate, 0);
	/* DPU */
	emit(w, &i, DpuTarget, DpuFeatureModeCfg, (15<<5)|(2<<1));
	emit(w, &i, DpuTarget, DpuDataFormat, 0);
	emit(w, &i, DpuTarget, DpuOffsetPend, 0);
	emit(w, &i, DpuTarget, DpuDstBaseAddr, reloc(n, n->scratchpa + 0x1000));
	emit(w, &i, DpuTarget, DpuDstSurfStride, 1<<4);
	emit(w, &i, DpuTarget, DpuCubeWidth, 0);
	emit(w, &i, DpuTarget, DpuCubeHeight, 0);
	emit(w, &i, DpuTarget, DpuCubeNotch, 0);
	emit(w, &i, DpuTarget, DpuCubeChannel, 31);
	emit(w, &i, DpuTarget, DpuBsCfg, (2<<16)|(1<<8)|(1<<6)|(1<<4));
	emit(w, &i, DpuTarget, DpuBsOwCfg, (1<<8)|(1<<5)|(1<<2));
	emit(w, &i, DpuTarget, DpuBsOwOp, 0x80);
	emit(w, &i, DpuTarget, DpuWdmaSize0, 31);
	emit(w, &i, DpuTarget, DpuWdmaSize1, 0);
	emit(w, &i, DpuTarget, DpuBnCfg, (1<<6)|(1<<4)|(1<<1)|1);
	emit(w, &i, DpuTarget, DpuEwCfg, (1<<9)|(1<<8)|(1<<7)|(1<<1)|1);
	emit(w, &i, DpuTarget, DpuOutCvtOffset, 0xffffff80);
	emit(w, &i, DpuTarget, DpuOutCvtScale, 0x4000);
	emit(w, &i, DpuTarget, DpuOutCvtShift, 14);
	/* DPU_RDMA */
	emit(w, &i, RdmaTarget, RdmaCubeWidth, 0);
	emit(w, &i, RdmaTarget, RdmaCubeHeight, 0);
	emit(w, &i, RdmaTarget, RdmaCubeChannel, 31);
	emit(w, &i, RdmaTarget, RdmaSrcBaseAddr, reloc(n, n->scratchpa + 0x800));
	emit(w, &i, RdmaTarget, RdmaBrdmaCfg, 1<<1);
	emit(w, &i, RdmaTarget, RdmaBsBaseAddr, reloc(n, n->scratchpa + 0x300));
	emit(w, &i, RdmaTarget, RdmaErdmaCfg, 1);
	emit(w, &i, RdmaTarget, RdmaEwBaseAddr, reloc(n, n->scratchpa + 0xc00));
	emit(w, &i, RdmaTarget, RdmaEwSurfStride, 0);
	emit(w, &i, RdmaTarget, RdmaFeatureModeCfg, (15<<11)|(1<<4));
	emit(w, &i, RdmaTarget, RdmaSrcDmaCfg, 0);
	/* trailer, with the broadcast operation-enable */
	w[i++] = 0;
	emit(w, &i, PcTarget, PcRegAmounts, 0);
	w[i++] = 0x0041000000000000ULL;
	emit(w, &i, GlobTarget, PcOpEnReg, 0x1d);

	t = n->task;
	t->int_mask = 0x1ffff;
	t->int_clear = 0x1ffff;
	t->regcfg_amount = i - 4;
	t->regcmd_addr = n->cmdpa;

	drvdmacache(DrvClean, n->cmdpa, 4096);
	drvdmacache(DrvClean, n->taskpa, 4096);
	drvdmacache(DrvClean, n->scratchpa, 0x4000);

	drw(&n->d, RegIntClear, 0x1ffff);
	drw(&n->d, RegIntMask, 0x1ffff);
	drw(&n->mmu, MmuIntclear, 3);
	drw(&n->mmu, MmuCommand, 5);
	drw(&n->d, RegPcDataAddr, (u32)n->cmdpa);
	drw(&n->d, RegPcDataAmount, t->regcfg_amount + 3);
	drw(&n->d, RegPcTaskControl, (6<<12) | 1);
	drw(&n->d, RegPcDmaBase, (u32)n->taskpa);
	drw(&n->d, RegPcOpEn, 1);
	drw(&n->d, RegPcOpEn, 0);

	raw = 0;
	for(i = 0; i < 1000; i++){
		raw = drd(&n->d, RegIntRaw);
		if(raw != 0)
			break;
		sleep(1);
	}
	done = raw != 0;
	n->lastraw = raw;
	drw(&n->d, RegIntClear, 0x1ffff);
	if(n->iommue){
		drw(&n->mmu, MmuIntclear, 3);
		drw(&n->mmu, MmuCommand, 5);
	}
	drvdmacache(DrvInval, n->scratchpa, 0x4000);
	print("npufs: conv raw %#ux status %#ux pc %#ux out %02ux %02ux %02ux %02ux",
	      raw, drd(&n->d, RegIntStatus), drd(&n->d, RegPcTaskStatus),
	      ((u8 *)n->scratch)[0x1000], ((u8 *)n->scratch)[0x1001],
	      ((u8 *)n->scratch)[0x1002], ((u8 *)n->scratch)[0x1003]);
	if(n->iommue)
		print(" iommu raw %#ux fault %#ux",
		      drd(&n->mmu, MmuIntraw), drd(&n->mmu, MmuFaultaddr));
	print("\n");
	if(!done)
		setwhy(n, "blocks did not signal (raw %#ux)", raw);
	else
		n->why[0] = 0;
	return done ? 0 : -1;
}

static char *
statustext(Npu *n, char *buf, int nbuf)
{
	char *p, *e;

	p = buf;
	e = buf + nbuf;
	if(!n->mapped)
		return seprint(p, e, "no npu node: %s\n", n->why);
	p = seprint(p, e, "model rk3566-rknpu (NVDLA-derived)\n");
	p = seprint(p, e, "version %#ux num %#ux responded %s\n",
	            n->version, n->versionnum, n->responded ? "yes" : "no");
	if(n->powered)
		p = seprint(p, e, "power on rail %s pd_npu on clocks 594/4 MHz\n",
		            n->rail);
	else
		p = seprint(p, e, "power off\n");
	if(n->powered){
		if(n->havemmu){
		p = seprint(p, e, "iommu %s", n->iommue ? "paging" : "off");
		if(n->iommue)
			p = seprint(p, e, " raw %#ux fault %#ux",
			            drd(&n->mmu, MmuIntraw),
			            drd(&n->mmu, MmuFaultaddr));
		p = seprint(p, e, "\n");
	}
	p = seprint(p, e, "int raw %#ux status %#ux mask %#ux clear %#ux\n",
		            drd(&n->d, RegIntRaw), drd(&n->d, RegIntStatus),
		            drd(&n->d, RegIntMask), drd(&n->d, RegIntClear));
		p = seprint(p, e, "pc op_en %#ux data_addr %#ux amount %#ux task %#ux dma %#x\n",
		            drd(&n->d, RegPcOpEn), drd(&n->d, RegPcDataAddr),
		            drd(&n->d, RegPcDataAmount), drd(&n->d, RegPcTaskControl),
		            drd(&n->d, RegPcDmaBase));
	}else
		p = seprint(p, e, "registers unreadable while the domain is off\n");
	if(n->lastraw != 0 || n->lastdcomp != 0)
		p = seprint(p, e, "last job raw %#ux dcomp %#ux\n",
		            n->lastraw, n->lastdcomp);
	if(n->why[0] != 0)
		p = seprint(p, e, "last error: %s\n", n->why);
	return buf;
}

static char *
regstext(Npu *n, char *buf, int nbuf)
{
	static struct { char *name; u32 off; } reg[] = {
		{ "VERSION",		RegVersion },
		{ "VERSION_NUM",	RegVersionNum },
		{ "PC_OP_EN",		RegPcOpEn },
		{ "PC_DATA_ADDR",	RegPcDataAddr },
		{ "PC_DATA_AMOUNT",	RegPcDataAmount },
		{ "INT_MASK",		RegIntMask },
		{ "INT_CLEAR",		RegIntClear },
		{ "INT_STATUS",		RegIntStatus },
		{ "INT_RAW_STATUS",	RegIntRaw },
		{ "PC_TASK_CONTROL",	RegPcTaskControl },
		{ "PC_DMA_BASE_ADDR",	RegPcDmaBase },
		{ "ENABLE_MASK",	RegEnableMask },
	};
	char *p, *e;
	int i;

	p = buf;
	e = buf + nbuf;
	if(!n->powered)
		return seprint(p, e, "power off; registers unreadable\n");
	for(i = 0; i < nelem(reg); i++)
		p = seprint(p, e, "%-17s %#06ux %#08ux\n",
		            reg[i].name, reg[i].off, drd(&n->d, reg[i].off));
	return buf;
}

static void
fsread(Req *r, void *aux)
{
	Npu *n;
	char buf[2048];
	char *name;

	n = aux;
	name = drvfile(r)->name;
	if(strcmp(name, "status") == 0){
		qlock(&n->lk);
		statustext(n, buf, sizeof buf);
		qunlock(&n->lk);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	if(strcmp(name, "regs") == 0){
		qlock(&n->lk);
		regstext(n, buf, sizeof buf);
		qunlock(&n->lk);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	respond(r, "read not permitted");
}

static void
ctlcmd(Npu *n, char *line)
{
	char res[128];
	char *f[4];
	int nf, off;

	nf = tokenize(line, f, nelem(f));
	if(nf == 0)
		return;
	if(strcmp(f[0], "power") == 0){
		if(pmuon(n) < 0)
			print("npufs: power: %s\n", n->why);
		else{
			ident(n);
			print("npufs: power on, int mask %#ux\n", n->identmask);
		}
	}else if(strcmp(f[0], "off") == 0){
		pmuoff(n);
		print("npufs: power off\n");
	}else if(strcmp(f[0], "reset") == 0){
		if(!n->powered){
			print("npufs: reset ignored, power is off\n");
			return;
		}
		drw(&n->cru, CruSoftrst2, (0x1800u<<16) | 0x1800u);
		sleep(1);
		drw(&n->cru, CruSoftrst2, (0x1800u<<16) | 0u);
		sleep(1);
		ident(n);
		print("npufs: reset, int mask %#ux\n", n->identmask);
	}else if(strcmp(f[0], "ident") == 0){
		ident(n);
	}else if(strcmp(f[0], "test") == 0){
		jobtest(n);
	}else if(strcmp(f[0], "cna") == 0){
		cnatest(n, 0);
	}else if(strcmp(f[0], "bcna") == 0){
		cnatest(n, 1);
	}else if(strcmp(f[0], "dummy") == 0 && nf >= 2){
		n->dummy = drvdma(&n->d, strtoul(f[1], nil, 0), 64, &n->dummypa);
		print("npufs: dummy %#llux\n", n->dummypa);
	}else if(strcmp(f[0], "conv") == 0){
		convtest(n);
	}else if(strcmp(f[0], "wtest") == 0 && nf >= 3){
		wtest(n, strtoul(f[1], nil, 0), strtoul(f[2], nil, 0));
	}else if(strcmp(f[0], "iommu") == 0){
		if(iommuon(n) < 0)
			print("npufs: iommu: %s\n", n->why);
		else{
			drw(&n->mmu, MmuIntclear, 3);
			print("npufs: iommu paging, status %#ux\n",
			      drd(&n->mmu, MmuStatus));
		}
	}else if(strcmp(f[0], "iommu-off") == 0){
		iommuoff(n);
		print("npufs: iommu off\n");
	}else if(strcmp(f[0], "read") == 0 && nf >= 2){
		off = strtoul(f[1], nil, 0);
		snprint(res, sizeof res, "%#x: %#ux", off, drd(&n->d, off));
		print("npufs: %s\n", res);
	}else if(strcmp(f[0], "write") == 0 && nf >= 3){
		off = strtoul(f[1], nil, 0);
		drw(&n->d, off, strtoul(f[2], nil, 0));
		snprint(res, sizeof res, "%#x: %#ux", off, drd(&n->d, off));
		print("npufs: %s\n", res);
	}else
		print("npufs: unknown command %s\n", f[0]);
}

static void
fswrite(Req *r, void *aux)
{
	static char ctl[128];
	static int ctllen;
	Npu *n;
	char *p, *e;

	n = aux;
	p = r->ifcall.data;
	e = p + r->ifcall.count;
	while(p < e){
		if(*p == '\n'){
			ctl[ctllen] = 0;
			if(ctllen > 0){
				qlock(&n->lk);
				ctlcmd(n, ctl);
				qunlock(&n->lk);
			}
			ctllen = 0;
			p++;
			continue;
		}
		if(ctllen < (int)sizeof ctl - 1)
			ctl[ctllen++] = *p;
		p++;
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static DrvFile files[] = {
	{ "status",	0444,	&npu,	fsread,	nil },
	{ "regs",	0444,	&npu,	fsread,	nil },
	{ "ctl",	0666,	&npu,	nil,	fswrite },
};

static void
usage(void)
{
	fprint(2, "usage: npufs [-i]\n");
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

	if(drvfind(&npu.d, "npu", 0) < 0){
		if(info){
			print("npufs: no npu in #P\n");
			exits("nonpu");
		}
		drvfail("npufs: no NPU in #P");
	}
	mountpmic();
	if(drvfindreg(&npu.mmu, MmuBase) == 0 && drvmap(&npu.mmu) == 0)
		npu.havemmu = 1;
	if(drvmap(&npu.d) < 0){
		setwhy(&npu, "cannot map %s: %r", npu.d.name);
	}else{
		npu.mapped = 1;
		if(drvfindreg(&npu.pmu, PmuBase) == 0 && drvmap(&npu.pmu) == 0){
			if(drvfindreg(&npu.cru, CruBase) == 0 && drvmap(&npu.cru) == 0){
				if(pmuon(&npu) == 0)
					ident(&npu);
			}else
				setwhy(&npu, "no CRU at %#llux in #P", (u64)CruBase);
		}else
			setwhy(&npu, "no PMU at %#llux in #P", (u64)PmuBase);
	}

	if(info){
		char buf[1024];

		statustext(&npu, buf, sizeof buf);
		print("%s", buf);
		exits(npu.responded ? nil : "noprobe");
	}

	if(npu.responded)
		print("HARVEY-ARM64 NPU PROBE version %#ux mask %#ux\n",
		      npu.version, npu.identmask);
	else
		print("HARVEY-ARM64 NPU PROBE FAIL %s\n",
		      npu.why[0] ? npu.why : "no response");

	s = drvsrv("npu", files, nelem(files));
	if(s == nil)
		drvfail("npufs: cannot build 9P tree");
	drvsrvpost(s);
	drvhealthrun("npufs");
}
