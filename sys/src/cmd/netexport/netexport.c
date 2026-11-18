/*
 * netexport - serve a directory over 9P2000 on a network address.
 *
 *	netexport [-D] addr root
 *	netexport tcp!*!5750 /mnt/host/data
 *
 * The x86 microVM guests run Inferno's export; ARM64 guests have no
 * Inferno, so this is the same service in Plan 9 C: announce, accept,
 * serve one 9P2000 session per connection, rooted at `root`.
 *
 * Read-only on purpose: a pod publishes data, it does not accept writes
 * into the host tree through the VM.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>

enum {
	Maxfid = 64,
	Maxmsz = 8192,
	Maxpath = 1024,
};

typedef struct Fid Fid;
struct Fid {
	int	inuse;
	int	fd;
	int	isdir;
	uchar	*dbuf;		/* packed stat entries for a directory */
	int	dlen;
	char	path[Maxpath];
};

static Fid fids[Maxfid];
static char *root;
static int msz = Maxmsz;
static int debug;

static void
usage(void)
{
	fprint(2, "usage: %s [-D] addr root\n", argv0);
	exits("usage");
}

static int
emptyfid(Fid *f)
{
	int fd;

	fd = f->fd;
	memset(f, 0, sizeof *f);
	f->fd = fd;
	return fd;
}

static Dir *
st(const char *path)
{
	return dirstat(path);
}

static void
srvversion(Fcall *t, Fcall *r)
{
	msz = t->msize;
	if(msz > Maxmsz)
		msz = Maxmsz;
	if(msz < 256)
		msz = 256;
	r->type = Rversion;
	r->msize = msz;
	r->version = "9P2000";
}

static void
srvattach(Fcall *t, Fcall *r)
{
	Dir *d;
	Fid *f = nil;
	int i;

	USED(t);
	for(i = 0; i < Maxfid; i++)
		if(!fids[i].inuse){
			f = &fids[i];
			break;
		}
	if(f == nil){
		r->type = Rerror;
		r->ename = "no free fids";
		return;
	}
	if((d = st(root)) == nil || (d->qid.type & QTDIR) == 0){
		free(d);
		r->type = Rerror;
		r->ename = "export root is not a directory";
		return;
	}
	memset(f, 0, sizeof *f);
	f->inuse = 1;
	f->fd = -1;
	snprint(f->path, sizeof f->path, "%s", root);
	r->type = Rattach;
	r->qid = d->qid;
	free(d);
}

static int
walkname(Fid *f, char *name, Qid *q)
{
	Dir *d;
	char p[Maxpath];

	if(strcmp(name, "..") == 0 || strchr(name, '/') != nil)
		return -1;
	if(strcmp(name, ".") == 0){
		if((d = st(f->path)) == nil)
			return -1;
		*q = d->qid;
		free(d);
		return 0;
	}
	snprint(p, sizeof p, "%s/%s", f->path, name);
	if((d = st(p)) == nil)
		return -1;
	*q = d->qid;
	free(d);
	snprint(f->path, sizeof f->path, "%s", p);
	return 0;
}

static void
srvwalk(Fcall *t, Fcall *r)
{
	Fid *f, *nf;
	Dir *d;
	Qid q;
	int i;

	if(t->fid < 0 || t->fid >= Maxfid || !fids[t->fid].inuse){
		r->type = Rerror;
		r->ename = "bad fid";
		return;
	}
	if(t->newfid < 0 || t->newfid >= Maxfid){
		r->type = Rerror;
		r->ename = "bad newfid";
		return;
	}
	f = &fids[t->fid];
	if((d = st(f->path)) == nil || (d->qid.type & QTDIR) == 0){
		free(d);
		r->type = Rerror;
		r->ename = "walk in non-directory";
		return;
	}
	free(d);
	nf = &fids[t->newfid];
	if(t->newfid != t->fid){
		if(nf->inuse){
			r->type = Rerror;
			r->ename = "newfid in use";
			return;
		}
		memset(nf, 0, sizeof *nf);
		nf->inuse = 1;
		nf->fd = -1;
		snprint(nf->path, sizeof nf->path, "%s", f->path);
	}
	r->type = Rwalk;
	r->nwqid = 0;
	for(i = 0; i < t->nwname; i++){
		if(walkname(nf, t->wname[i], &q) < 0){
			if(r->nwqid == 0){
				if(t->newfid != t->fid)
					emptyfid(nf);
				r->type = Rerror;
				r->ename = "file does not exist";
				return;
			}
			break;
		}
		r->wqid[r->nwqid++] = q;
	}
}

