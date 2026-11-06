/* srv9p - see srv9p.h. Written from the 9P2000 spec and the
 * frozen esp9 namespace; shares no code with mock9p (Go), which
 * it was tested against via a common independent client. */
#include "srv9p.h"

#include <string.h>

enum {
	Tversion = 100, Rversion = 101,
	Tauth = 102, Rauth = 103,
	Tattach = 104, Rattach = 105,
	Rerror = 107,
	Tflush = 108, Rflush = 109,
	Twalk = 110, Rwalk = 111,
	Topen = 112, Ropen = 113,
	Tread = 116, Rread = 117,
	Twrite = 118, Rwrite = 119,
	Tclunk = 120, Rclunk = 121,
	/* Canonical 9P2000 (Harvey sys/include/fcall.h):
	 * Tstat is 124, not 114 - 114 is Tcreate. This was
	 * wrong until B30: our own bench client never sends
	 * Tstat, so the dialect pair agreed, and the first
	 * real client (plan9port 9p, "dirstat") exposed it.
	 * Tcreate/Tremove/Twstat fall through to the
	 * not-implemented Rerror, as intended. */
	Tstat = 124, Rstat = 125,
};

/* ---- little-endian cursor ---- */
typedef struct {
	const uint8_t *b;
	uint32_t n, i;
	int bad;
} Rd;

static uint32_t rd16(Rd *r)
{
	uint32_t v;
	if (r->i + 2 > r->n) { r->bad = 1; return 0; }
	v = (uint32_t)r->b[r->i] | ((uint32_t)r->b[r->i + 1] << 8);
	r->i += 2;
	return v;
}

static uint32_t rd32(Rd *r)
{
	uint32_t lo = rd16(r), hi = rd16(r);
	return lo | (hi << 16);
}

static uint64_t rd64(Rd *r)
{
	uint64_t lo = rd32(r), hi = rd32(r);
	return lo | (hi << 32);
}

static const char *rdstr(Rd *r, uint32_t *len)
{
	uint32_t n = rd16(r);
	const char *s;
	if (r->i + n > r->n) { r->bad = 1; *len = 0; return ""; }
	s = (const char *)(r->b + r->i);
	r->i += n;
	*len = n;
	return s;
}

typedef struct {
	uint8_t *b;
	uint32_t cap, n;
} Wr;

static void wr8(Wr *w, uint32_t v)
{
	if (w->n < w->cap)
		w->b[w->n++] = (uint8_t)v;
}

static void wr16(Wr *w, uint32_t v) { wr8(w, v); wr8(w, v >> 8); }
static void wr32(Wr *w, uint32_t v) { wr16(w, v); wr16(w, v >> 16); }
static void wr64(Wr *w, uint64_t v) { wr32(w, (uint32_t)v); wr32(w, (uint32_t)(v >> 32)); }

static void wrbytes(Wr *w, const void *p, uint32_t n)
{
	if (w->n + n > w->cap)
		n = w->cap - w->n;
	memcpy(w->b + w->n, p, n);
	w->n += n;
}

static void wrstr(Wr *w, const char *s)
{
	uint32_t n = (uint32_t)strlen(s);
	wr16(w, n);
	wrbytes(w, s, n);
}

/* ---- transport helpers ---- */
static int readfull(Pp9Transport *t, void *buf, uint32_t n)
{
	uint8_t *p = (uint8_t *)buf;
	uint32_t got = 0;
	while (got < n) {
		int r = t->read(t->ctx, p + got, n - got);
		if (r == 0)
			return 0;
		if (r < 0)
			return -1;
		got += (uint32_t)r;
	}
	return 1;
}

static int writefull(Pp9Transport *t, const void *buf, uint32_t n)
{
	const uint8_t *p = (const uint8_t *)buf;
	uint32_t sent = 0;
	while (sent < n) {
		int r = t->write(t->ctx, p + sent, n - sent);
		if (r <= 0)
			return -1;
		sent += (uint32_t)r;
	}
	return 0;
}

