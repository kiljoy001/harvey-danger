/* Read-only device-tree device discovery for ARM64 user-space drivers. */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "fdt.h"

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
	Qdma,
	Qdmabase,
	Qdmasize,
	Qdmaalloc,
	Qdmafree,
	Qdmaclean,
	Qdmainval,
	Qdmacivac,
	Qtee,
	Qteesmc,
	Qteeversion,
	Qteeshbase,
	Qteeshsize,
};

/* 6 type bits: the DMA control files are past the old 4-bit field. */
#define TYPE(x) ((int)((x)->qid.path & 0x3f))
#define DEV(x)  ((int)(((x)->qid.path >> 6) & 0x3ff))
#define QPATH(d, t) (((u64)(d) << 6) | (t))

static Fdtdev pnpdev[Maxfdtdev];
static u32 npnpdev;
static u64 consolebase;
extern u64 arm64dmabase, arm64dmasize;
extern u64 arm64teeshmbase, arm64teeshmsize;

/*
 * DMA sub-allocation inside the reserved arena.  A driver maps the arena
 * once with segattach("dma") and then asks here for aligned chunks; the
 * kernel owns the free list so two drivers cannot hand out the same page.
 */
enum { Maxdmahole = 64 };
typedef struct Dmahole Dmahole;
struct Dmahole {
	u64 base;
	u64 size;
};
static Dmahole dmahole[Maxdmahole];
static int ndmahole;
static u64 dmabump;
static Lock dmalock;

/* Most recent allocation, reported by reading #P/dma/alloc. */
static struct {
	u64 pa;
	u64 len;
} dmalast;

/*
 * DMA commands are newline terminated: a text program may write the
 * command in several pieces, and a driver must not allocate once per
 * write.
 */
enum { Dmactlmax = 96 };
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
	Teesmcres	= 4,	/* a0..a3 */
};
typedef struct Teesmc Teesmc;
struct Teesmc {
	u64 a[Teesmcreq];
};
static Teesmc teereq, teeres;
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
}

static int inrange(u64, u64, u64, u64);

/*
 * OP-TEE function IDs we are willing to forward.  PSCI and every other
 * secure-monitor service stay in the kernel.
 */
static int
teesmcok(u64 fn, u64 hi, u64 lo)
{
	u64 pa;

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

static u64
dmaalloc(u64 size, u64 align)
{
	u64 a, end;
	int i;

	if(size == 0 || size > arm64dmasize)
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
		a = (dmahole[i].base + align - 1) & ~(align - 1);
		end = dmahole[i].base + dmahole[i].size;
		if(a + size > end)
			continue;
		if(a + size == end)
			dmahole[i].size = a - dmahole[i].base;
		else{
			dmahole[i].base = a + size;
			dmahole[i].size = end - dmahole[i].base;
		}
		if(dmahole[i].size == 0){
			dmahole[i] = dmahole[--ndmahole];
		}
		unlock(&dmalock);
		return a;
	}
	a = (dmabump + align - 1) & ~(align - 1);
	if(a + size > arm64dmabase + arm64dmasize){
		unlock(&dmalock);
		error(Enomem);
	}
	dmabump = a + size;
	unlock(&dmalock);
	return a;
}

static void
dmafree(u64 pa, u64 len)
{
	Dmahole h;
	int i, j;

	if(len == 0 || pa < arm64dmabase ||
	   pa - arm64dmabase > arm64dmasize || len > arm64dmasize - (pa - arm64dmabase))
		error(Ebadarg);
	len = PGROUND(len);
	lock(&dmalock);
	if(ndmahole >= Maxdmahole){
		unlock(&dmalock);
		error(Enomem);
	}
	h.base = pa;
	h.size = len;
	for(i = 0; i < ndmahole && dmahole[i].base < pa; i++)
		;
	for(j = ndmahole; j > i; j--)
		dmahole[j] = dmahole[j-1];
	dmahole[i] = h;
	ndmahole++;
	/* coalesce with the next and previous holes */
	for(i = 0; i + 1 < ndmahole; ){
		if(dmahole[i].base + dmahole[i].size == dmahole[i+1].base){
			dmahole[i].size += dmahole[i+1].size;
			for(j = i + 1; j + 1 < ndmahole; j++)
				dmahole[j] = dmahole[j+1];
			ndmahole--;
		}else
			i++;
	}
	unlock(&dmalock);
}

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
	npnpdev = 0;
	for(i = 0; i < info->ndev && npnpdev < Maxfdtdev; i++){
		if(info->dev[i].clas == Fdtclassiommu)
			continue;
		pnpdev[npnpdev++] = info->dev[i];
	}
}

