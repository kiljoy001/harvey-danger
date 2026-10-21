/*
 * VOP scanout device.
 *
 * o9display owns composition; this device is the bare-metal output below
 * it.  The first version is a single linear XRGB8888 buffer.  Userspace
 * writes pixel bytes at their buffer offset and the VOP scans them out.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "vop2.h"

enum {
	Qdir,
	Qdisplay,
	Qctl,
	Qdata,
	Qevents,
};

static Dirtab vopdir[] = {
	{".", {Qdir, 0, QTDIR}, 0, DMDIR | 0555},
	{"display", {Qdisplay, 0, QTDIR}, 0, DMDIR | 0555},
};

static Dirtab displaydir[] = {
	{".", {Qdisplay, 0, QTDIR}, 0, DMDIR | 0555},
	{"ctl", {Qctl, 0, 0}, 0, 0666},
	{"data", {Qdata, 0, 0}, 0, 0666},
	{"events", {Qevents, 0, 0}, 0, 0444},
};

static QLock voplk;

typedef struct Vopctl Vopctl;
struct Vopctl {
	int	n;
	char	buf[96];
};

static int
vopgen(Chan *c, char *name, Dirtab *tab, int ntab, int s, Dir *dp)
{
	Qid q;

	USED(tab);
	USED(ntab);
	switch((int)c->qid.path){
	case Qdir:
		if(s == DEVDOTDOT){
			q = (Qid){Qdir, 0, QTDIR};
			devdir(c, q, ".", 0, eve, DMDIR | 0555, dp);
			return 1;
		}
		return devgen(c, name, vopdir, nelem(vopdir), s, dp);
	case Qdisplay:
		if(s == DEVDOTDOT){
			q = (Qid){Qdir, 0, QTDIR};
			devdir(c, q, "..", 0, eve, DMDIR | 0555, dp);
			return 1;
		}
		return devgen(c, name, displaydir, nelem(displaydir), s, dp);
	}
	return -1;
}

static Chan *
vopattach(char *spec)
{
	return devattach('V', spec);
}

static Walkqid *
vopwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, nil, 0, vopgen);
}

static i32
vopstat(Chan *c, u8 *db, i32 n)
{
	return devstat(c, db, n, nil, 0, vopgen);
}

static Chan *
vopopen(Chan *c, int omode)
{
	Vopctl *v;

	c = devopen(c, omode, nil, 0, vopgen);
	if((int)c->qid.path == Qctl){
		v = mallocz(sizeof *v, 1);
		if(v == nil)
			error(Enomem);
		c->aux = v;
	}
	return c;
}

static void
vopclose(Chan *c)
{
	free(c->aux);
	c->aux = nil;
}

static i32
vopread(Chan *c, void *a, i32 n, i64 offset)
{
	char buf[96];
	u64 avail;

	switch((int)c->qid.path){
	case Qdir:
	case Qdisplay:
		return devdirread(c, a, n, nil, 0, vopgen);
	case Qctl:
		if(vopfb.size == 0)
			return 0;
		snprint(buf, sizeof buf, "vop 1 xrgb8888 %u %u %u\n",
			vopfb.width, vopfb.height, vopfb.stride);
		return readstr(offset, a, n, buf);
	case Qdata:
		qlock(&voplk);
		if(vopfb.size == 0 || offset < 0 ||
		   (u64)offset >= vopfb.size){
			qunlock(&voplk);
			return 0;
		}
		avail = vopfb.size - (u64)offset;
		if((u64)n > avail)
			n = avail;
		memmove(a, KADDR(vopfb.pa + offset), n);
		qunlock(&voplk);
		return n;
	case Qevents:
		qlock(&voplk);
		snprint(buf, sizeof buf, "frame %llu\n", vopfb.seq);
		qunlock(&voplk);
		if(n > (i32)strlen(buf))
			n = strlen(buf);
		memmove(a, buf, n);
		c->offset = 0;	/* polling endpoint; each read is current */
		return n;
	}
	return -1;
}

static void
vopfill(u32 color)
{
	u32 *p;
	u64 i;

	qlock(&voplk);
	if(vopfb.size == 0 || vopfb.pa == 0){
		qunlock(&voplk);
		error(Ebadarg);
	}
	p = KADDR(vopfb.pa);
	for(i = 0; i < vopfb.size / 4; i++)
		p[i] = 0xff000000 | color;
	dcacheclean(KADDR(vopfb.pa), vopfb.size);
	vopfb.seq++;
	qunlock(&voplk);
}

static i32
vopwrite(Chan *c, void *va, i32 n, i64 offset)
{
	Vopctl *v;
	char *s;
	int i, ch;

	switch((int)c->qid.path){
	case Qctl:
		v = c->aux;
		if(v == nil || n <= 0)
			error(Ebadarg);
		s = va;
		for(i = 0; i < n; i++){
			ch = s[i];
			if(ch == '\n'){
				if(v->n > 0 && v->buf[v->n-1] == '\r')
					v->n--;
				v->buf[v->n] = 0;
				if(strncmp(v->buf, "fill ", 5) == 0)
					vopfill(strtoul(v->buf + 5, nil, 16));
				else if(strncmp(v->buf, "commit", 6) == 0){
					qlock(&voplk);
					vopfb.seq++;
					qunlock(&voplk);
				}else if(v->n != 0)
					error(Ebadarg);
				v->n = 0;
				continue;
			}
			if(v->n >= (int)sizeof v->buf - 1){
				v->n = 0;
				error(Ebadarg);
			}
			v->buf[v->n++] = ch;
		}
		return n;
	case Qdata:
		if(vopfb.size == 0 || vopfb.pa == 0 ||
		   offset < 0 || n <= 0 ||
		   (u64)offset > vopfb.size ||
		   (u64)n > vopfb.size - (u64)offset)
			error(Ebadarg);
		qlock(&voplk);
		memmove(KADDR(vopfb.pa + offset), va, n);
		dcacheclean(KADDR(vopfb.pa + offset), n);
		vopfb.seq++;
		qunlock(&voplk);
		return n;
	case Qevents:
		error(Eperm);
	}
	return -1;
}

Dev vopdevtab = {
	.dc = 'V',
	.name = "vop",

	.reset = devreset,
	.init = devinit,
	.shutdown = devshutdown,
	.attach = vopattach,
	.walk = vopwalk,
	.stat = vopstat,
	.open = vopopen,
	.create = devcreate,
	.close = vopclose,
	.read = vopread,
	.bread = devbread,
	.write = vopwrite,
	.bwrite = devbwrite,
	.remove = devremove,
	.wstat = devwstat,
};