static void
srvopen(Fcall *t, Fcall *r)
{
	Fid *f;
	Dir *d;
	Dir *ents;
	int n, i, off;
	uchar *p;

	if(t->fid < 0 || t->fid >= Maxfid || !fids[t->fid].inuse){
		r->type = Rerror;
		r->ename = "bad fid";
		return;
	}
	f = &fids[t->fid];
	if((t->mode & 3) != OREAD){
		r->type = Rerror;
		r->ename = "read-only export";
		return;
	}
	if((d = st(f->path)) == nil){
		r->type = Rerror;
		r->ename = "stat failed";
		return;
	}
	if(d->qid.type & QTDIR){
		/* keep the fid's path: emptyfid() would erase it */
		if(f->fd >= 0)
			close(f->fd);
		f->fd = open(f->path, OREAD);
		if(f->fd < 0){
			free(d);
			r->type = Rerror;
			r->ename = "open failed";
			return;
		}
		/* Pack the whole directory once: reads then work on byte
		 * offsets into this buffer, which is what clients expect. */
		n = dirreadall(f->fd, &ents);
		/* room for each packed stat: name + uid + gid + fixed fields */
		f->dbuf = malloc((n > 0 ? (usize)n * 512 : 512));
		if(f->dbuf == nil){
			free(d);
			free(ents);
			r->type = Rerror;
			r->ename = "out of memory";
			return;
		}
		off = 0;
		for(i = 0; i < n; i++){
			p = f->dbuf + off;
			int m = convD2M(&ents[i], p,
			                (u32)((usize)n * 512 - (usize)off));

			if(m <= BIT16SZ)
				break;
			off += m;
		}
		f->dlen = off;
		free(ents);
		f->isdir = 1;
	}else{
		f->fd = open(f->path, OREAD);
		if(f->fd < 0){
			free(d);
			r->type = Rerror;
			r->ename = "open failed";
			return;
		}
		f->isdir = 0;
	}
	r->type = Ropen;
	r->qid = d->qid;
	r->iounit = 0;
	free(d);
}

static void
srvread(Fcall *t, Fcall *r, uchar **outp)
{
	Fid *f;
	uchar *out;
	int count, n;

	if(t->fid < 0 || t->fid >= Maxfid || !fids[t->fid].inuse){
		r->type = Rerror;
		r->ename = "bad fid";
		return;
	}
	f = &fids[t->fid];
	count = (int)t->count;
	if(count > msz - IOHDRSZ)
		count = msz - IOHDRSZ;
	if(count < 0)
		count = 0;
	out = malloc((usize)count + 1);
	if(out == nil){
		r->type = Rerror;
		r->ename = "out of memory";
		return;
	}
	if(f->isdir){
		long off = (long)t->offset;

		if(off < 0 || off > f->dlen)
			off = f->dlen;
		n = f->dlen - (int)off;
		if(n > count)
			n = count;
		if(n > 0)
			memmove(out, f->dbuf + off, n);
	}else{
		n = pread(f->fd, out, count, (long)t->offset);
		if(n < 0)
			n = 0;
	}
	r->type = Rread;
	r->count = n;
	r->data = (char *)out;
	*outp = out;
}

