/* #P, the platform bus for ARM64 user-space drivers: discovery, claims, registers, interrupts, the DMA arena (doc/drivers/platform-bus.md). */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "fdt.h"
#include "mmu.h"

enum {
	Qtopdir,
	Qdevdir,
	Qreg,
	Qsize,
	Qirq,
	Qirqflags,
	Qcompat,
	Qclass,
	Qconsole,
	Qwait,
	Qfamily,
	Qclaim,
	Qowner,
	Qirq2,
	Qwait2,
	Qregs,
	Qdma,
	Qdmaclone,
	Qdmaavail,
	Qchunkdir,	/* #P/dma/N; DEV is N */
	Qchunkctl,
	Qchunkdata,
	Qtee,
	Qteesmc,
	Qteeversion,
	Qteeshbase,
	Qteeshsize,
	Qplatform,
	Qteeclean,
	Qteeinval,
	Qteecivac,
	Qresmem,
	Qresmemdir,
	Qresmemctl,
	Qirqdir,
	Qirqline,
	Qirqedge,
};

/* 6 type bits: the DMA control files are past the old 4-bit field. */
#define TYPE(x) ((int)((x)->qid.path & 0x3f))
#define DEV(x)  ((int)(((x)->qid.path >> 6) & 0x3ff))
#define QPATH(d, t) (((u64)(d) << 6) | (t))

static Fdtdev pnpdev[Maxfdtdev];
static u32 npnpdev;
static Fdtinfo *pnpinfo;

/*
 * Named FDT /reserved-memory regions (e.g. wlan_msa_region, modem_region,
 * pil_adsp_region, smem_region).  Exposed under #P/resmem/<name>/ctl so a
 * guest driver can map a specific no-map region as normal non-cacheable RAM
 * (SG_NCRAM) with exact page granularity without consuming physseg[] slots.
 */
typedef struct Pnpresmem Pnpresmem;
struct Pnpresmem {
	char	name[Maxfdtname];
	u64	pa;
	u64	len;
	Physseg	ps;
};
static Pnpresmem pnpresmem[Maxfdtreserve];
static u32 npnpresmem;

/*
 * One owner per device, held through a file.  A driver opens
 * #P/<node>/claim, writes its name, and holds the device for as long as it
 * keeps that file open: closing it (or exiting) releases the device.  Only
 * a holder may map the device's window, wait for its interrupts or use its
 * regs file, so two families (or a stale process) cannot both program the
 * same controller, and a name alone proves nothing.  A claim written as
 * "<name> shared" coexists with other shared claims, under any name: blocks
 * several drivers touch on purpose (a SoC's GRF, a PMIC arbiter two guests
 * drive) say so.  The owner shown is the first claimer.
 */
typedef struct Pnpclaim Pnpclaim;
struct Pnpclaim {
	char	owner[32];	/* empty when free */
	int	shared;
	int	holders;	/* claim files holding it */
};
static Lock pnplock;
static Pnpclaim pnpclaim[Maxfdtdev];

/* Whether the calling process holds dev's claim: an open claim file of
 * its own, or one it inherited. */
static int
pnpholds(int dev)
{
	Proc *up = externup();
	Fgrp *f;
	Chan *c;
	int i, held;

	f = up->fgrp;
	if(f == nil)
		return 0;
	held = 0;
	lock(&f->r.l);
	for(i = 0; i <= f->maxfd; i++){
		c = f->fd[i];
		if(c != nil && c->dev != nil && c->dev->dc == 'P' &&
		   (c->flag & COPEN) && TYPE(c) == Qclaim && DEV(c) == dev &&
		   c->aux != nil){
			held = 1;
			break;
		}
	}
	unlock(&f->r.l);
	return held;
}

/*
 * A register access for #P/<node>/regs.  It runs on the kernel's own page
 * table, briefly, with interrupts off: in a process's table the identity
 * address of a device below the user boundary can be the process's own
 * memory.  An abort on the access (a powered-down or unclocked block)
 * is caught by user.c through pnpprobing, which skips the instruction,
 * and reported as an error rather than retried.  An asynchronous error
 * (a posted write the interconnect refuses later) is not caught.
 */
volatile int pnpprobing[MACHMAX];

static char*
pnpregaccess(u64 pa, int n, u64 *v, int write)
{
	Proc *up = externup();
	char *err;
	u64 tpa;
	Mpl s;
	int m;

	err = nil;
	s = splhi();
	m = machp()->machno;
	arm64mmuswitch(arm64mmukernelroot());
	if(arm64mmutranslate(pa, &tpa) < 0 || tpa != pa)
		err = "regs: block not in the kernel's map";
	else{
		pnpprobing[m] = 1;
		if(write){
			if(n == 4)
				*(volatile u32*)(usize)pa = *v;
			else
				*(volatile u64*)(usize)pa = *v;
		}else{
			if(n == 4)
				*v = *(volatile u32*)(usize)pa;
			else
				*v = *(volatile u64*)(usize)pa;
		}
		__asm__ volatile("dsb sy" ::: "memory");
		if(pnpprobing[m] == 2)
			err = "regs: bus error";
		pnpprobing[m] = 0;
	}
	if(up != nil && up->MMU.rootpa != 0)
		arm64mmuswitch(up->MMU.rootpa);
	splx(s);
	return err;
}

/* #P/<node>/regs: one 4- or 8-byte aligned register per read or write,
 * at its offset in the device's first window. */
static i32
pnpregs(Chan *c, void *a, i32 n, i64 off, int write)
{
	Fdtdev *d;
	char *err;
	u64 v;
	u32 v32;

	d = &pnpdev[DEV(c)];
	if((n != 4 && n != 8) || off < 0 || (off & (n - 1)) != 0 ||
	   (u64)off >= d->size || (u64)n > d->size - off)
		error("regs: one aligned 4- or 8-byte register");
	v = 0;
	if(write){
		if(n == 4){
			memmove(&v32, a, 4);
			v = v32;
		}else
			memmove(&v, a, 8);
	}
	err = pnpregaccess(d->base + off, n, &v, write);
	if(err != nil)
		error(err);
	if(!write){
		if(n == 4){
			v32 = v;
			memmove(a, &v32, 4);
		}else
			memmove(a, &v, 8);
	}
	return n;
}

/* Physseg.attach for a device's window (boot.c): only for a process
 * holding the device's claim. */
int
pnpsegattach(Physseg *ps)
{
	u32 i;

	for(i = 0; i < npnpdev; i++)
		if(strcmp(pnpdev[i].name, ps->name) == 0)
			return pnpholds(i) ? 0 : -1;
	return -1;
}
static u64 consolebase;
extern u64 arm64dmabase, arm64dmasize;
extern u64 arm64teeshmbase, arm64teeshmsize;

