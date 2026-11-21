/* touchfs: a touchscreen as a 9P file server.
 *
 * A touchscreen is not a mouse: it has several contacts, each with its own
 * position and lifetime, and no buttons.  touchfs posts /srv/touch with
 *
 *	/events	read	one line per frame, blocking until the next one:
 *			msec n [id x y pressure major]...
 *			every contact down in that frame, by tracking id;
 *			n is 0 once the last finger lifts.  Each open file
 *			has its own queue, so a quick tap is not merged away.
 *	/status	read	device, event source, coordinate range, frames, and the
 *			latest frame, for looking without waiting on events
 *
 * Coordinates are the controller's, with the range in status.
 *
 * Today the events come from a Linux driver in an LKL guest: touchfs mounts
 * /srv/lkl, finds the guest input device whose ABS capabilities include
 * multi-touch X and Y (protocol B, with slots), and decodes its evdev
 * records.  The range comes from the touch node in the guest's device tree:
 * the standard touchscreen-size-x/y, else the vendor properties in sizes[].
 * A native driver can replace the source without changing these files.
 *
 * With no /srv/lkl or no multi-touch device touchfs exits: it runs on every
 * board and serves only where a touchscreen is found.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"

enum {
	/* Linux input event types and codes */
	EvSyn		= 0x00,
	EvAbs		= 0x03,
	SynReport	= 0,
	SynDropped	= 3,
	AbsMtSlot	= 0x2f,
	AbsMtMajor	= 0x30,
	AbsMtX		= 0x35,
	AbsMtY		= 0x36,
	AbsMtId		= 0x39,
	AbsMtPressure	= 0x3a,

	Evsize		= 24,	/* struct input_event on a 64-bit guest */
	Nslot		= 16,
	Nqueue		= 64,	/* frames held per open events file */
	Linesz		= 16 + Nslot*40,

	Srvwait		= 900,	/* tenths of a second for /srv/lkl */
	Devwait		= 600,	/* and then for the input device */
};

typedef struct Slot Slot;
struct Slot {
	int	id;	/* tracking id, -1 when no contact */
	int	x, y, p, a;
};

typedef struct Reader Reader;
struct Reader {
	Reader	*next;
	char	*q[Nqueue];
	int	head, n;	/* oldest frame, frames queued */
	Req	*wait[8];	/* reads waiting for a frame */
	int	nwait;
};

typedef struct Touch Touch;
struct Touch {
	QLock	lk;
	Reader	*readers;
	char	dev[64];	/* guest input device name */
	char	src[64];	/* its event node */
	int	w, h;		/* coordinate range, 0 when unknown */
	uvlong	frames;
	uvlong	dropped;
	char	last[Linesz];	/* the latest frame */
};

static Touch touch;
static char Lkl[] = "/mnt/lkl";

/* Properties that give a touchscreen's coordinate range: x and y cells,
 * or a box as x0 y0 x1 y1. */
static struct {
	char	*x, *y;	/* one cell each */
	char	*box;	/* or four cells */
	char	*xy;	/* or two cells */
} sizes[] = {
	{ "touchscreen-size-x", "touchscreen-size-y", nil, nil },
	{ nil, nil, "focaltech,display-coords", nil },
	{ nil, nil, nil, "novatek,def-max-resolution" },
};

static long
readfile(char *path, char *buf, long n)
{
	int fd;
	long m, t;

	fd = open(path, OREAD);
	if(fd < 0)
		return -1;
	for(t = 0; t < n - 1; t += m){
		m = read(fd, buf + t, n - 1 - t);
		if(m <= 0)
			break;
	}
	close(fd);
	buf[t] = 0;
	return t;
}

/* Bit b of a capability bitmap, printed as hex words, most significant
 * first. */
static int
capbit(char *s, int b)
{
	char *w[16];
	int n, k;
	uvlong v;

	n = tokenize(s, w, nelem(w));
	k = n - 1 - b/64;
	if(k < 0)
		return 0;
	v = strtoull(w[k], nil, 16);
	return (v >> (b % 64)) & 1;
}

static u32
cell(uchar *p)
{
	return p[0]<<24 | p[1]<<16 | p[2]<<8 | p[3];
}