static void
srvclunk(Fcall *t, Fcall *r)
{
	Fid *f;

	if(t->fid < 0 || t->fid >= Maxfid || !fids[t->fid].inuse){
		r->type = Rerror;
		r->ename = "bad fid";
		return;
	}
	f = &fids[t->fid];
	if(f->fd >= 0)
		close(f->fd);
	free(f->dbuf);
	memset(f, 0, sizeof *f);
	r->type = Rclunk;
}

static void
srvstat(Fcall *t, Fcall *r, uchar **outp)
{
	Fid *f;
	Dir *d;
	uchar *out;
	int n;

	if(t->fid < 0 || t->fid >= Maxfid || !fids[t->fid].inuse){
		r->type = Rerror;
		r->ename = "bad fid";
		return;
	}
	f = &fids[t->fid];
	if((d = st(f->path)) == nil){
		r->type = Rerror;
		r->ename = "stat failed";
		return;
	}
	out = malloc(2048);
	if(out == nil){
		free(d);
		r->type = Rerror;
		r->ename = "out of memory";
		return;
	}
	n = convD2M(d, out, 2048);
	free(d);
	if(n <= BIT16SZ){
		free(out);
		r->type = Rerror;
		r->ename = "stat too large";
		return;
	}
	r->type = Rstat;
	r->nstat = n;
	r->stat = (char *)out;
	*outp = out;
}

static void
session(int conn)
{
	uchar *buf, *out;
	Fcall t, r;
	int n;

	buf = malloc(msz + IOHDRSZ);
	if(buf == nil)
		return;
	for(;;){
		if(readn(conn, buf, 4) != 4)
			break;
		n = GBIT32(buf);
		if(n < 7 || n > msz + IOHDRSZ)
			break;
		if(readn(conn, buf + 4, n - 4) != n - 4)
			break;
		if(convM2S(buf, n, &t) == 0)
			break;
		memset(&r, 0, sizeof r);
		r.tag = t.tag;
		out = nil;
		switch(t.type){
		case Tversion:
			srvversion(&t, &r);
			break;
		case Tattach:
			srvattach(&t, &r);
			break;
		case Twalk:
			srvwalk(&t, &r);
			break;
		case Topen:
			srvopen(&t, &r);
			break;
		case Tread:
			srvread(&t, &r, &out);
			break;
		case Tclunk:
			srvclunk(&t, &r);
			break;
		case Tstat:
			srvstat(&t, &r, &out);
			break;
		case Tflush:
			r.type = Rflush;
			break;
		default:
			r.type = Rerror;
			r.ename = "unsupported operation";
			break;
		}
		n = convS2M(&r, buf, msz + IOHDRSZ);
		free(out);
		if(n <= 0)
			break;
		if(debug)
			fprint(2, ">%F\n", &r);
		if(write(conn, buf, n) != n)
			break;
	}
	free(buf);
}

void
main(int argc, char **argv)
{
	char dir[NETPATHLEN], cdir[NETPATHLEN];
	int ctl, lfd, cfd;

	ARGBEGIN{
	case 'D':
		debug = 1;
		break;
	default:
		usage();
	}ARGEND;

	if(argc != 2)
		usage();
	root = argv[1];
	ctl = announce(argv[0], dir);
	if(ctl < 0)
		sysfatal("announce %s: %r", argv[0]);
	if(debug)
		fprint(2, "%s: serving %s on %s\n", argv0, root, argv[0]);

	for(;;){
		/* listen() waits for one connection and returns its
		 * directory; accept() opens <cdir>/data. */
		lfd = listen(dir, cdir);
		if(lfd < 0){
			sleep(100);
			continue;
		}
		cfd = accept(lfd, cdir);
		close(lfd);
		if(cfd < 0)
			continue;
		switch(rfork(RFPROC|RFFDG|RFNOWAIT)){
		case -1:
			close(cfd);
			break;
		case 0:
			close(ctl);
			session(cfd);
			close(cfd);
			exits(nil);
		default:
			close(cfd);
			break;
		}
	}
}