static int sendmsg(Pp9Transport *t, Wr *w)
{
	uint8_t hdr[4];
	uint32_t total = w->n + 4;
	hdr[0] = (uint8_t)total;
	hdr[1] = (uint8_t)(total >> 8);
	hdr[2] = (uint8_t)(total >> 16);
	hdr[3] = (uint8_t)(total >> 24);
	if (writefull(t, hdr, 4) < 0)
		return -1;
	return writefull(t, w->b, w->n);
}

static void reply_err(Pp9Transport *t, Pp9Server *srv, uint32_t tag, const char *msg)
{
	Wr w = { srv->tx, PP9_MSIZE, 0 };
	wr8(&w, Rerror);
	wr16(&w, tag);
	wrstr(&w, msg);
	sendmsg(t, &w);
}

/* ---- namespace helpers ---- */
static int find_file(Pp9Server *srv, const char *path, uint32_t len)
{
	int i;
	for (i = 0; i < srv->nfiles; i++) {
		const char *p = srv->files[i].path;
		if (strlen(p) == len && memcmp(p, path, len) == 0)
			return i;
	}
	return -1;
}

static const char *basename_of(const char *path)
{
	const char *s = strrchr(path, '/');
	if (path[0] == '/' && path[1] == '\0')
		return "/";
	return s ? s + 1 : path;
}

static int is_child(const char *parent, const char *path)
{
	uint32_t plen = (uint32_t)strlen(parent);
	if (strcmp(parent, "/") == 0)
		return path[0] == '/' && path[1] != '\0' && strchr(path + 1, '/') == NULL;
	return strncmp(path, parent, plen) == 0 && path[plen] == '/' &&
	       strchr(path + plen + 1, '/') == NULL;
}

static void wrqid(Wr *w, const Pp9File *f)
{
	/* Canonical 9P2000 qid wire order is type[1], vers[4],
	 * path[8] (Harvey sys/src/libc/9sys/convS2M.c pqid,
	 * plan9port lib9 convS2M.c). This was vers,type until
	 * B32: path bytes land at offset 5 either way and
	 * plan9port's 9p never checks qid.type, so ls/read
	 * appeared fine, but a real kernel mount checks
	 * QTDIR in cmount and rejected us with Emount
	 * ("inconsistent mount"). Same self-pair trap as
	 * the Tstat numbering in B30: mock9p shared the
	 * wrong order, so our own client agreed. */
	wr8(w, f->isdir ? 0x80 : 0);
	wr32(w, 0);
	wr64(w, f->qidpath);
}

static void wrstat(Wr *w, const Pp9File *f, uint64_t length)
{
	uint32_t start = w->n;
	uint32_t mode = f->isdir ? (0x80000000u | 0755) : (f->writable ? 0666 : 0444);
	wr16(w, 0); /* size, patched below */
	wr16(w, 0); /* type */
	wr32(w, 0); /* dev */
	wrqid(w, f);
	wr32(w, mode);
	wr32(w, 0); /* atime */
	wr32(w, 0); /* mtime */
	wr64(w, f->isdir ? 0 : length);
	wrstr(w, basename_of(f->path));
	wrstr(w, "esp9");
	wrstr(w, "esp9");
	wrstr(w, "esp9");
	w->b[start] = (uint8_t)((w->n - start - 2) & 0xFF);
	w->b[start + 1] = (uint8_t)(((w->n - start - 2) >> 8) & 0xFF);
}

/* Build file content. For directories, generates stat entries
 * into the arena. Returns buffer and length, or NULL. */