static void
findsize(int ev)
{
	char path[160];
	uchar b[32];	/* readfile keeps a byte for its NUL */
	int i;

	for(i = 0; i < nelem(sizes); i++){
		if(sizes[i].x != nil){
			snprint(path, sizeof path, "%s/sys/class/input/event%d/device/device/of_node/%s",
				Lkl, ev, sizes[i].x);
			if(readfile(path, (char*)b, sizeof b) < 4)
				continue;
			touch.w = cell(b);
			snprint(path, sizeof path, "%s/sys/class/input/event%d/device/device/of_node/%s",
				Lkl, ev, sizes[i].y);
			if(readfile(path, (char*)b, sizeof b) < 4)
				continue;
			touch.h = cell(b);
			return;
		}
		snprint(path, sizeof path, "%s/sys/class/input/event%d/device/device/of_node/%s",
			Lkl, ev, sizes[i].box != nil ? sizes[i].box : sizes[i].xy);
		if(sizes[i].box != nil && readfile(path, (char*)b, sizeof b) >= 16){
			touch.w = cell(b+8) - cell(b);
			touch.h = cell(b+12) - cell(b+4);
			return;
		}
		if(sizes[i].xy != nil && readfile(path, (char*)b, sizeof b) >= 8){
			touch.w = cell(b);
			touch.h = cell(b+4);
			return;
		}
	}
}

/* The guest event node of a multi-touch device, or -1. */
static int
finddev(void)
{
	char path[128], caps[256];
	int ev, n;

	for(ev = 0; ev < 32; ev++){
		snprint(path, sizeof path, "%s/sys/class/input/event%d/device/capabilities/abs", Lkl, ev);
		if(readfile(path, caps, sizeof caps) <= 0)
			continue;
		if(!capbit(caps, AbsMtX) || !capbit(caps, AbsMtY) || !capbit(caps, AbsMtSlot))
			continue;
		snprint(path, sizeof path, "%s/sys/class/input/event%d/device/name", Lkl, ev);
		n = readfile(path, touch.dev, sizeof touch.dev);
		if(n > 0 && touch.dev[n-1] == '\n')
			touch.dev[n-1] = 0;
		snprint(touch.src, sizeof touch.src, "lkl /dev/input/event%d", ev);
		findsize(ev);
		return ev;
	}
	return -1;
}

static void
mountlkl(void)
{
	int fd, t;

	for(t = 0; access("/srv/lkl", AEXIST) < 0; t++){
		if(t >= Srvwait)
			drvfail("touchfs: no /srv/lkl");
		sleep(100);
	}
	fd = create(Lkl, OREAD, DMDIR|0755);
	if(fd >= 0)
		close(fd);
	fd = open("/srv/lkl", ORDWR);
	if(fd < 0 || mount(fd, -1, Lkl, MREPL, "") < 0)
		drvfail("touchfs: mount /srv/lkl: %r");
}

/* Answer as many waiting reads as there are queued frames.  touch.lk held. */
static void
serve(Reader *rd)
{
	Req *r;
	char *s;
	int n;

	while(rd->nwait > 0 && rd->n > 0){
		r = rd->wait[0];
		rd->nwait--;
		memmove(rd->wait, rd->wait+1, rd->nwait*sizeof rd->wait[0]);
		s = rd->q[rd->head];
		rd->head = (rd->head + 1) % Nqueue;
		rd->n--;
		n = strlen(s);
		if(n > r->ifcall.count)
			n = r->ifcall.count;
		memmove(r->ofcall.data, s, n);
		r->ofcall.count = n;
		free(s);
		respond(r, nil);
	}
}

static void
publish(Slot *sl, int nsl)
{
	char line[Linesz], *e, *p;
	Reader *rd;
	int i, n;

	n = 0;
	for(i = 0; i < nsl; i++)
		if(sl[i].id >= 0)
			n++;
	e = line + sizeof line;
	p = seprint(line, e, "%lld %d", nsec()/1000000, n);
	for(i = 0; i < nsl; i++)
		if(sl[i].id >= 0)
			p = seprint(p, e, " %d %d %d %d %d",
				sl[i].id, sl[i].x, sl[i].y, sl[i].p, sl[i].a);
	seprint(p, e, "\n");

	qlock(&touch.lk);
	touch.frames++;
	strecpy(touch.last, touch.last + sizeof touch.last, line);
	for(rd = touch.readers; rd != nil; rd = rd->next){
		if(rd->n == Nqueue){
			free(rd->q[rd->head]);
			rd->head = (rd->head + 1) % Nqueue;
			rd->n--;
			touch.dropped++;
		}
		rd->q[(rd->head + rd->n) % Nqueue] = estrdup9p(line);
		rd->n++;
		serve(rd);
	}
	qunlock(&touch.lk);
}