/*
 * The DMA arena.  #P owns it: a driver opens #P/dma/clone and holds a
 * chunk, #P/dma/N, for as long as it keeps that file open or the chunk
 * mapped.  Its ctl file allocates the memory ("alloc <size> [<align>]
 * [cached|uncached]"), maps it into the caller ("map"), and cleans or
 * invalidates ranges of it; its data file reads and writes it through
 * the kernel.  No process can map the arena as a whole, or a chunk it
 * was not given: a chunk's files open by name only for a process that
 * already holds its ctl (the clone, or a copy inherited from it), and
 * nothing is freed by address.
 *
 * The memory is zeroed when allocated, so a chunk never shows what its
 * previous owner left.  It goes back to the arena once the last file
 * on it is closed and the last mapping of it gone.
 */
enum {
	Maxdmahole = 256,
	Maxdmachunk = 256,
	Dmactlmax = 128,
};
typedef struct Dmahole Dmahole;
struct Dmahole {
	u64 base;
	u64 size;
};
static Dmahole dmahole[Maxdmahole];
static int ndmahole;
static u64 dmabump;
static Lock dmalock;

typedef struct Dmachunk Dmachunk;
struct Dmachunk {
	int	inuse;		/* the slot is taken */
	int	nchan;		/* open files on it */
	int	nmap;		/* segments mapping it */
	int	allocating;	/* an alloc is under way */
	u64	pa;		/* 0 until allocated */
	u64	len;
	int	cached;
	usize	va;		/* where the latest map put it */
	Physseg	ps;		/* the segment its maps use */
};
static Dmachunk dmachunk[Maxdmachunk];

/* A ctl write may arrive in pieces; a command ends at a newline. */
typedef struct Dmactl Dmactl;
struct Dmactl {
	char buf[Dmactlmax];
	int n;
};

/*
 * OP-TEE SMC gate.  The kernel only executes a whitelisted secure-monitor
 * call at EL1; the OP-TEE client, shared-memory registration, sessions and
 * supplicant RPC all live in userspace.  The message-argument physical
 * address must lie in the #P/dma arena.
 */
enum {
	Teesmcreq	= 8,	/* a0..a7 */
	Teesmcres	= 8,	/* a0..a7 (OP-TEE reads first 4; QCOM SCM also needs a6) */
};
typedef struct Teesmc Teesmc;
struct Teesmc {
	u64 a[Teesmcreq];
};
static Lock teelock;

static void
teesmccall(u64 *in, u64 *out)
{
	register u64 x0 asm("x0") = in[0];
	register u64 x1 asm("x1") = in[1];
	register u64 x2 asm("x2") = in[2];
	register u64 x3 asm("x3") = in[3];
	register u64 x4 asm("x4") = in[4];
	register u64 x5 asm("x5") = in[5];
	register u64 x6 asm("x6") = in[6];
	register u64 x7 asm("x7") = in[7];

	__asm__ volatile("smc #0"
		: "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
		  "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
		:
		: "memory");

	out[0] = x0;
	out[1] = x1;
	out[2] = x2;
	out[3] = x3;
	out[4] = x4;
	out[5] = x5;
	out[6] = x6;
	out[7] = x7;
}

static int inrange(u64, u64, u64, u64);

/*
 * Secure-monitor function IDs we are willing to forward:
 *   - OP-TEE standard / fast calls (0x32000001/3/4, 0xbf00ff00..0xbf00ff1f)
 *   - Qualcomm SiP Service calls (ARM_SMCCC_OWNER_SIP == 2, i.e. bits 29..24 == 0x02)
 *     plus QCOM_SCM_INTERRUPTED (1) resume calls.
 * PSCI (owner 4) and every other secure-monitor service stay in the kernel.
 */
static int
teesmcok(u64 fn, u64 hi, u64 lo)
{
	u64 pa;

	/* Qualcomm SiP SCM calls (0x02xxxxxx, 0x42xxxxxx, 0x82xxxxxx, 0xc2xxxxxx)
	 * and QCOM_SCM_INTERRUPTED (1) continuation. */
	if(((fn >> 24) & 0x3f) == 0x02 || fn == 1)
		return 1;

	switch(fn){
	case 0x32000001:	/* GET_SHM_CONFIG */
		return 1;
	case 0x32000004:	/* CALL_WITH_ARG */
		pa = (hi << 32) | (lo & 0xffffffff);
		if(inrange(pa, 8, arm64dmabase, arm64dmasize) ||
		   inrange(pa, 8, arm64teeshmbase, arm64teeshmsize))
			return 1;
		return 0;
	case 0x32000003:	/* RETURN_FROM_RPC */
		return 1;
	}
	if(fn >= 0xbf00ff00 && fn <= 0xbf00ff1f)
		return 1;
	return 0;
}

/* The end of the memory #P hands out: the arena less the kernel's
 * own top block (mmu.c). */
static u64
dmaend(void)
{
	extern u64 arm64dmakern;

	return arm64dmakern != 0 ? arm64dmakern : arm64dmabase + arm64dmasize;
}

/* Inserts a hole, keeping the list sorted and coalesced.  Called with
 * dmalock held.  Returns -1 if the list is full: the range is then
 * leaked, and said so. */
static int
dmaputhole(u64 base, u64 size)
{
	int i, j;

	if(size == 0)
		return 0;
	if(base + size == dmabump){
		/* the arena's tail: lower the bump instead */
		dmabump = base;
		while(ndmahole > 0 &&
		      dmahole[ndmahole-1].base + dmahole[ndmahole-1].size == dmabump)
			dmabump = dmahole[--ndmahole].base;
		return 0;
	}
	for(i = 0; i < ndmahole && dmahole[i].base < base; i++)
		;
	if(i > 0 && dmahole[i-1].base + dmahole[i-1].size == base){
		dmahole[i-1].size += size;
		if(i < ndmahole && base + size == dmahole[i].base){
			dmahole[i-1].size += dmahole[i].size;
			for(j = i; j + 1 < ndmahole; j++)
				dmahole[j] = dmahole[j+1];
			ndmahole--;
		}
		return 0;
	}
	if(i < ndmahole && base + size == dmahole[i].base){
		dmahole[i].base = base;
		dmahole[i].size += size;
		return 0;
	}
	if(ndmahole >= Maxdmahole){
		print("#P/dma: hole list full: %#llux+%#llux leaked\n", base, size);
		return -1;
	}
	for(j = ndmahole; j > i; j--)
		dmahole[j] = dmahole[j-1];
	dmahole[i].base = base;
	dmahole[i].size = size;
	ndmahole++;
	return 0;
}