static const char * PP9_ICACHE file_content(Pp9Server *srv, int idx, uint32_t *len)
{
	const Pp9File *f = &srv->files[idx];
	int i;
	if (!f->isdir) {
		if (f->read == NULL) {
			*len = 0;
			return "";
		}
		return f->read(srv->appctx, len);
	}
	{
		uint32_t cap = 1024, used = 0;
		char *buf = pp_arena_alloc(srv->arena, cap, 8);
		if (buf == NULL) {
			*len = 0;
			return "";
		}
		for (i = 0; i < srv->nfiles; i++) {
			Wr w;
			uint32_t clen = 0;
			const Pp9File *c = &srv->files[i];
			if (!is_child(f->path, c->path))
				continue;
			if (!c->isdir && c->read != NULL)
				c->read(srv->appctx, &clen);
			w.b = (uint8_t *)buf + used;
			w.cap = cap - used;
			w.n = 0;
			wrstat(&w, c, clen);
			used += w.n;
		}
		*len = used;
		return buf;
	}
}

static Pp9Fid *findfid(Pp9Server *srv, uint32_t fid)
{
	int i;
	for (i = 0; i < PP9_MAXFIDS; i++)
		if (srv->fids[i].used && srv->fids[i].fidnum == fid)
			return &srv->fids[i];
	return NULL;
}

static Pp9Fid *newfid(Pp9Server *srv, uint32_t fid, int file)
{
	Pp9Fid *f = findfid(srv, fid);
	int i;
	if (f != NULL) {
		f->file = file;
		f->open = 0;
		return f;
	}
	for (i = 0; i < PP9_MAXFIDS; i++) {
		if (!srv->fids[i].used) {
			srv->fids[i].used = 1;
			srv->fids[i].fidnum = fid;
			srv->fids[i].file = file;
			srv->fids[i].open = 0;
			return &srv->fids[i];
		}
	}
	return NULL;
}

/* ---- message handling ---- */
void PP9_ICACHE
pp9_init(Pp9Server *srv, const Pp9File *files, int nfiles, void *appctx, pp_arena *arena)
{
	memset(srv, 0, sizeof(*srv));
	srv->files = files;
	srv->nfiles = nfiles;
	srv->appctx = appctx;
	srv->arena = arena;
}

