/*
 * layoutfs.c - stage 2 of the browser pipeline.
 *
 * Mounts an htmlfs (default /mnt/html), turns its node table into a simple
 * block layout, and serves both the laid-out text and a flat box list:
 *
 *	S text		line-wrapped text
 *	S boxes		id x y w h <text>
 *	S width		current wrap width (columns)
 *	S ctl		write "width N"
 *	S tree		pass-through of htmlfs tree
 *	S links		pass-through of htmlfs links
 *	S src		pass-through read; write proxy to htmlfs src
 *	S gen		pass-through of htmlfs generation stream
 *
 * This is intentionally a text-metric layout (8x16 cells) so the terminal
 * stage can consume it today; a real box/font stage replaces `emit' without
 * changing the upstream or downstream protocols.
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

enum {
	CellW = 8,
	CellH = 16,
};

static char	*upstream = "/mnt/css";
static QLock	layoutlk;
static int	width = 80;

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
		sysfatal("layoutfs: out of memory");
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
bufputs(Buf *b, char *s)
{
	bufadd(b, s, strlen(s));
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
emit(Buf *text, Buf *boxes, int id, int y, char *s, int len)
{
	char hdr[64];

	snprint(hdr, sizeof hdr, "%d %d %d %d %d ", id, 0, y, len * CellW,
		CellH);
	bufadd(boxes, hdr, strlen(hdr));
	bufadd(boxes, s, len);
	bufputc(boxes, '\n');
	bufadd(text, s, len);
	bufputc(text, '\n');
}

static void
layout(char *tree, int n, Buf *text, Buf *boxes)
{
	Buf raw;
	char *p, *e, *nl, *s, *b;
	int len, cols, id, y, cut, i;

	memset(&raw, 0, sizeof raw);
	p = tree;
	e = tree + n;
	while(p < e){
		nl = memchr(p, '\n', e - p);
		len = nl != nil ? nl - p : e - p;
		if(len > 0 && p[0] == 'B')
			bufputc(&raw, '\n');
		else if(len > 0 && p[0] == 'T')
			bufadd(&raw, p + 1, len - 1);
		p = nl != nil ? nl + 1 : e;
	}

	qlock(&layoutlk);
	cols = width;
	qunlock(&layoutlk);
	if(cols < 8)
		cols = 8;

	id = 0;
	y = 0;
	s = raw.p;
	e = raw.p + raw.n;
	while(s < e){
		nl = memchr(s, '\n', e - s);
		len = nl != nil ? nl - s : e - s;
		while(len > cols){
			cut = cols;
			for(i = cols; i > 0 && s[i] != ' '; i--)
				;
			if(i > 0)
				cut = i;
			emit(text, boxes, id++, y, s, cut);
			y += CellH;
			s += cut;
			len -= cut;
			while(len > 0 && *s == ' '){
				s++;
				len--;
			}
		}
		emit(text, boxes, id++, y, s, len);
		y += CellH;
		s = nl != nil ? nl + 1 : e;
	}
	free(raw.p);
}

static void
layoutread(Req *r)
{
	char *aux, *p, buf[32];
	char path[128];
	int n, fd;
	Buf text, boxes;

	aux = r->fid->file->aux;
	memset(&text, 0, sizeof text);
	memset(&boxes, 0, sizeof boxes);
	if(strcmp(aux, "gen") == 0){
		snprint(path, sizeof path, "%s/gen", upstream);
		fd = open(path, OREAD);
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
	if(strcmp(aux, "width") == 0){
		qlock(&layoutlk);
		snprint(buf, sizeof buf, "%d\n", width);
		qunlock(&layoutlk);
		readbuf(r, buf, strlen(buf));
		respond(r, nil);
		return;
	}
	if(strcmp(aux, "text") == 0 || strcmp(aux, "boxes") == 0){
		snprint(path, sizeof path, "%s/tree", upstream);
		n = slurp(path, &p);
		if(n < 0){
			respond(r, "upstream tree");
			return;
		}
		layout(p, n, &text, &boxes);
		free(p);
		if(strcmp(aux, "text") == 0)
			readbuf(r, text.p, text.n);
		else
			readbuf(r, boxes.p, boxes.n);
		free(text.p);
		free(boxes.p);
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
layoutwrite(Req *r)
{
	char *aux, *p;
	char path[128];
	int off, n, fd, m;
	long w;

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
		if(strncmp(p, "width ", 6) == 0){
			w = strtol(p + 6, nil, 10);
			if(w >= 8 && w <= 4096){
				qlock(&layoutlk);
				width = w;
				qunlock(&layoutlk);
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
	snprint(path, sizeof path, "%s/src", upstream);
	fd = open(path, OWRITE);
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
layoutfsserve(int fd)
{
	Srv s;
	File *f;
	static struct {
		char	*name;
		u32	mode;
	} files[] = {
		{ "src", 0666 }, { "text", 0444 }, { "boxes", 0444 },
		{ "width", 0444 }, { "ctl", 0222 }, { "tree", 0444 },
		{ "links", 0444 }, { "gen", 0444 }, { "rules", 0444 },
		{ "styles", 0444 },
	};
	int i;

	memset(&s, 0, sizeof s);
	s.tree = alloctree("browser", "browser", DMDIR|0555, nil);
	if(s.tree == nil)
		sysfatal("layoutfs: alloctree: %r");
	for(i = 0; i < nelem(files); i++){
		f = createfile(s.tree->root, files[i].name, "browser",
			       files[i].mode, files[i].name);
		if(f == nil)
			sysfatal("layoutfs: create %s: %r", files[i].name);
		closefile(f);
	}
	s.read = layoutread;
	s.write = layoutwrite;
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
	layoutfsserve(fd);
	close(fd);
	threadexits(nil);
}

void
layoutfsserver(char *name)
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