/* Carves size bytes aligned to align: from a hole when one fits (its
 * slack before the aligned start stays a hole), else from the tail. */
static u64
dmaalloc(u64 size, u64 align)
{
	u64 a, end, hbase;
	int i;

	if(size == 0 || arm64dmasize == 0 || size > dmaend() - arm64dmabase)
		error(Ebadarg);
	if(align == 0)
		align = 4096;
	if(align > BIGPGSZ || (align & (align - 1)) != 0)
		error(Ebadarg);
	size = PGROUND(size);
	lock(&dmalock);
	if(dmabump == 0)
		dmabump = arm64dmabase;
	for(i = 0; i < ndmahole; i++){
		hbase = dmahole[i].base;
		a = (hbase + align - 1) & ~(align - 1);
		end = hbase + dmahole[i].size;
		if(a < hbase || a + size > end)
			continue;
		if(a > hbase && a + size < end && ndmahole >= Maxdmahole)
			continue;	/* splitting needs a slot we lack */
		/* take [a, a+size); keep what is left on either side */
		if(a + size < end){
			dmahole[i].base = a + size;
			dmahole[i].size = end - (a + size);
			if(a > hbase)
				dmaputhole(hbase, a - hbase);
		}else if(a > hbase)
			dmahole[i].size = a - hbase;
		else{
			for(; i + 1 < ndmahole; i++)
				dmahole[i] = dmahole[i+1];
			ndmahole--;
		}
		unlock(&dmalock);
		return a;
	}
	a = (dmabump + align - 1) & ~(align - 1);
	if(a < dmabump || a + size > dmaend()){
		unlock(&dmalock);
		error(Enomem);
	}
	if(a > dmabump && dmaputhole(dmabump, a - dmabump) < 0){
		unlock(&dmalock);
		error(Enomem);
	}
	dmabump = a + size;
	unlock(&dmalock);
	return a;
}

/* Frees a chunk whose last file and mapping are gone.  Called with
 * dmalock held. */
static void
dmachunkput(Dmachunk *ck)
{
	if(!ck->inuse || ck->nchan > 0 || ck->nmap > 0)
		return;
	if(ck->len != 0)
		dmaputhole(ck->pa, ck->len);
	memset(ck, 0, sizeof *ck);
}

/* Physseg.release: a segment mapping the chunk is gone. */
static void
dmachunkrelease(Physseg *ps)
{
	Dmachunk *ck;

	ck = (Dmachunk*)((uchar*)ps - offsetof(Dmachunk, ps));
	lock(&dmalock);
	if(ck->nmap > 0)
		ck->nmap--;
	dmachunkput(ck);
	unlock(&dmalock);
}

/*
 * Whether the calling process holds a chunk's ctl open.  The ctl is
 * the chunk's capability: opening its files by name requires it.
 */
static int
dmaholds(int n)
{
	Proc *up = externup();
	Fgrp *f;
	Chan *c;
	int i, held;

	f = up->fgrp;
	if(f == nil)
		return 0;
	held = 0;
	lock(&f->r.l);
	for(i = 0; i <= f->maxfd; i++){
		c = f->fd[i];
		if(c != nil && c->dev != nil && c->dev->dc == 'P' &&
		   (c->flag & COPEN) && TYPE(c) == Qchunkctl && DEV(c) == n){
			held = 1;
			break;
		}
	}
	unlock(&f->r.l);
	return held;
}

static void dmacache(int, u64, u64);

enum { Dclean, Dinval, Dcleaninval };

static int
inrange(u64 pa, u64 len, u64 base, u64 size)
{
	return base != 0 && pa >= base && pa - base <= size &&
	       len <= size - (pa - base);
}