static void PP9_ICACHE
handle(Pp9Server *srv, Pp9Transport *t, uint32_t typ, uint32_t tag, Rd *r)
{
	Wr w = { srv->tx, PP9_MSIZE, 0 };
	uint32_t mark = pp_arena_mark(srv->arena);

	switch (typ) {
	case Tversion: {
		uint32_t msize = rd32(r);
		uint32_t vlen;
		const char *ver = rdstr(r, &vlen);
		wr8(&w, Rversion);
		wr16(&w, tag);
		wr32(&w, msize < PP9_MSIZE ? msize : PP9_MSIZE);
		if (vlen == 6 && memcmp(ver, "9P2000", 6) == 0)
			wrstr(&w, "9P2000");
		else
			wrstr(&w, "unknown");
		break;
	}
	case Tauth:
		reply_err(t, srv, tag, "no authentication required");
		goto out;
	case Tattach: {
		uint32_t fid = rd32(r);
		int root = find_file(srv, "/", 1);
		rd32(r);
		rdstr(r, &(uint32_t){0});
		rdstr(r, &(uint32_t){0});
		if (root < 0 || newfid(srv, fid, root) == NULL) {
			reply_err(t, srv, tag, "cannot attach");
			goto out;
		}
		wr8(&w, Rattach);
		wr16(&w, tag);
		wrqid(&w, &srv->files[root]);
		break;
	}
	case Twalk: {
		uint32_t fid = rd32(r), newfidnum = rd32(r);
		uint32_t nw = rd16(r), i;
		Pp9Fid *src = findfid(srv, fid);
		int cur;
		uint32_t walked = 0;
		if (src == NULL) {
			reply_err(t, srv, tag, "unknown fid");
			goto out;
		}
		cur = src->file;
		if (nw == 0) {
			if (newfid(srv, newfidnum, cur) == NULL) {
				reply_err(t, srv, tag, "no free fids");
				goto out;
			}
		}
		wr8(&w, Rwalk);
		wr16(&w, tag);
		wr16(&w, 0); /* nwqid patched by count below via w.n fixup */
		for (i = 0; i < nw; i++) {
			uint32_t nlen;
			const char *name = rdstr(r, &nlen);
			const Pp9File *curf = &srv->files[cur];
			int nxt = -1;
			if (nlen == 2 && memcmp(name, "..", 2) == 0) {
				const char *p = curf->path;
				const char *slash = strrchr(p, '/');
				uint32_t plen = (slash == p) ? 1 : (uint32_t)(slash - p);
				nxt = find_file(srv, p, plen);
			} else {
				/* Build candidate path in arena. */
				uint32_t plen = (uint32_t)strlen(curf->path);
				char *cand = pp_arena_alloc(srv->arena, plen + 1 + nlen + 1, 1);
				if (cand == NULL) {
					reply_err(t, srv, tag, "no memory");
					goto out;
				}
				memcpy(cand, curf->path, plen);
				if (plen > 1)
					cand[plen++] = '/';
				if (plen == 1 && cand[0] == '/') {
					/* root: no extra slash needed */
				}
				memcpy(cand + plen, name, nlen);
				cand[plen + nlen] = '\0';
				nxt = find_file(srv, cand, plen + nlen);
			}
			if (nxt < 0)
				break;
			cur = nxt;
			wrqid(&w, &srv->files[cur]);
			walked++;
		}
		if (r->bad) {
			reply_err(t, srv, tag, "bad walk");
			goto out;
		}
		if (nw > 0 && walked == 0) {
			reply_err(t, srv, tag, "file does not exist");
			goto out;
		}
		if (walked == nw && newfid(srv, newfidnum, cur) == NULL) {
			reply_err(t, srv, tag, "no free fids");
			goto out;
		}
		srv->tx[3] = (uint8_t)(walked & 0xFF);
		srv->tx[4] = (uint8_t)((walked >> 8) & 0xFF);
		break;
	}
	case Topen: {
		uint32_t fid = rd32(r);
		uint32_t mode = r->b[r->i++];
		Pp9Fid *f = findfid(srv, fid);
		if (f == NULL) {
			reply_err(t, srv, tag, "unknown fid");
			goto out;
		}
		if ((mode & 3) != 0 && !srv->files[f->file].writable) {
			reply_err(t, srv, tag, "permission denied");
			goto out;
		}
		f->open = 1;
		wr8(&w, Ropen);
		wr16(&w, tag);
		wrqid(&w, &srv->files[f->file]);
		wr32(&w, 0);
		break;
	}
	case Tread: {
		uint32_t fid = rd32(r);
		uint64_t off = rd64(r);
		uint32_t count = rd32(r);
		Pp9Fid *f = findfid(srv, fid);
		uint32_t len = 0;
		const char *data;
		if (f == NULL) {
			reply_err(t, srv, tag, "unknown fid");
			goto out;
		}
		data = file_content(srv, f->file, &len);
		wr8(&w, Rread);
		wr16(&w, tag);
		if (off >= len) {
			wr32(&w, 0);
		} else {
			uint32_t avail = len - (uint32_t)off;
			if (avail > count)
				avail = count;
			if (avail > PP9_MSIZE - 64)
				avail = PP9_MSIZE - 64;
			wr32(&w, avail);
			wrbytes(&w, data + off, avail);
		}
		break;
	}
	case Twrite: {
		uint32_t fid = rd32(r);
		Pp9Fid *f;
		rd64(r);
		{
			uint32_t count = rd32(r);
			const char *data = (const char *)(r->b + r->i);
			const char *err;
			f = findfid(srv, fid);
			if (f == NULL) {
				reply_err(t, srv, tag, "unknown fid");
				goto out;
			}
			if (!srv->files[f->file].writable || srv->files[f->file].write == NULL) {
				reply_err(t, srv, tag, "file is read-only");
				goto out;
			}
			if (r->i + count > r->n) {
				reply_err(t, srv, tag, "bad write");
				goto out;
			}
			err = srv->files[f->file].write(srv->appctx, data, count);
			if (err != NULL) {
				reply_err(t, srv, tag, err);
				goto out;
			}
			wr8(&w, Rwrite);
			wr16(&w, tag);
			wr32(&w, count);
		}
		break;
	}
	case Tstat: {
		uint32_t fid = rd32(r);
		Pp9Fid *f = findfid(srv, fid);
		uint32_t len = 0;
		if (f == NULL) {
			reply_err(t, srv, tag, "unknown fid");
			goto out;
		}
		(void)file_content(srv, f->file, &len);
		wr8(&w, Rstat);
		wr16(&w, tag);
		wr16(&w, 0); /* outer stat length, patched below */
		{
			uint32_t at = w.n;
			wrstat(&w, &srv->files[f->file], len);
			srv->tx[3] = (uint8_t)((w.n - at) & 0xFF);
			srv->tx[4] = (uint8_t)(((w.n - at) >> 8) & 0xFF);
		}
		break;
	}
	case Tclunk: {
		uint32_t fid = rd32(r);
		Pp9Fid *f = findfid(srv, fid);
		if (f != NULL)
			f->used = 0;
		wr8(&w, Rclunk);
		wr16(&w, tag);
		break;
	}
	case Tflush:
		wr8(&w, Rflush);
		wr16(&w, tag);
		break;
	default:
		reply_err(t, srv, tag, "not implemented");
		goto out;
	}
	sendmsg(t, &w);
out:
	pp_arena_release(srv->arena, mark);
}

