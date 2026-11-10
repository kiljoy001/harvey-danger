#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <fcall.h>
#include "dat.h"
#include "fns.h"

void
cvttorunes(char *p, int n, Rune *r, int *nb, int *nr, int *nulls)
{
	uchar *q;
	Rune *s;
	int j, w;

	/*
	 * Always guaranteed that n bytes may be interpreted
	 * without worrying about partial runes.  This may mean
	 * reading up to UTFmax-1 more bytes than n; the caller
	 * knows this.  If n is a firm limit, the caller should
	 * set p[n] = 0.
	 */
	q = (uchar*)p;
	s = r;
	for(j=0; j<n; j+=w){
		if(*q < Runeself){
			w = 1;
			*s = *q++;
		}else{
			w = chartorune(s, (char*)q);
			q += w;
		}
		if(*s)
			s++;
		else if(nulls)
				*nulls = TRUE;
	}
	*nb = (char*)q-p;
	*nr = s-r;
}

void
error(char *s)
{
	fprint(2, "rio: %s: %r\n", s);
	if(errorshouldabort)
		abort();
	threadexitsall("error");
}

void*
erealloc(void *p, uint n)
{
	p = realloc(p, n);
	if(p == nil)
		error("realloc failed");
	setrealloctag(p, getcallerpc());
	return p;
}

void*
emalloc(uint n)
{
	void *p;

	p = malloc(n);
	if(p == nil)
		error("malloc failed");
	setmalloctag(p, getcallerpc());
	memset(p, 0, n);
	return p;
}

char*
estrdup(char *s)
{
	char *p;

	p = malloc(strlen(s)+1);
	if(p == nil)
		error("strdup failed");
	setmalloctag(p, getcallerpc());
	strcpy(p, s);
	return p;
}

int
isalnum(Rune c)
{
	/*
	 * Hard to get absolutely right.  Use what we know about ASCII
	 * and assume anything above the Latin control characters is
	 * potentially an alphanumeric.
	 */
	if(c <= ' ')
		return FALSE;
	if(0x7F<=c && c<=0xA0)
		return FALSE;
	if(utfrune("!\"#$%&'()*+,-./:;<=>?@[\\]^`{|}~", c))
		return FALSE;
	return TRUE;
}

int
isspace(Rune c)
{
	return c == 0 || c == ' ' || c == '\t' ||
		c == '\n' || c == '\r' || c == '\v';
}

Rune*
strrune(Rune *s, Rune c)
{
	Rune c1;

	if(c == 0) {
		while(*s++)
			;
		return s-1;
	}

	while(c1 = *s++)
		if(c1 == c)
			return s-1;
	return nil;
}

/* int */
/* min(int a, int b) */
/* { */
/* 	if(a < b) */
/* 		return a; */
/* 	return b; */
/* } */

/* int */
/* max(int a, int b) */
/* { */
/* 	if(a > b) */
/* 		return a; */
/* 	return b; */
/* } */

char*
runetobyte(Rune *r, int n, int *ip)
{
	char *s;
	int m;

	s = emalloc(n*UTFmax+1);
	m = snprint(s, n*UTFmax+1, "%.*S", n, r);
	*ip = m;
	return s;
}

/* File servers may block. Keep their I/O out of Rio's drawing process. */
typedef struct Fileload Fileload;
struct Fileload {
	Ref ref;
	char *path;
	Channel *done;
	Dir *d;
	Rune *r;
	int nr;
	char err[ERRMAX];
};

static Ref fileloads;

static void
fileloadfree(Fileload *f)
{
	if(decref(&f->ref) != 0)
		return;
	free(f->path);
	free(f->d);
	free(f->r);
	chanfree(f->done);
	free(f);
}

