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
	Qdma,
	Qdmabase,
	Qdmasize,
};

#define TYPE(x) ((int)((x)->qid.path & 0xf))
#define DEV(x)  ((int)(((x)->qid.path >> 4) & 0xfff))
#define QPATH(d, t) (((u64)(d) << 4) | (t))

static Fdtdev pnpdev[Maxfdtdev];
static u32 npnpdev;
static u64 consolebase;
extern u64 arm64dmabase, arm64dmasize;

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
		if(s < 0 || s > (int)npnpdev || (s == (int)npnpdev && arm64dmasize == 0))
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
		if(dev < 0 || dev >= (int)npnpdev || s < 0 || s > Qconsole-Qreg)
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
	if(omode != OREAD)
		error(Eperm);
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

static void
pnpclose(Chan *c)
{
	USED(c);
}

static i32
pnpwrite(Chan *c, void *a, i32 n, i64 off)
{
	USED(c);
	USED(a);
	USED(n);
	USED(off);
	error(Eperm);
	return 0;
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
