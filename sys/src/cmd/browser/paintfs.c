/*
 * paintfs.c - raster stage of the browser pipeline.
 *
 * Mounts a layoutfs (default /mnt/layout) and turns its box list into an
 * image, headless by default:
 *
 *	S ppm		P6 PPM of the current frame
 *	S snapshot	"PAINT <w> <h> 4\n" followed by raw RGBA
 *	S size		"<w> <h>"
 *	S ctl		write "size WxH"
 *	S boxes/text/tree/links	pass-through of layoutfs
 *	S src		pass-through read; write proxy to layoutfs src
 *	S gen		pass-through of the generation stream
 *
 * The first cut draws the layout rectangles into a plain RGBA buffer.  The
 * drawing calls are deliberately local so a libdraw/Memimage or font
 * renderer can replace them without touching the file protocol.
 */

#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "ringfs.h"

typedef struct Buf Buf;
struct Buf {
	char	*p;
	int	n;
	int	cap;
};

static char	*upstream = "/mnt/layout";
static QLock	paintlk;
static int	imgw = 640;
static int	imgh = 480;

static void
bufgrow(Buf *b, int need)
{
	int ncap;
	char *np;

	if(b->n + need <= b->cap)
		return;
	ncap = b->cap ? b->cap : 256;
	while(ncap < b->n + need)
		ncap *= 2;
	np = realloc(b->p, ncap);
	if(np == nil)
		sysfatal("paintfs: out of memory");
	b->p = np;
	b->cap = ncap;
}

static void
bufadd(Buf *b, void *p, int n)
{
	if(n <= 0)
		return;
	bufgrow(b, n);
	memmove(b->p + b->n, p, n);
	b->n += n;
}

static void
bufputc(Buf *b, int c)
{
	char ch = c;

	bufadd(b, &ch, 1);
}

static int
slurp(char *path, char **out)
{
	int fd, n, tot, cap;
	char *p, *np;

	fd = open(path, OREAD);
	if(fd < 0)
		return -1;
	cap = 8192;
	tot = 0;
	p = malloc(cap);
	if(p == nil){
		close(fd);
		return -1;
	}
	for(;;){
		if(tot == cap){
			cap *= 2;
			np = realloc(p, cap);
			if(np == nil){
				free(p);
				close(fd);
				return -1;
			}
			p = np;
		}
		n = read(fd, p + tot, cap - tot);
		if(n < 0){
			free(p);
			close(fd);
			return -1;
		}
		if(n == 0)
			break;
		tot += n;
	}
	close(fd);
	*out = p;
	return tot;
}

static void
fillrect(uchar *img, int w, int h, int x, int y, int rw, int rh,
	 int r, int g, int b)
{
	int i, j;

	for(j = y; j < y + rh; j++){
		if(j < 0 || j >= h)
			continue;
		for(i = x; i < x + rw; i++){
			if(i < 0 || i >= w)
				continue;
			img[(j*w + i)*4 + 0] = r;
			img[(j*w + i)*4 + 1] = g;
			img[(j*w + i)*4 + 2] = b;
			img[(j*w + i)*4 + 3] = 255;
		}
	}
}

static void
drawbox(uchar *img, int w, int h, int x, int y, int bw, int bh)
{
	if(bw <= 0 || bh <= 0)
		return;
	fillrect(img, w, h, x, y, bw, bh, 255, 255, 255);
	fillrect(img, w, h, x, y, bw, 1, 64, 96, 160);
	fillrect(img, w, h, x, y + bh - 1, bw, 1, 64, 96, 160);
	fillrect(img, w, h, x, y, 1, bh, 64, 96, 160);
	fillrect(img, w, h, x + bw - 1, y, 1, bh, 64, 96, 160);
}

static uchar*
render(int *wp, int *hp)
{
	char *boxes, *p, *e, *nl, *q;
	int n, w, h, maxy, x, y, bw, bh, i;
	uchar *img;

	n = slurp("/mnt/layout/boxes", &boxes);
	if(n < 0)
		return nil;
	qlock(&paintlk);
	w = imgw;
	h = imgh;
	qunlock(&paintlk);

	maxy = 0;
	p = boxes;
	e = boxes + n;
	while(p < e){
		nl = memchr(p, '\n', e - p);
		if(nl == nil)
			nl = e;
		q = p;
		strtol(q, &q, 10);		/* id */
		x = strtol(q, &q, 10);
		y = strtol(q, &q, 10);
		bw = strtol(q, &q, 10);
		bh = strtol(q, &q, 10);
		if(y + bh > maxy)
			maxy = y + bh;
		p = nl < e ? nl + 1 : e;
	}
	if(maxy + 8 > h)
		h = maxy + 8;

	img = malloc(w * h * 4);
	if(img == nil){
		free(boxes);
		return nil;
	}
	for(i = 0; i < w*h; i++){
		img[i*4 + 0] = 248;
		img[i*4 + 1] = 248;
		img[i*4 + 2] = 248;
		img[i*4 + 3] = 255;
	}
	p = boxes;
	e = boxes + n;
	while(p < e){
		nl = memchr(p, '\n', e - p);
		if(nl == nil)
			nl = e;
		q = p;
		strtol(q, &q, 10);		/* id */
		x = strtol(q, &q, 10);
		y = strtol(q, &q, 10);
		bw = strtol(q, &q, 10);
		bh = strtol(q, &q, 10);
		drawbox(img, w, h, x, y, bw, bh);
		p = nl < e ? nl + 1 : e;
	}
	free(boxes);
	*wp = w;
	*hp = h;
	return img;
}

