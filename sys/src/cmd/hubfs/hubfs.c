/* hubfs: a 9P file server whose files are broadcast byte-stream "hubs".
 * Writers append to a ring buffer; every reader keeps its own position and
 * replays the stream from where it attached.  A read with no data queues
 * until the next write.  The default (and only) mode is broadcast: writers
 * never block and slow readers lag, missing data that has wrapped out of the
 * ring.  Ported from 9front's hubfs (fgb/ANTS), trimmed to the core hub
 * semantics and minus the network listener and paranoid/freeze/trunc modes.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

enum {
	Bucksize = 128 * 1024,	/* ring buffer bytes per hub */
	Maxq = 64,		/* queued reads per hub */
	Maxhub = 16,		/* maximum number of hubs */
	Namelen = 64,
};

typedef struct Hub Hub;
typedef struct Msgq Msgq;

struct Hub {
	char name[Namelen];
	char *buf;		/* Bucksize ring buffer */
	u64 total;		/* bytes ever written to the stream */
	int len;		/* valid bytes currently buffered (<= Bucksize) */
	Req *rreqs[Maxq];	/* reads waiting for more data */
	int nrreq;
};

struct Msgq {
	u64 pos;		/* this reader's position in the stream */
};

static Hub hubs[Maxhub];
static int nhub;

static Hub *
hubof(File *f)
{
	return f->aux;
}

/* Copy n bytes from stream position p into dst. */
static void
copyfrom(Hub *h, u64 p, void *dst, int n)
{
	u64 base = h->total - h->len;
	u64 idx = (p - base) % Bucksize;
	int first = Bucksize - (int)idx;
	char *d = dst;

	if(n > first){
		memmove(d, h->buf + idx, first);
		memmove(d + first, h->buf, n - first);
	}else
		memmove(d, h->buf + idx, n);
}

/* Copy n bytes from src into the stream at position p. */
static void
copyto(Hub *h, u64 p, void *src, int n)
{
	u64 idx = p % Bucksize;
	int first = Bucksize - (int)idx;
	char *s = src;

	if(n > first){
		memmove(h->buf + idx, s, first);
		memmove(h->buf, s + first, n - first);
	}else
		memmove(h->buf + idx, s, n);
}

/* Answer queued reads that now have data. */
static void
msgsend(Hub *h)
{
	Req *r;
	Msgq *mq;
	u64 count;
	int i, j;

	for(i = 0; i < h->nrreq; i++){
		r = h->rreqs[i];
		if(r == nil)
			continue;
		mq = r->fid->aux;
		if(mq->pos >= h->total)
			continue;
		if(mq->pos < h->total - h->len)
			mq->pos = h->total - h->len;
		count = r->ifcall.count;
		if(count > h->total - mq->pos)
			count = h->total - mq->pos;
		copyfrom(h, mq->pos, r->ofcall.data, count);
		mq->pos += count;
		r->ofcall.count = count;
		h->rreqs[i] = nil;
		respond(r, nil);
	}
	j = 0;
	for(i = 0; i < h->nrreq; i++)
		if(h->rreqs[i] != nil)
			h->rreqs[j++] = h->rreqs[i];
	h->nrreq = j;
}

static void
fsread(Req *r)
{
	Hub *h;
	Msgq *mq;
	u64 count;

	h = hubof(r->fid->file);
	if(h == nil){
		respond(r, "not a hub");
		return;
	}
	mq = r->fid->aux;
	if(mq->pos >= h->total){
		if(h->nrreq < Maxq)
			h->rreqs[h->nrreq++] = r;
		else
			respond(r, "read queue full");
		return;
	}
	if(mq->pos < h->total - h->len)
		mq->pos = h->total - h->len;
	count = r->ifcall.count;
	if(count > h->total - mq->pos)
		count = h->total - mq->pos;
	copyfrom(h, mq->pos, r->ofcall.data, count);
	mq->pos += count;
	r->ofcall.count = count;
	respond(r, nil);
}

static void
fswrite(Req *r)
{
	Hub *h;
	int n;

	h = hubof(r->fid->file);
	if(h == nil){
		respond(r, "not a hub");
		return;
	}
	n = r->ifcall.count;
	if(n > Bucksize)
		n = Bucksize;
	copyto(h, h->total, r->ifcall.data, n);
	h->total += n;
	h->len += n;
	if(h->len > Bucksize)
		h->len = Bucksize;
	msgsend(h);
	r->ofcall.count = n;
	respond(r, nil);
}

static void
fsopen(Req *r)
{
	Hub *h;
	Msgq *mq;

	h = hubof(r->fid->file);
	if(h == nil){
		respond(r, nil);
		return;
	}
	mq = emalloc9p(sizeof *mq);
	mq->pos = h->total - h->len;	/* start at the oldest buffered byte */
	r->fid->aux = mq;
	respond(r, nil);
}

static void
fscreate(Req *r)
{
	Hub *h;
	File *f;

	if(nhub >= Maxhub){
		respond(r, "too many hubs");
		return;
	}
	f = createfile(r->fid->file, r->ifcall.name, r->fid->uid,
		r->ifcall.perm, nil);
	if(f == nil){
		respond(r, "create failed");
		return;
	}
	h = &hubs[nhub++];
	memset(h, 0, sizeof *h);
	strncpy(h->name, r->ifcall.name, Namelen - 1);
	h->buf = emalloc9p(Bucksize);
	f->aux = h;
	r->fid->file = f;
	r->ofcall.qid = f->qid;
	respond(r, nil);
}

static void
fsdestroyfile(File *f)
{
	Hub *h;

	h = f->aux;
	if(h != nil && h->buf != nil)
		free(h->buf);
}

Srv fs = {
	.open = fsopen,
	.read = fsread,
	.write = fswrite,
	.create = fscreate,
};

void
main(int argc, char **argv)
{
	USED(argc);
	USED(argv);
	fs.tree = alloctree(nil, nil, DMDIR | 0777, fsdestroyfile);
	postmountsrv(&fs, "hubfs", nil, MREPL | MCREATE);
	exits(nil);
}
