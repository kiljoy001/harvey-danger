/*
 * cssfs.c - the style stage of the browser pipeline.
 *
 * Mounts an htmlfs (default /mnt/html) and exposes the style inputs:
 *
 *	S rules		text of the <style> blocks in the document
 *	S styles	default per-tag style table
 *	S tree		pass-through of htmlfs tree
 *	S links		pass-through of htmlfs links
 *	S src		pass-through read; write proxy to htmlfs src
 *	S gen		pass-through generation stream
 *
 * The default table is what layoutfs will read first; a real cascade can
 * replace `styles' without changing the file protocol.
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

static char *upstream = "/mnt/html";

static char *deftable =
	"h1 size=2 weight=bold\n"
	"h2 size=1.5 weight=bold\n"
	"h3 size=1.25 weight=bold\n"
	"h4 size=1.1 weight=bold\n"
	"p margin=1\n"
	"li margin=1\n"
	"b weight=bold\n"
	"strong weight=bold\n"
	"a color=blue underline=1\n"
	"pre family=mono\n"
	"code family=mono\n";

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
		sysfatal("cssfs: out of memory");
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

static char*
findci(char *s, int n, char *pat)
{
	int plen, i;
	char *e;

	plen = strlen(pat);
	e = s + n;
	for(; s + plen <= e; s++){
		for(i = 0; i < plen; i++)
			if(tolower((uchar)s[i]) != tolower((uchar)pat[i]))
				break;
		if(i == plen)
			return s + plen;
	}
	return nil;
}

static void
extract(char *s, int n, Buf *out)
{
	char *p, *q, *e;
	int i;

	i = 0;
	while(i < n){
		p = findci(s + i, n - i, "<style");
		if(p == nil)
			break;
		q = memchr(p, '>', (s + n) - p);
		if(q == nil)
			break;
		q++;
		e = findci(q, (s + n) - q, "</style");
		if(e == nil)
			break;
		bufadd(out, q, (e - 7) - q);	/* drop "</style" */
		bufputc(out, '\n');
		i = e - s;
	}
}

static void
cssread(Req *r)
{
	char *aux, *src, *p;
	int n, fd;
	Buf b;

	aux = r->fid->file->aux;
	memset(&b, 0, sizeof b);
	if(strcmp(aux, "gen") == 0){
		fd = open("/mnt/html/gen", OREAD);
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
	if(strcmp(aux, "styles") == 0){
		readbuf(r, deftable, strlen(deftable));
		respond(r, nil);
		return;
	}
	if(strcmp(aux, "rules") == 0){
		n = slurp("/mnt/html/src", &src);
		if(n < 0){
			respond(r, "upstream src");
			return;
		}
		extract(src, n, &b);
		free(src);
		readbuf(r, b.p, b.n);
		free(b.p);
		respond(r, nil);
		return;
	}
	if(strcmp(aux, "tree") == 0 || strcmp(aux, "links") == 0
	   || strcmp(aux, "src") == 0){
		char path[128];

		snprint(path, sizeof path, "%s/%s", upstream, aux);
		n = slurp(path, &p);
		if(n < 0){
			respond(r, "upstream unavailable");
			return;
		}
		readbuf(r, p, n);
		free(p);
		respond(r, nil);
		return;
	}
	respond(r, "bad file");
}

static void
csswrite(Req *r)
{
	char *aux;
	int off, n, fd, m;

	aux = r->fid->file->aux;
	if(strcmp(aux, "src") != 0){
		respond(r, "read-only");
		return;
	}
	off = r->ifcall.offset;
	n = r->ifcall.count;
	fd = open("/mnt/html/src", OWRITE);
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
cssfsserve(int fd)
{
	Srv s;
	File *f;
	static struct {
		char	*name;
		u32	mode;
	} files[] = {
		{ "src", 0666 }, { "rules", 0444 }, { "styles", 0444 },
		{ "tree", 0444 }, { "links", 0444 }, { "gen", 0444 },
		{ "ctl", 0222 },
	};
	int i;

	memset(&s, 0, sizeof s);
	s.tree = alloctree("browser", "browser", DMDIR|0555, nil);
	if(s.tree == nil)
		sysfatal("cssfs: alloctree: %r");
	for(i = 0; i < nelem(files); i++){
		f = createfile(s.tree->root, files[i].name, "browser",
			       files[i].mode, files[i].name);
		if(f == nil)
			sysfatal("cssfs: create %s: %r", files[i].name);
		closefile(f);
	}
	s.read = cssread;
	s.write = csswrite;
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
	cssfsserve(fd);
	close(fd);
	threadexits(nil);
}

void
cssfsserver(char *name)
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
