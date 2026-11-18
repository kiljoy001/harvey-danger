/*
 * jsfs.c - the script stage of the browser pipeline.
 *
 * The current Harvey V8 build is d8 (an executable), not an embeddable
 * library, so jsfs runs the script through d8 and serves the result.  The
 * file protocol is the one an embedded isolate will use later:
 *
 *	S eval		write a script; it is executed in a fresh d8
 *	S result	output of the last script
 *	S console	same as result
 *	S gen		generation counter
 *	S ctl		write "clear"
 *
 * d8 is expected at /js/d8 (staged by the test runner with --tree).
 */

#include <u.h>
#include <libc.h>
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

static QLock	jslk;
static Buf	result;
static ulong	gen;

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
		sysfatal("jsfs: out of memory");
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

static int
slurpfile(char *path, char **out)
{
	int fd, n, tot, cap;
	char *buf, *np;

	fd = open(path, OREAD);
	if(fd < 0)
		return -1;
	cap = 8192;
	tot = 0;
	buf = malloc(cap);
	if(buf == nil){
		close(fd);
		return -1;
	}
	for(;;){
		if(tot == cap){
			cap *= 2;
			np = realloc(buf, cap);
			if(np == nil){
				free(buf);
				close(fd);
				return -1;
			}
			buf = np;
		}
		n = read(fd, buf + tot, cap - tot);
		if(n < 0){
			free(buf);
			close(fd);
			return -1;
		}
		if(n == 0)
			break;
		tot += n;
	}
	close(fd);
	buf[tot] = 0;
	*out = buf;
	return tot;
}

int
jsrun(char *code, char **out)
{
	int fd, n;
	Waitmsg *w;

	remove("/jsout");
	fd = create("/jsout", OWRITE|OTRUNC, 0666);
	if(fd < 0)
		return -1;
	switch(rfork(RFPROC|RFFDG)){
	case -1:
		close(fd);
		return -1;
	case 0:
		dup(fd, 1);
		dup(fd, 2);
		close(fd);
		execl("/js/d8", "d8", "-e", code, nil);
		exits("exec d8");
	}
	close(fd);
	while((w = wait()) != nil)
		free(w);
	n = slurpfile("/jsout", out);
	return n;
}
static void
jsread(Req *r)
{
	char *aux, buf[32];

	aux = r->fid->file->aux;
	if(strcmp(aux, "gen") == 0){
		qlock(&jslk);
		snprint(buf, sizeof buf, "%lud\n", gen);
		qunlock(&jslk);
		readbuf(r, buf, strlen(buf));
	} else if(strcmp(aux, "result") == 0 || strcmp(aux, "console") == 0){
		qlock(&jslk);
		readbuf(r, result.p, result.n);
		qunlock(&jslk);
	} else {
		respond(r, "bad file");
		return;
	}
	respond(r, nil);
}

static void
jswrite(Req *r)
{
	char *aux, *code, *out;
	int n;

	aux = r->fid->file->aux;
	if(strcmp(aux, "ctl") == 0){
		qlock(&jslk);
		result.n = 0;
		qunlock(&jslk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	if(strcmp(aux, "eval") != 0){
		respond(r, "read-only");
		return;
	}
	n = r->ifcall.count;
	code = malloc(n + 1);
	if(code == nil){
		respond(r, "out of memory");
		return;
	}
	memmove(code, r->ifcall.data, n);
	code[n] = 0;
	print("jsfs: eval start\n");
	out = nil;
	n = jsrun(code, &out);
	print("jsfs: eval done %d\n", n);
	qlock(&jslk);
	result.n = 0;
	if(n < 0)
		bufputs(&result, "jsfs: d8 exec failed\n");
	else
		bufadd(&result, out, n);
	gen++;
	qunlock(&jslk);
	free(code);
	free(out);
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

void
jsfsserve(int fd)
{
	Srv s;
	File *f;
	static struct {
		char	*name;
		u32	mode;
	} files[] = {
		{ "eval", 0666 }, { "result", 0444 }, { "console", 0444 },
		{ "gen", 0444 }, { "ctl", 0222 },
	};
	int i;

	memset(&s, 0, sizeof s);
	s.tree = alloctree("browser", "browser", DMDIR|0555, nil);
	if(s.tree == nil)
		sysfatal("jsfs: alloctree: %r");
	for(i = 0; i < nelem(files); i++){
		f = createfile(s.tree->root, files[i].name, "browser",
			       files[i].mode, files[i].name);
		if(f == nil)
			sysfatal("jsfs: create %s: %r", files[i].name);
		closefile(f);
	}
	s.read = jsread;
	s.write = jswrite;
	s.infd = fd;
	s.outfd = fd;
	s.msize = 32768;
	s.threaded = 0;		/* runjs forks; keep it in this process */
	srv(&s);
}

void
jsfsserver(char *name)
{
	int fd;

	/* one client: serving in this process keeps runjs's fork/exec sane */
	fd = ringlisten(name);
	if(fd >= 0)
		jsfsserve(fd);
}