void PP9_ICACHE
pp9_reset(Pp9Server *srv)
{
	int i;
	for (i = 0; i < PP9_MAXFIDS; i++)
		srv->fids[i].used = 0;
	srv->hdrused = 0;
	srv->bodyused = 0;
	srv->bodyneed = 0;
}

int PP9_ICACHE
pp9_input(Pp9Server *srv, Pp9Transport *out, const uint8_t *data, uint32_t len)
{
	uint32_t i = 0;
	while (i < len) {
		if (srv->bodyneed == 0) {
			while (i < len && srv->hdrused < 4)
				srv->hdr[srv->hdrused++] = data[i++];
			if (srv->hdrused < 4)
				break;
			{
				uint32_t size = (uint32_t)srv->hdr[0] |
					((uint32_t)srv->hdr[1] << 8) |
					((uint32_t)srv->hdr[2] << 16) |
					((uint32_t)srv->hdr[3] << 24);
				if (size < 7 || size - 4 > PP9_MSIZE)
					return -1;
				srv->bodyneed = size - 4;
				srv->bodyused = 0;
				srv->hdrused = 0;
			}
		}
		if (srv->bodyneed > 0) {
			uint32_t want = srv->bodyneed - srv->bodyused;
			uint32_t avail = len - i;
			if (avail > want)
				avail = want;
			memcpy(srv->rx + srv->bodyused, data + i, avail);
			srv->bodyused += avail;
			i += avail;
			if (srv->bodyused == srv->bodyneed) {
				uint32_t typ = srv->rx[0];
				uint32_t tag = (uint32_t)srv->rx[1] |
					((uint32_t)srv->rx[2] << 8);
				Rd r = { srv->rx, srv->bodyneed, 3, 0 };
				handle(srv, out, typ, tag, &r);
				srv->bodyneed = 0;
				srv->bodyused = 0;
			}
		}
	}
	return 0;
}

int PP9_ICACHE
pp9_serve(Pp9Server *srv, Pp9Transport *t)
{
	for (;;) {
		uint8_t hdr[4];
		uint32_t size, typ, tag;
		int rr = readfull(t, hdr, 4);
		if (rr == 0)
			return 0;
		if (rr < 0)
			return -1;
		size = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
		       ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
		if (size < 7 || size - 4 > PP9_MSIZE)
			return -1;
		if (readfull(t, srv->rx, size - 4) <= 0)
			return -1;
		typ = srv->rx[0];
		tag = (uint32_t)srv->rx[1] | ((uint32_t)srv->rx[2] << 8);
		{
			Rd r = { srv->rx, size - 4, 3, 0 };
			handle(srv, t, typ, tag, &r);
		}
	}
}