static void
fileloadproc(void *v)
{
	enum { Limit = 4*1024*1024, Chunk = 32768 };
	Fileload *f;
	char *buf;
	int fd, n, used, cap, k;
	vlong deadline, left;
	Rune rune;

	f = v;
	fd = -1;
	buf = nil;
	/* Rio's inherited alarm handler interrupts blocked filesystem calls. */
	deadline = nsec() + 5000000000LL;
	alarm(5000);
	f->d = dirstat(f->path);
	if(f->d == nil)
		goto Ioerror;
	if(f->d->mode & DMDIR)
		goto Done;
	if(f->d->length > Limit){
		strcpy(f->err, "file exceeds the 4 MiB editor limit");
		goto Done;
	}
	left = (deadline-nsec()+999999)/1000000;
	if(left <= 0){
		strcpy(f->err, "file read timed out after 5 seconds");
		goto Done;
	}
	alarm(left);
	fd = open(f->path, OREAD);
	if(fd < 0)
		goto Ioerror;
	used = cap = 0;
	for(;;){
		left = (deadline-nsec()+999999)/1000000;
		if(left <= 0){
			strcpy(f->err, "file read timed out after 5 seconds");
			goto Done;
		}
		alarm(left);
		if(used == cap){
			cap = min(cap == 0 ? Chunk : 2*cap, Limit+1);
			buf = erealloc(buf, cap);
		}
		n = read(fd, buf+used, cap-used);
		if(n < 0)
			goto Ioerror;
		if(n == 0)
			break;
		used += n;
		if(used > Limit){
			strcpy(f->err, "file exceeds the 4 MiB editor limit");
			goto Done;
		}
	}
	alarm(0);
	f->r = runemalloc(used+1);
	for(n=0; n<used; n+=k){
		if(!fullrune(buf+n, used-n))
			goto Binary;
		k = chartorune(&rune, buf+n);
		if((rune == Runeerror && k == 1) || rune == 0
		|| (rune < ' ' && rune != '\t' && rune != '\n'
			&& rune != '\r' && rune != '\f' && rune != '\b'))
			goto Binary;
		f->r[f->nr++] = rune;
	}
	goto Done;
Binary:
	strcpy(f->err, "binary or non-UTF-8 file; opening a read-only preview");
	free(f->r);
	f->r = nil;
	f->nr = 0;
	goto Done;
Ioerror:
	if(nsec() >= deadline)
		strcpy(f->err, "file read timed out after 5 seconds");
	else
		rerrstr(f->err, sizeof f->err);
Done:
	if(fd >= 0)
		close(fd);
	alarm(0);
	free(buf);
	sendul(f->done, 0);
	decref(&fileloads);
	fileloadfree(f);
	threadexits(nil);
}

int
wreadfile(char *path, Dir **dp, Rune **rp, int *nrp, char *err)
{
	Fileload *f;
	Timer *timer;
	Alt alts[3];
	int result;

	/* Some transports cannot cancel a blocked read. Bound outstanding work. */
	if(ainc(&fileloads.ref) > 8){
		decref(&fileloads);
		strcpy(err, "too many pending file reads; wait for the file server");
		return -1;
	}
	f = emalloc(sizeof *f);
	f->ref.ref = 2; /* caller and reader own independent lifetimes */
	f->path = estrdup(path);
	f->done = chancreate(sizeof(u32), 1);
	procrfork(fileloadproc, f, 16384, RFFDG);
	timer = timerstart(5000);
	memset(alts, 0, sizeof alts);
	alts[0].c = f->done;
	alts[0].op = CHANRCV;
	alts[1].c = timer->c;
	alts[1].op = CHANRCV;
	alts[2].op = CHANEND;
	result = alt(alts);
	if(result == 1){
		timerstop(timer);
		strcpy(err, "file read timed out after 5 seconds");
		fileloadfree(f); /* a late reader reclaims its own data */
		return -1;
	}
	timercancel(timer);
	if(f->err[0]){
		utfecpy(err, err+ERRMAX, f->err);
		fileloadfree(f);
		return -1;
	}
	*dp = f->d;
	*rp = f->r;
	*nrp = f->nr;
	f->d = nil;
	f->r = nil;
	fileloadfree(f);
	return 0;
}

typedef struct Openpath Openpath;
struct Openpath {
	char *path;
	char *reason;
};

static void
rioopenproc(void *v)
{
	Openpath *o;
	char path[8192], reason[ERRMAX], *argv[5];

	o = v;
	utfecpy(path, path+sizeof path, o->path);
	utfecpy(reason, reason+sizeof reason, o->reason);
	free(o->path);
	free(o->reason);
	free(o);
	argv[0] = "rio-open";
	if(reason[0]){
		argv[1] = "-e";
		argv[2] = path;
		argv[3] = reason;
		argv[4] = nil;
	}else{
		argv[1] = path;
		argv[2] = nil;
	}
	procexec(nil, "/bin/rio-open", argv);
	threadexits("rio-open exec failed");
}

void
rioopenpath(char *path, char *reason)
{
	Openpath *o;

	o = emalloc(sizeof *o);
	o->path = estrdup(path);
	o->reason = estrdup(reason == nil ? "" : reason);
	procrfork(rioopenproc, o, 16384, RFFDG);
}