static void
toppm(uchar *img, int w, int h, Buf *o)
{
	char hdr[64];
	int i;

	snprint(hdr, sizeof hdr, "P6\n%d %d\n255\n", w, h);
	bufadd(o, hdr, strlen(hdr));
	for(i = 0; i < w*h; i++){
		bufputc(o, img[i*4 + 0]);
		bufputc(o, img[i*4 + 1]);
		bufputc(o, img[i*4 + 2]);
	}
}

static void
tosnapshot(uchar *img, int w, int h, Buf *o)
{
	char hdr[64];

	snprint(hdr, sizeof hdr, "PAINT %d %d 4\n", w, h);
	bufadd(o, hdr, strlen(hdr));
	bufadd(o, img, w*h*4);
}

static void
paintread(Req *r)
{
	char *aux, *p;
	char path[128];
	int n, fd, w, h;
	uchar *img;
	Buf o;

	aux = r->fid->file->aux;
	memset(&o, 0, sizeof o);
	if(strcmp(aux, "gen") == 0){
		fd = open("/mnt/layout/gen", OREAD);
		if(fd < 0){
			respond(r, "upstream gen");
			return;
		}
		n = pread(fd, r->ofcall.data, r->ifcall.count, r->ifcall.offset);
		close(fd);
		if(n < 0){
			respond(r, "upstream gen");
			return;
		}
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}
	if(strcmp(aux, "size") == 0){
		qlock(&paintlk);
		snprint(path, sizeof path, "%d %d\n", imgw, imgh);
		qunlock(&paintlk);
		readbuf(r, path, strlen(path));
		respond(r, nil);
		return;
	}
	if(strcmp(aux, "ppm") == 0 || strcmp(aux, "snapshot") == 0){
		img = render(&w, &h);
		if(img == nil){
			respond(r, "layout unavailable");
			return;
		}
		if(strcmp(aux, "ppm") == 0)
			toppm(img, w, h, &o);
		else
			tosnapshot(img, w, h, &o);
		readbuf(r, o.p, o.n);
		free(o.p);
		free(img);
		respond(r, nil);
		return;
	}
	snprint(path, sizeof path, "%s/%s", upstream, aux);
	n = slurp(path, &p);
	if(n < 0){
		respond(r, "upstream unavailable");
		return;
	}
	readbuf(r, p, n);
	free(p);
	respond(r, nil);
}

static void
paintwrite(Req *r)
{
	char *aux, *p, *q;
	int off, n, fd, m;
	long w, h;

	aux = r->fid->file->aux;
	off = r->ifcall.offset;
	n = r->ifcall.count;
	if(strcmp(aux, "ctl") == 0){
		p = malloc(n + 1);
		if(p == nil){
			respond(r, "out of memory");
			return;
		}
		memmove(p, r->ifcall.data, n);
		p[n] = 0;
		if(strncmp(p, "size ", 5) == 0){
			w = strtol(p + 5, &q, 10);
			h = (*q == 'x' || *q == 'X') ? strtol(q + 1, nil, 10) : 0;
			if(w >= 16 && w <= 4096 && h >= 16 && h <= 4096){
				qlock(&paintlk);
				imgw = w;
				imgh = h;
				qunlock(&paintlk);
			}
		}
		free(p);
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}
	if(strcmp(aux, "src") != 0){
		respond(r, "read-only");
		return;
	}
	fd = open("/mnt/layout/src", OWRITE);
	if(fd < 0){
		respond(r, "upstream src");
		return;
	}
	m = pwrite(fd, r->ifcall.data, n, off);
	close(fd);
	if(m != n){
		respond(r, "short write");
		return;
	}
	r->ofcall.count = n;
	respond(r, nil);
}

void
paintfsserve(int fd)
{
	Srv s;
	File *f;
	static struct {
		char	*name;
		u32	mode;
	} files[] = {
		{ "src", 0666 }, { "ppm", 0444 }, { "snapshot", 0444 },
		{ "size", 0444 }, { "ctl", 0222 }, { "boxes", 0444 },
		{ "text", 0444 }, { "tree", 0444 }, { "links", 0444 },
		{ "gen", 0444 },
	};
	int i;

	memset(&s, 0, sizeof s);
	s.tree = alloctree("browser", "browser", DMDIR|0555, nil);
	if(s.tree == nil)
		sysfatal("paintfs: alloctree: %r");
	for(i = 0; i < nelem(files); i++){
		f = createfile(s.tree->root, files[i].name, "browser",
			       files[i].mode, files[i].name);
		if(f == nil)
			sysfatal("paintfs: create %s: %r", files[i].name);
		closefile(f);
	}
	s.read = paintread;
	s.write = paintwrite;
	s.infd = fd;
	s.outfd = fd;
	s.msize = 32768;
	s.threaded = 1;
	srv(&s);
}

static void
serveconn(void *v)
{
	int fd;

	fd = (int)(vlong)v;
	paintfsserve(fd);
	close(fd);
	threadexits(nil);
}

void
paintfsserver(char *name)
{
	int fd;

	for(;;){
		fd = ringlisten(name);
		if(fd < 0)
			break;
		if(proccreate(serveconn, (void*)(vlong)fd, 65536) < 0){
			close(fd);
			break;
		}
	}
}