/* Decode evdev records into frames, for ever. */
static void
reader(void *arg)
{
	uchar buf[Evsize*64], *e;
	Slot sl[Nslot];
	int fd, cur, skip, i, type, code, val;
	long n;

	fd = (int)(uintptr)arg;
	for(i = 0; i < Nslot; i++)
		sl[i].id = -1;
	cur = 0;
	skip = 0;
	for(;;){
		n = read(fd, buf, sizeof buf);
		if(n <= 0){
			fprint(2, "touchfs: read %s: %r\n", touch.src);
			sleep(1000);
			continue;
		}
		for(e = buf; e + Evsize <= buf + n; e += Evsize){
			type = e[16] | e[17]<<8;
			code = e[18] | e[19]<<8;
			val = e[20] | e[21]<<8 | e[22]<<16 | e[23]<<24;
			if(type == EvSyn){
				if(code == SynDropped)
					skip = 1;	/* until the next report */
				else if(code == SynReport){
					if(!skip)
						publish(sl, Nslot);
					skip = 0;
				}
				continue;
			}
			if(type != EvAbs)
				continue;
			if(code == AbsMtSlot){
				cur = val >= 0 && val < Nslot ? val : 0;
				continue;
			}
			switch(code){
			case AbsMtId:	sl[cur].id = val; break;
			case AbsMtX:	sl[cur].x = val; break;
			case AbsMtY:	sl[cur].y = val; break;
			case AbsMtPressure:	sl[cur].p = val; break;
			case AbsMtMajor:	sl[cur].a = val; break;
			}
		}
	}
}

static void
fsopen(Req *r)
{
	Reader *rd;

	if(strcmp(r->fid->file->name, "events") == 0){
		rd = emalloc9p(sizeof *rd);
		memset(rd, 0, sizeof *rd);
		qlock(&touch.lk);
		rd->next = touch.readers;
		touch.readers = rd;
		qunlock(&touch.lk);
		r->fid->aux = rd;
	}
	respond(r, nil);
}

static void
fsdestroyfid(Fid *f)
{
	Reader *rd, **l;

	rd = f->aux;
	if(rd == nil)
		return;
	qlock(&touch.lk);
	for(l = &touch.readers; *l != nil; l = &(*l)->next)
		if(*l == rd){
			*l = rd->next;
			break;
		}
	qunlock(&touch.lk);
	while(rd->n > 0){
		free(rd->q[rd->head]);
		rd->head = (rd->head + 1) % Nqueue;
		rd->n--;
	}
	free(rd);
}

static void
fsflush(Req *r)
{
	Reader *rd;
	Req *o;
	int i;

	o = r->oldreq;
	qlock(&touch.lk);
	for(rd = touch.readers; rd != nil; rd = rd->next)
		for(i = 0; i < rd->nwait; i++)
			if(rd->wait[i] == o){
				rd->nwait--;
				memmove(rd->wait+i, rd->wait+i+1,
					(rd->nwait-i)*sizeof rd->wait[0]);
				respond(o, "interrupted");
				goto out;
			}
out:
	qunlock(&touch.lk);
	respond(r, nil);
}

static void
fsread(Req *r, void *aux)
{
	Reader *rd;
	char buf[256 + Linesz];

	USED(aux);
	if(strcmp(drvfile(r)->name, "status") == 0){
		qlock(&touch.lk);
		snprint(buf, sizeof buf, "device %s\nsource %s\nsize %d %d\nframes %llud\ndropped %llud\nlast %s",
			touch.dev, touch.src, touch.w, touch.h, touch.frames, touch.dropped,
			touch.frames ? touch.last : "none\n");
		qunlock(&touch.lk);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	rd = r->fid->aux;
	qlock(&touch.lk);
	if(rd->nwait == nelem(rd->wait)){
		qunlock(&touch.lk);
		respond(r, "too many reads");
		return;
	}
	rd->wait[rd->nwait++] = r;
	serve(rd);
	qunlock(&touch.lk);
}

static DrvFile files[] = {
	{ "events",	0444,	&touch,	fsread,	nil },
	{ "status",	0444,	&touch,	fsread,	nil },
};

void
main(int argc, char **argv)
{
	DrvSrv *s;
	char path[64];
	int ev, fd, t;

	USED(argc);
	USED(argv);
	mountlkl();
	for(t = 0; (ev = finddev()) < 0; t++){
		if(t >= Devwait)
			drvfail("touchfs: no multi-touch input device");
		sleep(100);
	}
	snprint(path, sizeof path, "%s/dev/input/event%d", Lkl, ev);
	fd = open(path, OREAD);
	if(fd < 0)
		drvfail("touchfs: open %s: %r", path);

	s = drvsrv("touch", files, nelem(files));
	if(s == nil)
		drvfail("touchfs: cannot build 9P tree");
	s->srv.open = fsopen;
	s->srv.flush = fsflush;
	s->srv.destroyfid = fsdestroyfid;
	switch(rfork(RFPROC|RFMEM|RFNOWAIT)){
	case -1:
		drvfail("touchfs: rfork: %r");
	case 0:
		reader((void*)(uintptr)fd);
		_exits(nil);
	}
	drvsrvpost(s);
	drvhealthrun("touchfs");
}
