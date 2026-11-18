/*
 * textfs.c - terminal stage of the browser pipeline.
 *
 * Mounts a layoutfs (default /mnt/layout) and re-exports its results as
 * the text view:
 *
 *	S text		laid-out text
 *	S boxes		pass-through box list (id x y w h text)
 *	S tree		pass-through node table
 *	S links		pass-through links
 *	S src		pass-through read; write proxy to layoutfs src
 *	S gen		pass-through generation stream
 *
 * The terminal stage is deliberately thin: swapping it for a paintfs that
 * reads the same box list is the whole point of the split.
 */

#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "ringfs.h"

static char *upstream = "/mnt/layout";

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
textread(Req *r)
{
	char *aux, *p;
	char path[128];
	int n, fd;

	aux = r->fid->file->aux;
	if(strcmp(aux, "gen") == 0){
		/* stream: preserve the caller's offset and blocking behaviour */
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
textwrite(Req *r)
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
textfsserve(int fd)
{
	Srv s;
	File *f;
	static struct {
		char	*name;
		u32	mode;
	} files[] = {
		{ "src", 0666 }, { "text", 0444 }, { "boxes", 0444 },
		{ "tree", 0444 }, { "links", 0444 }, { "gen", 0444 },
		{ "rules", 0444 }, { "styles", 0444 },
	};
	int i;

	memset(&s, 0, sizeof s);
	s.tree = alloctree("browser", "browser", DMDIR|0555, nil);
	if(s.tree == nil)
		sysfatal("textfs: alloctree: %r");
	for(i = 0; i < nelem(files); i++){
		f = createfile(s.tree->root, files[i].name, "browser",
			       files[i].mode, files[i].name);
		if(f == nil)
			sysfatal("textfs: create %s: %r", files[i].name);
		closefile(f);
	}
	s.read = textread;
	s.write = textwrite;
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
	textfsserve(fd);
	close(fd);
	threadexits(nil);
}

void
textfsserver(char *name)
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