static int
pnpgen(Chan *c, char *name, Dirtab *tab, int ntab, int s, Dir *dp)
{
	Qid q;
	Fdtdev *d;
	int dev;

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
		if(arm64dmasize != 0 && s == (int)npnpdev){
			q.path = QPATH(npnpdev, Qdma);
			q.type = QTDIR;
			devdir(c, q, "dma", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		if(s == (int)npnpdev + (arm64dmasize != 0)){
			q.path = QPATH(npnpdev, Qtee);
			q.type = QTDIR;
			devdir(c, q, "tee", 0, eve, DMDIR|0555, dp);
			return 1;
		}
		if(s < 0 || s >= (int)npnpdev + 1 + (arm64dmasize != 0))
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
		if(dev < 0 || dev >= (int)npnpdev || s < 0 || s > Qwait-Qreg)
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
			q.path = QPATH(npnpdev, Qdmabase);
			q.type = QTFILE;
			devdir(c, q, "base", 0, eve, 0444, dp);
			return 1;
		}
		if(s == 1){
			q.path = QPATH(npnpdev, Qdmasize);
			q.type = QTFILE;
			devdir(c, q, "size", 0, eve, 0444, dp);
			return 1;
		}
		if(s == 2){
			q.path = QPATH(npnpdev, Qdmaalloc);
			q.type = QTFILE;
			devdir(c, q, "alloc", 0, eve, 0666, dp);
			return 1;
		}
		if(s == 3){
			q.path = QPATH(npnpdev, Qdmafree);
			q.type = QTFILE;
			devdir(c, q, "free", 0, eve, 0666, dp);
			return 1;
		}
		if(s == 4){
			q.path = QPATH(npnpdev, Qdmaclean);
			q.type = QTFILE;
			devdir(c, q, "clean", 0, eve, 0666, dp);
			return 1;
		}
		if(s == 5){
			q.path = QPATH(npnpdev, Qdmainval);
			q.type = QTFILE;
			devdir(c, q, "inval", 0, eve, 0666, dp);
			return 1;
		}
		if(s == 6){
			q.path = QPATH(npnpdev, Qdmacivac);
			q.type = QTFILE;
			devdir(c, q, "cleaninval", 0, eve, 0666, dp);
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
	}else if(t == Qdmaalloc || t == Qdmafree || t == Qdmaclean ||
	         t == Qdmainval || t == Qdmacivac || t == Qteesmc){
		/* DMA control files and the SMC gate accept reads and writes. */
	}else if(m != OREAD)
		error(Eperm);
	if(t == Qwait){
		int dev;

		dev = DEV(c);
		if(dev < 0 || dev >= (int)npnpdev)
			error(Ebadarg);
		if(arm64irqenable(pnpdev[dev].irq) < 0)
			error("no interrupt");
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static void
pnpclose(Chan *c)
{
	free(c->aux);
	c->aux = nil;
}

static i32
pnpwrite(Chan *c, void *a, i32 n, i64 off)
{
	Dmactl *ct;
	char buf[Dmactlmax], *p;
	u64 v, len, pa;
	int i, done;

	USED(off);
	if(n <= 0)
		error(Ebadarg);
	if(TYPE(c) == Qteesmc){
		if(n != sizeof teereq)
			error(Ebadarg);
		lock(&teelock);
		memmove(&teereq, a, n);
		if(!teesmcok(teereq.a[0], teereq.a[1], teereq.a[2])){
			unlock(&teelock);
			error(Eperm);
		}
		teesmccall(teereq.a, teeres.a);
		unlock(&teelock);
		return n;
	}
	if((ct = c->aux) == nil){
		ct = malloc(sizeof *ct);
		if(ct == nil)
			error(Enomem);
		ct->n = 0;
		c->aux = ct;
	}
	done = 0;
	for(i = 0; i < n; i++){
		if(ct->n >= Dmactlmax-1){
			ct->n = 0;
			error(Ebadarg);
		}
		ct->buf[ct->n++] = ((char*)a)[i];
		if(((char*)a)[i] == '\n')
			done = 1;
	}
	if(!done)
		return n;
	memmove(buf, ct->buf, ct->n);
	buf[ct->n] = 0;
	ct->n = 0;
	p = buf;
	switch(TYPE(c)){
	case Qdmaalloc:
		if(number(&p, &v) < 0)
			error(Ebadarg);
		if(number(&p, &len) < 0 || len == 0)
			len = 4096;
		pa = dmaalloc(v, len);
		lock(&dmalock);
		dmalast.pa = pa;
		dmalast.len = PGROUND(v);
		unlock(&dmalock);
		break;
	case Qdmafree:
		if(number(&p, &v) < 0 || number(&p, &len) < 0)
			error(Ebadarg);
		dmafree(v, len);
		break;
	case Qdmaclean:
	case Qdmainval:
	case Qdmacivac:
		if(number(&p, &v) < 0 || number(&p, &len) < 0)
			error(Ebadarg);
		dmacache(TYPE(c) == Qdmaclean ? Dclean :
		         TYPE(c) == Qdmainval ? Dinval : Dcleaninval, v, len);
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
	if(TYPE(c) == Qdmabase){
		char dbuf[64];
		snprint(dbuf, sizeof dbuf, "0x%llx\n", arm64dmabase);
		return readstr(off, a, n, dbuf);
	}
	if(TYPE(c) == Qdmasize){
		char dbuf[64];
		snprint(dbuf, sizeof dbuf, "0x%llx\n", arm64dmasize);
		return readstr(off, a, n, dbuf);
	}
	if(TYPE(c) == Qdmaalloc){
		char dbuf[64];

		lock(&dmalock);
		if(dmalast.len == 0){
			unlock(&dmalock);
			return 0;
		}
		snprint(dbuf, sizeof dbuf, "0x%llx %llux\n", dmalast.pa,
		        dmalast.len);
		unlock(&dmalock);
		return readstr(off, a, n, dbuf);
	}
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
	if(TYPE(c) == Qteesmc){
		int len;

		lock(&teelock);
		len = Teesmcres * sizeof(u64);
		if(off >= len){
			unlock(&teelock);
			return 0;
		}
		if(n > len - off)
			n = len - off;
		memmove(a, (u8*)&teeres + off, n);
		unlock(&teelock);
		return n;
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
	case Qwait: {
		int n;

		n = arm64irqwait(d->irq);
		if(n < 0)
			error("no interrupt");
		snprint(buf, sizeof buf, "%d\n", n);
		break;
	}
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