static void
dmacache(int op, u64 pa, u64 len)
{
	u64 a, end;

	if(len == 0 ||
	   (!inrange(pa, len, arm64dmabase, arm64dmasize) &&
	    !inrange(pa, len, arm64teeshmbase, arm64teeshmsize)))
		error(Ebadarg);
	a = (u64)KADDR(pa) & ~63ULL;
	end = (u64)KADDR(pa) + len;
	for(; a < end; a += 64){
		if(op == Dclean)
			__asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
		else if(op == Dinval)
			__asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
		else
			__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static int
number(char **pp, u64 *v)
{
	char *p = *pp;
	u64 x = 0;
	int n = 0, d;

	while(*p == ' ' || *p == '\t')
		p++;
	if(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')){
		p += 2;
		for(;; p++, n++){
			if(*p >= '0' && *p <= '9') d = *p - '0';
			else if(*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
			else if(*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
			else break;
			x = x*16 + d;
		}
	}else{
		for(; *p >= '0' && *p <= '9'; p++, n++)
			x = x*10 + (*p - '0');
	}
	*pp = p;
	*v = x;
	return n ? 0 : -1;
}

void
arm64pnpinit(Fdtinfo *info)
{
	u32 i;

	consolebase = info->uartbase;
	pnpinfo = info;
	npnpdev = 0;
	for(i = 0; i < info->ndev && npnpdev < Maxfdtdev; i++){
		/*
		 * IOMMU windows are granted like any other device.  The
		 * kernel's own rkiommu driver works from the device tree,
		 * so a user driver that owns a bus (the NPU, for one) can
		 * programme the tables its device walks.
		 */
		pnpdev[npnpdev++] = info->dev[i];
	}
	npnpresmem = 0;
	for(i = 0; i < info->nreserve && npnpresmem < Maxfdtreserve; i++){
		Pnpresmem *rm;

		if(info->reserve[i].name[0] == 0 || info->reserve[i].size == 0)
			continue;
		if((info->reserve[i].base | info->reserve[i].size) & (PGSZ - 1))
			continue;
		rm = &pnpresmem[npnpresmem++];
		memset(rm, 0, sizeof *rm);
		strecpy(rm->name, rm->name + sizeof rm->name, info->reserve[i].name);
		rm->pa = info->reserve[i].base;
		rm->len = info->reserve[i].size;
		rm->ps.attr = SG_PHYSICAL | SG_READ | SG_WRITE | SG_NCRAM;
		rm->ps.name = rm->name;
		rm->ps.pa = rm->pa;
		rm->ps.size = (rm->len + BIGPGSZ - 1) / BIGPGSZ;
		rm->ps.pgszi = -1;
	}
}

static char*
pnpfamily(int clas)
{
	switch(clas){
	case Fdtclassuart:	return "serial";
	case Fdtclassi2c:
	case Fdtclassspi:
	case Fdtclassgpio:
	case Fdtclasspwm:
	case Fdtclasssaradc:
	case Fdtclasstsadc:
	case Fdtclasswdt:
	case Fdtclassrng:
	case Fdtclassi2s:
	case Fdtclasscru:
	case Fdtclassgrf:
	case Fdtclasspower:
	case Fdtclassrtc:	return "soc";
	case Fdtclassgmac:	return "net";
	case Fdtclassmmc:	return "mmc";
	case Fdtclassusb:	return "usb";
	case Fdtclasspcie:	return "pci";
	case Fdtclassnpu:
	case Fdtclassgpu:
	case Fdtclassvpu:	return "accel";
	case Fdtclassvop:
	case Fdtclasshdmi:
	case Fdtclassdsi:
	case Fdtclassedp:	return "display";
	case Fdtclasssfc:	return "storage";
	case Fdtclassiommu:	return "iommu";
	}
	return "misc";
}

static int
pnpgen(Chan *c, char *name, Dirtab *tab, int ntab, int s, Dir *dp)
{
	Proc *up = externup();
	Qid q;
	Fdtdev *d;
	int dev, n;

	USED(name);
	USED(tab);
	USED(ntab);
	q.vers = 0;
	switch(TYPE(c)){	case Qtopdir:
		if(s == DEVDOTDOT){
			q.path = QPATH(0, Qtopdir);
			q.type = QTDIR;
			devdir(c, q, "#P", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		n = (int)npnpdev;
		if(arm64dmasize != 0){
			if(s == n){
				q.path = QPATH(npnpdev, Qdma);
				q.type = QTDIR;
				devdir(c, q, "dma", 0, eve, DMDIR|0555, dp);
				return 1;
			}
			n++;
		}
		if(s == n){
			q.path = QPATH(npnpdev, Qtee);
			q.type = QTDIR;
			devdir(c, q, "tee", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		n++;
		if(npnpresmem != 0){
			if(s == n){
				q.path = QPATH(npnpdev, Qresmem);
				q.type = QTDIR;
				devdir(c, q, "resmem", 0, eve, DMDIR|0555, dp);
				return 1;
			}
			n++;
		}
		if(s == n){
			q.path = QPATH(npnpdev, Qirqdir);
			q.type = QTDIR;
			devdir(c, q, "irq", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		n++;
		if(s == n){
			q.path = QPATH(npnpdev, Qplatform);
			q.type = QTFILE;
			devdir(c, q, "platform", 0, eve, 0444, dp);
			return 1;
		}
		n++;
		if(s < 0 || s >= n)
			return -1;
		d = &pnpdev[s];
		q.path = QPATH(s, Qdevdir);
		q.type = QTDIR;
		devdir(c, q, d->name, 0, eve, DMDIR|0555, dp);
		return 1;
	case Qdevdir:
		if(s == DEVDOTDOT){
			q.path = QPATH(0, Qtopdir);
			q.type = QTDIR;
			devdir(c, q, "#P", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		dev = DEV(c);
		if(dev < 0 || dev >= (int)npnpdev || s < 0 || s > Qregs-Qreg)
			return -1;
		q.type = QTFILE;
		switch(s + Qreg){
		case Qreg:
			q.path = QPATH(dev, Qreg);
			devdir(c, q, "reg", 0, eve, 0444, dp);
			break;
		case Qsize:
			q.path = QPATH(dev, Qsize);
			devdir(c, q, "size", 0, eve, 0444, dp);
			break;
		case Qirq:
			q.path = QPATH(dev, Qirq);
			devdir(c, q, "irq", 0, eve, 0444, dp);
			break;
		case Qirqflags:
			q.path = QPATH(dev, Qirqflags);
			devdir(c, q, "irqflags", 0, eve, 0444, dp);
			break;
		case Qcompat:
			q.path = QPATH(dev, Qcompat);
			devdir(c, q, "compatible", 0, eve, 0444, dp);
			break;
		case Qclass:
			q.path = QPATH(dev, Qclass);
			devdir(c, q, "class", 0, eve, 0444, dp);
			break;
		case Qconsole:
			q.path = QPATH(dev, Qconsole);
			devdir(c, q, "console", 0, eve, 0444, dp);
			break;
		case Qwait:
			q.path = QPATH(dev, Qwait);
			devdir(c, q, "wait", 0, eve, 0444, dp);
			break;
		case Qfamily:
			q.path = QPATH(dev, Qfamily);
			devdir(c, q, "family", 0, eve, 0444, dp);
			break;
		case Qclaim:
			q.path = QPATH(dev, Qclaim);
			devdir(c, q, "claim", 0, eve, 0666, dp);
			break;
		case Qowner:
			q.path = QPATH(dev, Qowner);
			devdir(c, q, "owner", 0, eve, 0444, dp);
			break;
		case Qirq2:
			q.path = QPATH(dev, Qirq2);
			devdir(c, q, "irq2", 0, eve, 0444, dp);
			break;
		case Qwait2:
			q.path = QPATH(dev, Qwait2);
			devdir(c, q, "wait2", 0, eve, 0444, dp);
			break;
		case Qregs:
			q.path = QPATH(dev, Qregs);
			devdir(c, q, "regs", pnpdev[dev].size, eve, 0666, dp);
			break;
		default:
			return -1;
		}
		return 1;
	case Qdma:
		if(s == DEVDOTDOT){
			q.path = QPATH(0, Qtopdir);
			q.type = QTDIR;
			devdir(c, q, "#P", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		if(s == 0){
			q.path = QPATH(0, Qdmaclone);
			q.type = QTFILE;
			devdir(c, q, "clone", 0, eve, 0666, dp);
			return 1;
		}
		if(s == 1){
			q.path = QPATH(0, Qdmaavail);
			q.type = QTFILE;
			devdir(c, q, "avail", 0, eve, 0444, dp);
			return 1;
		}
		/* then the chunks in use, by slot */
		for(dev = 0, s -= 2; dev < Maxdmachunk; dev++)
			if(dmachunk[dev].inuse && s-- == 0)
				break;
		if(dev == Maxdmachunk)
			return -1;
		snprint(up->genbuf, sizeof up->genbuf, "%d", dev);
		q.path = QPATH(dev, Qchunkdir);
		q.type = QTDIR;
		devdir(c, q, up->genbuf, 0, eve, DMDIR|0555, dp);
		return 1;
	case Qchunkdir:
		dev = DEV(c);
		if(s == DEVDOTDOT){
			q.path = QPATH(npnpdev, Qdma);
			q.type = QTDIR;
			devdir(c, q, "dma", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		if(dev < 0 || dev >= Maxdmachunk || !dmachunk[dev].inuse)
			return -1;
		q.type = QTFILE;
		if(s == 0){
			q.path = QPATH(dev, Qchunkctl);
			devdir(c, q, "ctl", 0, eve, 0666, dp);
			return 1;
		}
		if(s == 1){
			q.path = QPATH(dev, Qchunkdata);
			devdir(c, q, "data", dmachunk[dev].len, eve, 0666, dp);
			return 1;
		}
		return -1;
	case Qtee:
		if(s == DEVDOTDOT){
			q.path = QPATH(0, Qtopdir);
			q.type = QTDIR;
			devdir(c, q, "#P", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		if(s == 0){
			q.path = QPATH(npnpdev, Qteesmc);
			q.type = QTFILE;
			devdir(c, q, "smc", 0, eve, 0600, dp);
			return 1;
		}
		if(s == 1){
			q.path = QPATH(npnpdev, Qteeversion);
			q.type = QTFILE;
			devdir(c, q, "version", 0, eve, 0444, dp);
			return 1;
		}
		if(s == 2){
			q.path = QPATH(npnpdev, Qteeshbase);
			q.type = QTFILE;
			devdir(c, q, "shmbase", 0, eve, 0444, dp);
			return 1;
		}
		if(s == 3){
			q.path = QPATH(npnpdev, Qteeshsize);
			q.type = QTFILE;
			devdir(c, q, "shmsize", 0, eve, 0444, dp);
			return 1;
		}
		/* cache maintenance of the static shared memory, "<pa> <len>" */
		if(s == 4){
			q.path = QPATH(npnpdev, Qteeclean);
			q.type = QTFILE;
			devdir(c, q, "clean", 0, eve, 0600, dp);
			return 1;
		}
		if(s == 5){
			q.path = QPATH(npnpdev, Qteeinval);
			q.type = QTFILE;
			devdir(c, q, "inval", 0, eve, 0600, dp);
			return 1;
		}
		if(s == 6){
			q.path = QPATH(npnpdev, Qteecivac);
			q.type = QTFILE;
			devdir(c, q, "cleaninval", 0, eve, 0600, dp);
			return 1;
		}
		return -1;
	case Qresmem:
		if(s == DEVDOTDOT){
			q.path = QPATH(0, Qtopdir);
			q.type = QTDIR;
			devdir(c, q, "#P", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		if(s < 0 || s >= (int)npnpresmem)
			return -1;
		q.path = QPATH(s, Qresmemdir);
		q.type = QTDIR;
		devdir(c, q, pnpresmem[s].name, 0, eve, DMDIR|0555, dp);
		return 1;
	case Qresmemdir:
		dev = DEV(c);
		if(s == DEVDOTDOT){
			q.path = QPATH(npnpdev, Qresmem);
			q.type = QTDIR;
			devdir(c, q, "resmem", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		if(dev < 0 || dev >= (int)npnpresmem || s != 0)
			return -1;
		q.path = QPATH(dev, Qresmemctl);
		q.type = QTFILE;
		devdir(c, q, "ctl", 0, eve, 0600, dp);
		return 1;
	case Qirqdir:
		if(s == DEVDOTDOT){
			q.path = QPATH(0, Qtopdir);
			q.type = QTDIR;
			devdir(c, q, "#P", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		if(name != nil && s == 0){
			char *e;
			ulong id;
			int edge;

			id = strtoul(name, &e, 10);
			edge = (e != name && e[0] == 'e' && e[1] == 0);
			if(e != name && (*e == 0 || edge) && id >= 32 && id < 1020){
				q.path = QPATH(id, edge ? Qirqedge : Qirqline);
				q.type = QTFILE;
				devdir(c, q, name, 0, eve, 0400, dp);
				return 1;
			}
		}
		return -1;
	}
	return -1;
}

static Chan *
pnpattach(char *spec)
{
	return devattach('P', spec);
}

static Walkqid *
pnpwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, nil, 0, pnpgen);
}

static i32
pnpstat(Chan *c, u8 *db, i32 n)
{
	return devstat(c, db, n, nil, 0, pnpgen);
}

static Chan *
pnpopen(Chan *c, int omode)
{
	int t, m;

	t = TYPE(c);
	m = openmode(omode) & 3;
	if(c->qid.type & QTDIR){
		if(m != OREAD)
			error(Eperm);
	}else if(t == Qdmaclone || t == Qchunkctl || t == Qchunkdata ||
	         t == Qteesmc || t == Qteeclean || t == Qteeinval ||
	         t == Qteecivac || t == Qclaim || t == Qregs ||
	         t == Qresmemctl){
		/* DMA chunks, the SMC gate and claims accept writes. */
	}else if(m != OREAD)
		error(Eperm);
	if(t == Qdmaclone){
		int n;

		/* a new chunk: this file becomes its ctl */
		lock(&dmalock);
		for(n = 0; n < Maxdmachunk; n++)
			if(!dmachunk[n].inuse)
				break;
		if(n == Maxdmachunk){
			unlock(&dmalock);
			error("#P/dma: no free chunk");
		}
		memset(&dmachunk[n], 0, sizeof dmachunk[n]);
		dmachunk[n].inuse = 1;
		dmachunk[n].nchan = 1;
		unlock(&dmalock);
		c->qid.path = QPATH(n, Qchunkctl);
		c->qid.type = QTFILE;
	}else if(t == Qchunkctl || t == Qchunkdata){
		int n;

		n = DEV(c);
		if(n < 0 || n >= Maxdmachunk || !dmaholds(n))
			error(Eperm);
		lock(&dmalock);
		if(!dmachunk[n].inuse){
			unlock(&dmalock);
			error(Eperm);
		}
		dmachunk[n].nchan++;
		unlock(&dmalock);
	}
	if(t == Qteesmc){
		c->aux = mallocz(sizeof(Teesmc), 1);
		if(c->aux == nil)
			error(Enomem);
	}
	if(t == Qresmemctl){
		c->aux = mallocz(sizeof(usize), 1);
		if(c->aux == nil)
			error(Enomem);
	}
	if(t == Qregs){
		if(DEV(c) < 0 || DEV(c) >= (int)npnpdev || !pnpholds(DEV(c)))
			error("device not claimed");
	}
	if(t == Qwait || t == Qwait2 || t == Qirqline || t == Qirqedge){
		int dev;
		u32 irq;

		if(t == Qirqline || t == Qirqedge){
			irq = DEV(c);
			if(irq < 32 || irq >= 1020)
				error(Ebadarg);
			if(t == Qirqedge)
				arm64irqedge(irq);
		}else{
			dev = DEV(c);
			if(dev < 0 || dev >= (int)npnpdev)
				error(Ebadarg);
			if(!pnpholds(dev))
				error("device not claimed");
			irq = (t == Qwait2) ? pnpdev[dev].irq2 : pnpdev[dev].irq;
		}
		{
			int *idp;

			idp = mallocz(sizeof(int), 1);
			if(idp == nil)
				error(Enomem);
			*idp = arm64irqenable(irq);
			if(*idp < 0){
				free(idp);
				error("no interrupt");
			}
			c->aux = idp;
		}
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static void
pnpclose(Chan *c)
{
	int dev;

	/*
	 * A chunk lives while a file on it is open or a segment maps it:
	 * a driver that exits or is killed returns its memory once both
	 * are gone, never while it is still mapped.
	 */
	if((TYPE(c) == Qchunkctl || TYPE(c) == Qchunkdata) &&
	   (c->flag & COPEN)){
		Dmachunk *ck;

		dev = DEV(c);
		if(dev >= 0 && dev < Maxdmachunk){
			ck = &dmachunk[dev];
			lock(&dmalock);
			if(ck->nchan > 0)
				ck->nchan--;
			dmachunkput(ck);
			unlock(&dmalock);
		}
	}
	/*
	 * Closing the wait file must release the GIC line, or a reader that
	 * exits leaves the line claimed and the next consumer gets -1.
	 */
	if((TYPE(c) == Qirqline || TYPE(c) == Qirqedge) && c->aux != nil)
		arm64irqdisable(DEV(c), *(int*)c->aux);
	if((TYPE(c) == Qwait || TYPE(c) == Qwait2) &&
	   (dev = DEV(c)) >= 0 && dev < (int)npnpdev &&
	   c->aux != nil)
		arm64irqdisable(TYPE(c) == Qwait2 ? pnpdev[dev].irq2 : pnpdev[dev].irq,
			*(int*)c->aux);
	/* The last claim file holding a device releases it. */
	if(TYPE(c) == Qclaim && c->aux != nil &&
	   (dev = DEV(c)) >= 0 && dev < (int)npnpdev){
		lock(&pnplock);
		if(pnpclaim[dev].holders > 0 && --pnpclaim[dev].holders == 0)
			memset(&pnpclaim[dev], 0, sizeof pnpclaim[dev]);
		unlock(&pnplock);
	}
	free(c->aux);
	c->aux = nil;
}

/*
 * A chunk's ctl commands:
 *	alloc <size> [<align>] [cached|uncached]
 *	map
 *	clean|inval|cleaninval <off> <len>
 */
static void
dmachunkctl(int n, char *cmd)
{
	Proc *up = externup();
	Dmachunk *ck;
	char *f[5];
	int nf, cached, op;
	u64 size, align, pa, off, len;
	usize va;

	if(n < 0 || n >= Maxdmachunk)
		error(Ebadarg);
	ck = &dmachunk[n];
	nf = tokenize(cmd, f, nelem(f));
	if(nf == 0)
		error(Ebadarg);
	if(strcmp(f[0], "alloc") == 0){
		if(nf < 2 || nf > 4)
			error(Ebadarg);
		size = strtoull(f[1], nil, 0);
		align = nf > 2 ? strtoull(f[2], nil, 0) : 4096;
		cached = 1;
		if(nf > 3){
			if(strcmp(f[3], "uncached") == 0)
				cached = 0;
			else if(strcmp(f[3], "cached") != 0)
				error(Ebadarg);
		}
		/* one alloc per chunk, even from threads sharing its ctl */
		lock(&dmalock);
		if(ck->len != 0 || ck->allocating){
			unlock(&dmalock);
			error("#P/dma: chunk already allocated");
		}
		ck->allocating = 1;
		unlock(&dmalock);
		if(waserror()){
			ck->allocating = 0;
			nexterror();
		}
		pa = dmaalloc(size, align);
		poperror();
		size = PGROUND(size);
		/*
		 * Zero it, through the kernel's write-back alias, and
		 * push that out of the caches: a device, or an uncached
		 * mapping, then sees zeros, and no line of the previous
		 * owner's data is left to be written back later.
		 */
		memset(KADDR(pa), 0, size);
		dmacache(Dcleaninval, pa, size);
		lock(&dmalock);
		ck->pa = pa;
		ck->len = size;
		ck->cached = cached;
		ck->ps.attr = SG_PHYSICAL | SG_READ | SG_WRITE |
			(cached ? SG_CACHED : SG_NCRAM);
		ck->ps.name = "dmachunk";
		ck->ps.pa = pa;
		ck->ps.size = (size + BIGPGSZ - 1) / BIGPGSZ;
		/* 2 MiB pages where the chunk allows: a guest's RAM then
		 * costs 512 times fewer page-table entries */
		if((pa | size) & (BIGPGSZ - 1))
			ck->ps.pgszi = getpgszi(PGSZ);
		else
			ck->ps.pgszi = getpgszi(BIGPGSZ);
		ck->ps.release = dmachunkrelease;
		ck->allocating = 0;
		unlock(&dmalock);
		return;
	}
	if(ck->len == 0)
		error("#P/dma: chunk not allocated");
	if(strcmp(f[0], "map") == 0){
		if(nf != 1)
			error(Ebadarg);
		lock(&dmalock);
		ck->nmap++;	/* the segment's reference, taken before it exists */
		unlock(&dmalock);
		if(waserror()){
			lock(&dmalock);
			ck->nmap--;
			dmachunkput(ck);
			unlock(&dmalock);
			nexterror();
		}
		va = segattachps(up, SG_READ | SG_WRITE, &ck->ps, 0, ck->len);
		poperror();
		ck->va = va;
		return;
	}
	if(strcmp(f[0], "clean") == 0)
		op = Dclean;
	else if(strcmp(f[0], "inval") == 0)
		op = Dinval;
	else if(strcmp(f[0], "cleaninval") == 0)
		op = Dcleaninval;
	else
		error(Ebadarg);
	if(nf != 3)
		error(Ebadarg);
	off = strtoull(f[1], nil, 0);
	len = strtoull(f[2], nil, 0);
	if(len == 0 || off > ck->len || len > ck->len - off)
		error(Ebadarg);
	dmacache(op, ck->pa + off, len);
}

/* Reads or writes a chunk through the kernel's alias.  An uncached
 * chunk's range is cleaned and invalidated around the access, so the
 * alias neither reads stale lines nor leaves dirty ones. */
static i32
dmachunkio(Chan *c, void *a, i32 n, i64 off, int write)
{
	Dmachunk *ck;
	u64 pa;
	int dev;

	dev = DEV(c);
	if(dev < 0 || dev >= Maxdmachunk)
		error(Ebadarg);
	ck = &dmachunk[dev];
	if(ck->len == 0)
		error("#P/dma: chunk not allocated");
	if(off < 0 || (u64)off >= ck->len){
		if(write)
			error("#P/dma: write past the chunk");
		return 0;
	}
	if((u64)n > ck->len - off)
		n = ck->len - off;
	pa = ck->pa + off;
	if(!ck->cached)
		dmacache(Dcleaninval, pa, n);
	if(write)
		memmove(KADDR(pa), a, n);
	else
		memmove(a, KADDR(pa), n);
	if(!ck->cached)
		dmacache(Dcleaninval, pa, n);
	return n;
}

static i32
pnpwrite(Chan *c, void *a, i32 n, i64 off)
{
	Dmactl *ct;
	char buf[Dmactlmax], *p;
	u64 v, len;
	int i, done;

	USED(off);
	if(n <= 0)
		error(Ebadarg);
	if(TYPE(c) == Qclaim){
		char name[48], *f[3];
		int nf, dev, shared;
		Pnpclaim *cl;

		dev = DEV(c);
		if(dev < 0 || dev >= (int)npnpdev)
			error(Ebadarg);
		if(c->aux != nil)
			error("claim already held through this file");
		if(n >= (int)sizeof name)
			error(Ebadarg);
		memmove(name, a, n);
		name[n] = 0;
		nf = tokenize(name, f, nelem(f));
		shared = nf == 2 && strcmp(f[1], "shared") == 0;
		if(nf < 1 || nf > 2 || (nf == 2 && !shared) ||
		   strlen(f[0]) >= sizeof cl->owner)
			error(Ebadarg);
		cl = &pnpclaim[dev];
		lock(&pnplock);
		if(cl->holders > 0 && (!shared || !cl->shared)){
			unlock(&pnplock);
			error("device already claimed");
		}
		if(cl->holders == 0){
			strcpy(cl->owner, f[0]);
			cl->shared = shared;
		}
		cl->holders++;
		unlock(&pnplock);
		c->aux = mallocz(sizeof(int), 1);	/* marks the file a holder */
		if(c->aux == nil){
			lock(&pnplock);
			if(--cl->holders == 0)
				memset(cl, 0, sizeof *cl);
			unlock(&pnplock);
			error(Enomem);
		}
		return n;
	}
	if(TYPE(c) == Qregs)
		return pnpregs(c, a, n, off, 1);
	if(TYPE(c) == Qresmemctl){
		Proc *up = externup();
		Pnpresmem *rm;
		char cmd[32], *f[2];
		int dev, nf;
		usize va;

		dev = DEV(c);
		if(dev < 0 || dev >= (int)npnpresmem || c->aux == nil)
			error(Ebadarg);
		if(n >= (int)sizeof cmd)
			error(Ebadarg);
		memmove(cmd, a, n);
		cmd[n] = 0;
		nf = tokenize(cmd, f, nelem(f));
		if(nf != 1 || strcmp(f[0], "map") != 0)
			error(Ebadarg);
		rm = &pnpresmem[dev];
		if(rm->ps.pgszi < 0){
			if((rm->pa | rm->len) & (BIGPGSZ - 1))
				rm->ps.pgszi = getpgszi(PGSZ);
			else
				rm->ps.pgszi = getpgszi(BIGPGSZ);
		}
		va = segattachps(up, SG_READ | SG_WRITE, &rm->ps, 0, rm->len);
		*(usize*)c->aux = va;
		return n;
	}
	if(TYPE(c) == Qteesmc){
		Teesmc req, *res;

		if(n != sizeof req)
			error(Ebadarg);
		res = c->aux;
		if(res == nil)
			error(Ebadarg);
		memmove(&req, a, n);
		if(!teesmcok(req.a[0], req.a[1], req.a[2]))
			error(Eperm);
		lock(&teelock);
		teesmccall(req.a, res->a);
		unlock(&teelock);
		return n;
	}
	if(TYPE(c) == Qchunkdata)
		return dmachunkio(c, a, n, off, 1);
	if((ct = c->aux) == nil){
		ct = mallocz(sizeof *ct, 1);
		if(ct == nil)
			error(Enomem);
		c->aux = ct;
	}
	done = 0;
	for(i = 0; i < n && !done; i++){
		if(ct->n >= Dmactlmax-1){
			ct->n = 0;
			error(Ebadarg);
		}
		if(((char*)a)[i] == '\n')
			done = 1;
		else
			ct->buf[ct->n++] = ((char*)a)[i];
	}
	if(!done)
		return n;
	memmove(buf, ct->buf, ct->n);
	buf[ct->n] = 0;
	ct->n = 0;
	p = buf;
	switch(TYPE(c)){
	case Qchunkctl:
		dmachunkctl(DEV(c), buf);
		break;
	case Qteeclean:
	case Qteeinval:
	case Qteecivac:
		if(number(&p, &v) < 0 || number(&p, &len) < 0 ||
		   !inrange(v, len, arm64teeshmbase, arm64teeshmsize))
			error(Ebadarg);
		dmacache(TYPE(c) == Qteeclean ? Dclean :
		         TYPE(c) == Qteeinval ? Dinval : Dcleaninval, v, len);
		break;
	default:
		error(Eperm);
	}
	return n;
}

static i32
pnpread(Chan *c, void *a, i32 n, i64 off)
{
	Fdtdev *d;
	char buf[64];
	int dev;

	if(c->qid.type & QTDIR)
		return devdirread(c, a, n, nil, 0, pnpgen);
	if(TYPE(c) == Qdmaavail){
		/* Free arena bytes, holes and the untouched tail, and the
		 * largest piece: lets a supervisor check there is room
		 * before launching a guest instead of retrying into an
		 * exhausted arena. */
		char dbuf[64];
		u64 avail, big, tail;
		int i;

		lock(&dmalock);
		tail = arm64dmasize == 0 ? 0 :
			dmaend() - (dmabump == 0 ? arm64dmabase : dmabump);
		avail = big = tail;
		for(i = 0; i < ndmahole; i++){
			avail += dmahole[i].size;
			if(dmahole[i].size > big)
				big = dmahole[i].size;
		}
		unlock(&dmalock);
		snprint(dbuf, sizeof dbuf, "%llud %llud\n", avail, big);
		return readstr(off, a, n, dbuf);
	}
	if(TYPE(c) == Qchunkctl){
		/* "<n> pa <pa> len <len> cached|uncached va <va>" */
		char dbuf[128];
		Dmachunk *ck;

		dev = DEV(c);
		if(dev < 0 || dev >= Maxdmachunk)
			error(Ebadarg);
		ck = &dmachunk[dev];
		snprint(dbuf, sizeof dbuf, "%d pa %#llux len %#llux %s va %#p\n",
			dev, ck->pa, ck->len, ck->cached ? "cached" : "uncached",
			ck->va);
		return readstr(off, a, n, dbuf);
	}
	if(TYPE(c) == Qchunkdata)
		return dmachunkio(c, a, n, off, 0);
	if(TYPE(c) == Qteeversion){
		return readstr(off, a, n, "optee-smc-gate 1\n");
	}
	if(TYPE(c) == Qteeshbase){
		snprint(buf, sizeof buf, "0x%llx\n", arm64teeshmbase);
		return readstr(off, a, n, buf);
	}
	if(TYPE(c) == Qteeshsize){
		snprint(buf, sizeof buf, "0x%llx\n", arm64teeshmsize);
		return readstr(off, a, n, buf);
	}
	if(TYPE(c) == Qresmemctl){
		char rbuf[128];
		Pnpresmem *rm;
		usize va;

		dev = DEV(c);
		if(dev < 0 || dev >= (int)npnpresmem)
			error(Ebadarg);
		rm = &pnpresmem[dev];
		va = c->aux != nil ? *(usize*)c->aux : 0;
		snprint(rbuf, sizeof rbuf, "%s pa %#llux len %#llux va %#p\n",
			rm->name, rm->pa, rm->len, (void*)va);
		return readstr(off, a, n, rbuf);
	}
	if(TYPE(c) == Qteesmc){
		Teesmc *res;
		int len;

		res = c->aux;
		if(res == nil)
			error(Ebadarg);
		len = Teesmcres * sizeof(u64);
		if(off >= len)
			return 0;
		if(n > len - off)
			n = len - off;
		memmove(a, (u8*)res + off, n);
		return n;
	}
	if(TYPE(c) == Qplatform){
		char pbuf[1024];
		Fdtinfo *pi;
		int len, i;

		pi = pnpinfo;
		if(pi == nil)
			return 0;
		len = snprint(pbuf, sizeof pbuf,
		        "board %s\n"
		        "model %s\n"
		        "compatible %s\n"
		        "ram 0x%llx 0x%llx\n"
		        "bootarchive 0x%llx 0x%llx\n"
		        "teeshm 0x%llx 0x%llx\n"
		        "scm 0x%llx 0x%llx\n"
		        "framebuffer 0x%llx 0x%llx %u %u %u %u\n",
		        fdtboardname(pi->board),
		        pi->model,
		        pi->rootcompat,
		        pi->rambase, pi->ramsize,
		        pi->bootarchbase, pi->bootarchsize,
		        pi->teeshmbase, pi->teeshmsize,
		        pi->scmbase, pi->scmsize,
		        pi->fbbase, pi->fbsize,
		        pi->fbwidth, pi->fbheight, pi->fbstride, pi->fbbpp);
		for(i = 0; i < (int)pi->nusable && i < 8 &&
		    len < (int)sizeof pbuf; i++)
			len += snprint(pbuf+len, sizeof pbuf-len,
			               "memory 0x%llx 0x%llx\n",
			               pi->usable[i].base, pi->usable[i].size);
		if(len >= (int)sizeof pbuf)
			len = sizeof pbuf-1;
		return readstr(off, a, n, pbuf);
	}
	if(TYPE(c) == Qirqline || TYPE(c) == Qirqedge){
		int cnt;

		if(c->aux == nil)
			error("no interrupt");
		cnt = arm64irqwait(DEV(c), *(int*)c->aux);
		if(cnt < 0)
			error("no interrupt");
		snprint(buf, sizeof buf, "%d\n", cnt);
		return readstr(off, a, n, buf);
	}
	dev = DEV(c);	if(dev < 0 || dev >= (int)npnpdev)
		error(Ebadarg);
	d = &pnpdev[dev];
	switch(TYPE(c)){
	case Qreg:
		snprint(buf, sizeof buf, "0x%llx\n", d->base);
		break;
	case Qsize:
		snprint(buf, sizeof buf, "0x%llx\n", d->size);
		break;
	case Qirq:
		snprint(buf, sizeof buf, "%u\n", d->irq);
		break;
	case Qirq2:
		snprint(buf, sizeof buf, "%u\n", d->irq2);
		break;
	case Qirqflags:
		snprint(buf, sizeof buf, "%u\n", d->irqflags);
		break;
	case Qcompat:
		snprint(buf, sizeof buf, "%s\n", d->compat);
		break;
	case Qclass:
		snprint(buf, sizeof buf, "%s\n", fdtclassname(d->clas));
		break;
	case Qconsole:
		snprint(buf, sizeof buf, "%u\n", d->base == consolebase);
		break;
	case Qwait:
	case Qwait2: {
		int n;

		if(c->aux == nil)
			error("no interrupt");
		n = arm64irqwait(TYPE(c) == Qwait2 ? d->irq2 : d->irq, *(int*)c->aux);
		if(n < 0)
			error("no interrupt");
		snprint(buf, sizeof buf, "%d\n", n);
		break;
	}
	case Qfamily:
		snprint(buf, sizeof buf, "%s\n", pnpfamily(d->clas));
		break;
	case Qowner:
		lock(&pnplock);
		snprint(buf, sizeof buf, "%s%s\n", pnpclaim[dev].owner,
			pnpclaim[dev].shared ? " shared" : "");
		unlock(&pnplock);
		break;
	case Qregs:
		return pnpregs(c, a, n, off, 0);
	default:
		error(Ebadarg);
	}
	return readstr(off, a, n, buf);
}

Dev pnpdevtab = {
	.dc = 'P',
	.name = "pnp",

	.reset = devreset,
	.init = devinit,
	.shutdown = devshutdown,
	.attach = pnpattach,
	.walk = pnpwalk,
	.stat = pnpstat,
	.open = pnpopen,
	.create = devcreate,
	.close = pnpclose,
	.read = pnpread,
	.bread = devbread,
	.write = pnpwrite,
	.bwrite = devbwrite,
	.remove = devremove,
	.wstat = devwstat,
};
