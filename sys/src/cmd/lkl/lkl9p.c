/*
 * 9P2000 fileserver exporting LKL's Linux VFS (/dev, /sys, /proc, etc.)
 * to Harvey as /srv/lkl.
 *
 * No guest syscall except walk's single stat runs on the srv() thread:
 * open/create/stat/remove/read/write are handed to a pool of worker
 * threads that respond out of order by tag, so one slow or blocking
 * guest operation (directory loads, tty or evdev reads) cannot stall
 * the mount.  Guest device fds are O_NONBLOCK and workers poll them,
 * checking a per-request cancel flag; the flush handler sets that flag
 * so a flushed request is abandoned and responded instead of parking
 * its client forever.  Enqueue never blocks the srv() thread: a full
 * queue is an immediate error, not a stall.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "lklfw.h"

extern long	lkl_syscall(long, long*);
extern int	lkl_printf(const char*, ...);
extern int	lklhost_spawn(void (*)(void*), void*);

void
incref(void *p)
{
	Ref *r = p;

	__atomic_add_fetch(&r->ref, 1, __ATOMIC_SEQ_CST);
}

i32
decref(void *p)
{
	Ref *r = p;

	return __atomic_sub_fetch(&r->ref, 1, __ATOMIC_SEQ_CST);
}

int
proccreate(void (*fn)(void*), void *arg, uint stack)
{
	USED(fn);
	USED(arg);
	USED(stack);
	sysfatal("lkl9p: proccreate unused");
	return -1;
}

enum {
	LklNrIoctl	= 29,
	LklNrMknodat	= 33,
	LklNrMkdirat	= 34,
	LklNrUnlinkat	= 35,
	LklNrOpenat	= 56,
	LklNrClose	= 57,
	LklNrGetdents64	= 61,
	LklNrRead	= 63,
	LklNrWrite	= 64,
	LklNrPread64	= 67,
	LklNrPwrite64	= 68,
	LklNrNewfstatat	= 79,
	LklNrMount	= 40,
	LklNrReboot	= 142,

	LklAtFdcwd	= -100,
	LklAtRemovedir	= 0x200,

	LklOrdonly	= 0,
	LklOwronly	= 1,
	LklOrdwr	= 2,
	LklOcreat	= 0100,
	LklOtrunc	= 01000,
	LklOnonblock	= 04000,
	LklEagain	= 11,

	LklSifmt	= 0170000,
	LklSifdir	= 0040000,
	LklSifreg	= 0100000,

	Nworkers	= 8,
	ReqQsize	= 128,

	Opread		= 1,
	Opwrite		= 2,
	Opopen		= 3,
	Opcreate	= 4,
	Opstat		= 5,
	Opremove	= 6,
};

typedef struct LklStat LklStat;
struct LklStat {
	u64	st_dev;
	u64	st_ino;
	u32	st_mode;
	u32	st_nlink;
	u32	st_uid;
	u32	st_gid;
	u64	st_rdev;
	u64	__pad1;
	vlong	st_size;
	i32	st_blksize;
	i32	__pad2;
	vlong	st_blocks;
	vlong	st_atime;
	u64	st_atime_nsec;
	vlong	st_mtime;
	u64	st_mtime_nsec;
	vlong	st_ctime;
	u64	st_ctime_nsec;
	u32	__unused4;
	u32	__unused5;
};

typedef struct LklFid LklFid;
struct LklFid {
	char	*path;
	int	dir;
	int	isreg;
	long	fd;
	Dir	*dirs;
	int	ndirs;
	Lock	lk;		/* guards inflight/dead */
	int	inflight;	/* worker requests using this fid */
	int	dead;		/* destroyfid ran; last worker reaps */
};

typedef struct WorkItem WorkItem;
struct WorkItem {
	Req	*r;
	LklFid	*lf;
	int	op;
	int	cancelled;	/* set by flush; polled by workers */
};

static struct {
	QLock		lk;
	Rendez		notempty;
	WorkItem	*q[ReqQsize];
	int		head;
	int		tail;
	int		count;
} wq;

static long	lklclose(long);
static void	freedirs(LklFid*);

static void
lfhold(LklFid *lf)
{
	lock(&lf->lk);
	lf->inflight++;
	unlock(&lf->lk);
}

static void
reapfid(LklFid *lf)
{
	if(lf->fd >= 0)
		lklclose(lf->fd);
	freedirs(lf);
	free(lf->path);
	free(lf);
}

static void
lfrelease(LklFid *lf)
{
	int reap;

	lock(&lf->lk);
	reap = --lf->inflight == 0 && lf->dead;
	unlock(&lf->lk);
	if(reap)
		reapfid(lf);
}

static long
sys0(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
	long p[6];

	p[0] = a0;
	p[1] = a1;
	p[2] = a2;
	p[3] = a3;
	p[4] = a4;
	p[5] = a5;
	return lkl_syscall(nr, p);
}

static long
lklstat(const char *path, LklStat *st)
{
	return sys0(LklNrNewfstatat, LklAtFdcwd, (long)path, (long)st, 0, 0, 0);
}

static long
lklfstatat(int dfd, const char *path, LklStat *st)
{
	return sys0(LklNrNewfstatat, dfd, (long)path, (long)st, 0, 0, 0);
}

static long
lklopenat(int dfd, const char *path, int flags, int mode)
{
	return sys0(LklNrOpenat, dfd, (long)path, flags, mode, 0, 0);
}

static long
lklclose(long fd)
{
	return sys0(LklNrClose, fd, 0, 0, 0, 0, 0);
}

static long
lklmkdirat(int dfd, const char *path, int mode)
{
	return sys0(LklNrMkdirat, dfd, (long)path, mode, 0, 0, 0);
}

static long
lklunlinkat(int dfd, const char *path, int flags)
{
	return sys0(LklNrUnlinkat, dfd, (long)path, flags, 0, 0, 0);
}

static uvlong
hashpath(const char *s)
{
	uvlong h = 14695981039346656037ULL;

	while(*s){
		h ^= (uchar)*s++;
		h *= 1099511628211ULL;
	}
	return h ? h : 1;
}

static Qid
stat2qid(const char *path, LklStat *st)
{
	Qid q;

	q.path = st->st_ino ? st->st_ino : hashpath(path);
	q.vers = (u32)st->st_mtime;
	q.type = (st->st_mode & LklSifmt) == LklSifdir ? QTDIR : QTFILE;
	return q;
}

static void
stat2dir(const char *path, const char *name, LklStat *st, Dir *d)
{
	memset(d, 0, sizeof *d);
	d->name = estrdup9p((char*)name);
	d->uid = estrdup9p("lkl");
	d->gid = estrdup9p("lkl");
	d->muid = estrdup9p("lkl");
	d->qid = stat2qid(path, st);
	d->mode = (st->st_mode & 0777) |
		((st->st_mode & LklSifmt) == LklSifdir ? DMDIR : 0);
	d->atime = (u32)st->st_atime;
	d->mtime = (u32)st->st_mtime;
	d->length = st->st_size >= 0 ? (uvlong)st->st_size : 0;
}

static const char*
basename(const char *path)
{
	const char *p;

	if(strcmp(path, "/") == 0)
		return "/";
	p = strrchr(path, '/');
	return p && p[1] ? p + 1 : path;
}

static char*
joinpath(const char *dir, const char *name)
{
	int n;
	char *out;

	if(strcmp(dir, "/") == 0){
		n = 1 + strlen(name) + 1;
		out = emalloc9p(n);
		snprint(out, n, "/%s", name);
		return out;
	}
	n = strlen(dir) + 1 + strlen(name) + 1;
	out = emalloc9p(n);
	snprint(out, n, "%s/%s", dir, name);
	return out;
}

static char*
parentpath(const char *path)
{
	char *out, *p;

	if(strcmp(path, "/") == 0)
		return estrdup9p("/");
	out = estrdup9p((char*)path);
	p = strrchr(out, '/');
	if(p == nil || p == out){
		free(out);
		return estrdup9p("/");
	}
	*p = '\0';
	return out;
}

static char*
lklerr(long negerr)
{
	static char buf[64];

	switch(negerr){
	case 1:  return "operation not permitted";
	case 2:  return "file not found";
	case 5:  return "i/o error";
	case 9:  return "bad file descriptor";
	case 11: return "resource temporarily unavailable";
	case 12: return "out of memory";
	case 13: return "permission denied";
	case 17: return "file exists";
	case 20: return "not a directory";
	case 21: return "is a directory";
	case 22: return "invalid argument";
	case 28: return "no space left";
	case 30: return "read-only file system";
	case 32: return "broken pipe";
	case 39: return "directory not empty";
	default:
		snprint(buf, sizeof buf, "lkl errno %ld", negerr);
		return buf;
	}
}

static void
freedirs(LklFid *lf)
{
	int i;

	if(lf->dirs == nil)
		return;
	for(i = 0; i < lf->ndirs; i++){
		free(lf->dirs[i].name);
		free(lf->dirs[i].uid);
		free(lf->dirs[i].gid);
		free(lf->dirs[i].muid);
	}
	free(lf->dirs);
	lf->dirs = nil;
	lf->ndirs = 0;
}

static void
lkl9p_attach(Req *r)
{
	LklFid *lf;

	lf = emalloc9p(sizeof *lf);
	memset(lf, 0, sizeof *lf);
	lf->path = estrdup9p("/");
	lf->dir = 1;
	lf->fd = -1;
	r->fid->aux = lf;
	r->ofcall.qid.path = hashpath("/");
	r->ofcall.qid.vers = 0;
	r->ofcall.qid.type = QTDIR;
	r->fid->qid = r->ofcall.qid;
	respond(r, nil);
}

static char*
lkl9p_clone(Fid *oldfid, Fid *newfid)
{
	LklFid *olf, *nlf;

	olf = oldfid->aux;
	nlf = emalloc9p(sizeof *nlf);
	memset(nlf, 0, sizeof *nlf);
	nlf->path = estrdup9p(olf->path);
	nlf->dir = olf->dir;
	nlf->isreg = olf->isreg;
	nlf->fd = -1;
	newfid->aux = nlf;
	return nil;
}

static char*
lkl9p_walk1(Fid *fid, char *name, Qid *qid)
{
	LklFid *lf;
	LklStat st;
	char *np;
	long r;

	lf = fid->aux;
	if(strcmp(name, ".") == 0)
		np = estrdup9p(lf->path);
	else if(strcmp(name, "..") == 0)
		np = parentpath(lf->path);
	else
		np = joinpath(lf->path, name);

	r = lklstat(np, &st);
	if(r < 0){
		free(np);
		return "file not found";
	}
	free(lf->path);
	lf->path = np;
	lf->dir = (st.st_mode & LklSifmt) == LklSifdir;
	lf->isreg = (st.st_mode & LklSifmt) == LklSifreg && st.st_size > 0;
	*qid = stat2qid(np, &st);
	return nil;
}

static int
loaddirs(LklFid *lf, long dfd)
{
	char *buf, *cpath;
	long n, off;
	int cap;
	Dir *d;
	LklStat st;

	buf = emalloc9p(4096);
	cap = 16;
	lf->dirs = emalloc9p(cap * sizeof(Dir));
	lf->ndirs = 0;
	for(;;){
		n = sys0(LklNrGetdents64, dfd, (long)buf, 4096, 0, 0, 0);
		if(n <= 0)
			break;
		off = 0;
		while(off < n){
			u16 reclen;
			char *dname;

			memmove(&reclen, buf + off + 16, 2);
			dname = buf + off + 19;
			if(strcmp(dname, ".") != 0 && strcmp(dname, "..") != 0){
				if(lf->ndirs >= cap){
					cap *= 2;
					lf->dirs = erealloc9p(lf->dirs, cap * sizeof(Dir));
				}
				cpath = joinpath(lf->path, dname);
				if(lklfstatat(dfd, dname, &st) < 0 &&
				   lklstat(cpath, &st) < 0){
					memset(&st, 0, sizeof st);
					st.st_mode = LklSifreg | 0644;
				}
				d = &lf->dirs[lf->ndirs++];
				stat2dir(cpath, dname, &st, d);
				free(cpath);
			}
			off += reclen;
		}
	}
	free(buf);
	return 0;
}

extern int	wifiscan_buf(char*, int);
extern int	wificonnect(const char*, const char*);
extern int	wifidisconnect(void);
extern int	wifitcpcheck(const char*, int);
extern void	wifistatus_buf(char*, int);
extern int	modem_probe(void);
extern int	modem_set_opmode(int);
extern int	modem_scan(void);
extern int	modem_voice_dial(const char*);
extern int	modem_voice_answer(int);
extern int	modem_voice_hangup(int);
extern int	modem_voice_calls(void);
extern int	modem_wms_list(void);
extern int	modem_wms_send(const char*, const char*);
extern int	modem_wds_status(void);
extern int	modem_wds_start(const char*);
extern int	modem_wds_stop(void);
extern void	modemstatus_buf(char*, int);
extern char	lkl_dmesg[];
extern int	lkl_dmesg_len;
static long
readsysfsint(const char *path, long *out)
{
	char buf[64];
	long fd, n;

	fd = lklopenat(LklAtFdcwd, path, LklOrdonly, 0);
	if(fd < 0)
		return -1;
	n = sys0(LklNrRead, fd, (long)buf, sizeof(buf) - 1, 0, 0, 0);
	lklclose(fd);
	if(n <= 0)
		return -1;
	buf[n] = 0;
	*out = strtol(buf, nil, 10);
	return 0;
}

static void
powerstatus_buf(char *buf, int len)
{
	char pmi[64], pm[64], p[128], name[64];
	long vbat = -1, vph = -1, vbus = -1, ibus = -1;
	long chgt = -1, pmi_t = -1, pm_t = -1, xo_t = -1, cpu_t = -1;
	long pct = -1;
	int i;
	char *e = buf + len, *s = buf;

	pmi[0] = 0;
	pm[0] = 0;
	for(i = 0; i < 4; i++){
		long fd, n;
		snprint(p, sizeof p, "/sys/bus/iio/devices/iio:device%d/name", i);
		fd = lklopenat(LklAtFdcwd, p, LklOrdonly, 0);
		if(fd < 0)
			continue;
		n = sys0(LklNrRead, fd, (long)name, sizeof(name) - 1, 0, 0, 0);
		lklclose(fd);
		if(n <= 0)
			continue;
		name[n] = 0;
		if(strstr(name, "pmic@2:adc") != nil)
			snprint(pmi, sizeof pmi, "/sys/bus/iio/devices/iio:device%d", i);
		else if(strstr(name, "pmic@0:adc") != nil)
			snprint(pm, sizeof pm, "/sys/bus/iio/devices/iio:device%d", i);
	}
	if(pmi[0]){
		snprint(p, sizeof p, "%s/in_voltage_vbat_sns_input", pmi);
		readsysfsint(p, &vbat);
		snprint(p, sizeof p, "%s/in_voltage_vph_pwr_input", pmi);
		readsysfsint(p, &vph);
		snprint(p, sizeof p, "%s/in_voltage_usb_in_v_div_16_input", pmi);
		readsysfsint(p, &vbus);
		snprint(p, sizeof p, "%s/in_voltage_usb_in_i_uv_input", pmi);
		readsysfsint(p, &ibus);
		snprint(p, sizeof p, "%s/in_temp_chg_temp_input", pmi);
		readsysfsint(p, &chgt);
		snprint(p, sizeof p, "%s/in_temp_die_temp_input", pmi);
		readsysfsint(p, &pmi_t);
	}
	if(pm[0]){
		snprint(p, sizeof p, "%s/in_temp_die_temp_input", pm);
		readsysfsint(p, &pm_t);
		snprint(p, sizeof p, "%s/in_temp_xo_therm_input", pm);
		readsysfsint(p, &xo_t);
	}
	readsysfsint("/sys/class/thermal/thermal_zone0/temp", &cpu_t);

	if(vbat >= 0){
		long mv = vbat / 1000;
		if(mv <= 3400)
			pct = 0;
		else if(mv >= 4350)
			pct = 100;
		else
			pct = (mv - 3400) * 100 / (4350 - 3400);
		s = seprint(s, e, "vbat_mv %ld\nest_pct %ld\n", mv, pct);
	}
	if(vph >= 0)
		s = seprint(s, e, "vph_mv %ld\n", vph / 1000);
	if(vbus >= 0)
		s = seprint(s, e, "vbus_mv %ld\nusb_present %d\n",
			vbus / 1000, (vbus / 1000) > 4000 ? 1 : 0);
	if(ibus >= 0)
		s = seprint(s, e, "ibus_ua %ld\n", ibus);
	if(chgt >= 0)
		s = seprint(s, e, "chg_temp_mc %ld\n", chgt);
	if(pmi_t >= 0)
		s = seprint(s, e, "pmi632_temp_mc %ld\n", pmi_t);
	if(pm_t >= 0)
		s = seprint(s, e, "pm6125_temp_mc %ld\n", pm_t);
	if(xo_t >= 0)
		s = seprint(s, e, "xo_temp_mc %ld\n", xo_t);
	if(cpu_t >= 0)
		s = seprint(s, e, "cpu_temp_mc %ld\n", cpu_t);
	if(s == buf)
		seprint(s, e, "status unavailable\n");
}

static QLock	wifiscanlk;
static QLock	modemlk;

static void
doopen(Req *r, LklFid *lf)
{
	int m, flags;
	long fd;

	if(lf->dir){
		fd = lklopenat(LklAtFdcwd, lf->path, LklOrdonly, 0);
		if(fd < 0){
			respond(r, lklerr(-fd));
			return;
		}
		freedirs(lf);
		loaddirs(lf, fd);
		lklclose(fd);
		lf->fd = -1;
		respond(r, nil);
		return;
	}

	m = r->ifcall.mode & 3;
	if(strcmp(lf->path, "/dmesg") == 0 && (m == OREAD || m == ORDWR)){
		long dfd = lklopenat(LklAtFdcwd, "/dmesg",
			LklOwronly | LklOcreat | LklOtrunc, 0444);
		if(dfd >= 0){
			int len = lkl_dmesg_len;
			if(len > 0)
				sys0(LklNrWrite, dfd, (long)lkl_dmesg, len, 0, 0, 0);
			lklclose(dfd);
		}
	}
	if(strcmp(lf->path, "/wifistatus") == 0 && (m == OREAD || m == ORDWR)){
		char sbuf[512];
		long sfd;

		wifistatus_buf(sbuf, sizeof sbuf);
		sfd = lklopenat(LklAtFdcwd, "/wifistatus",
			LklOwronly | LklOcreat | LklOtrunc, 0444);
		if(sfd >= 0){
			sys0(LklNrWrite, sfd, (long)sbuf, strlen(sbuf), 0, 0, 0);
			lklclose(sfd);
		}
	}
	if(strcmp(lf->path, "/modemstatus") == 0 && (m == OREAD || m == ORDWR)){
		char mbuf[2048];
		long mfd;

		qlock(&modemlk);
		modemstatus_buf(mbuf, sizeof mbuf);
		qunlock(&modemlk);
		mfd = lklopenat(LklAtFdcwd, "/modemstatus",
			LklOwronly | LklOcreat | LklOtrunc, 0444);
		if(mfd >= 0){
			sys0(LklNrWrite, mfd, (long)mbuf, strlen(mbuf), 0, 0, 0);
			lklclose(mfd);
		}
	}
	if(strcmp(lf->path, "/power") == 0 && (m == OREAD || m == ORDWR)){
		char pbuf[512];
		long pfd;

		powerstatus_buf(pbuf, sizeof pbuf);
		pfd = lklopenat(LklAtFdcwd, "/power",
			LklOwronly | LklOcreat | LklOtrunc, 0444);
		if(pfd >= 0){
			sys0(LklNrWrite, pfd, (long)pbuf, strlen(pbuf), 0, 0, 0);
			lklclose(pfd);
		}
	}
	if(strcmp(lf->path, "/wifiscan") == 0 && (m == OREAD || m == ORDWR)){
		char *scanbuf;
		long wfd, n;

		qlock(&wifiscanlk);
		scanbuf = malloc(16384);
		if(scanbuf != nil){
			wifiscan_buf(scanbuf, 16384);
			wfd = lklopenat(LklAtFdcwd, "/wifiscan",
				LklOwronly | LklOcreat | LklOtrunc, 0444);
			if(wfd >= 0){
				n = strlen(scanbuf);
				if(n > 0)
					sys0(LklNrWrite, wfd, (long)scanbuf, n, 0, 0, 0);
				lklclose(wfd);
			}
			free(scanbuf);
		}
		qunlock(&wifiscanlk);
	}
	switch(m){
	case OREAD:  flags = LklOrdonly; break;
	case OWRITE: flags = LklOwronly; break;
	case ORDWR:  flags = LklOrdwr; break;
	case OEXEC:  flags = LklOrdonly; break;
	default:     flags = LklOrdonly; break;
	}
	if(r->ifcall.mode & OTRUNC)
		flags |= LklOtrunc;
	if(!lf->isreg)
		flags |= LklOnonblock;

	fd = lklopenat(LklAtFdcwd, lf->path, flags, 0);
	if(fd < 0){
		respond(r, lklerr(-fd));
		return;
	}
	if(strcmp(lf->path, "/dev/ttyGS0") == 0){
		struct {
			u32 c_iflag, c_oflag, c_cflag, c_lflag;
			uchar c_line;
			uchar c_cc[19];
		} t;
		memset(&t, 0, sizeof t);
		if(sys0(LklNrIoctl, fd, 0x5401, (long)&t, 0, 0, 0) >= 0){
			t.c_iflag = 0;
			t.c_oflag = 0;
			t.c_lflag = 0;
			t.c_cc[6] = 1;
			t.c_cc[5] = 0;
			sys0(LklNrIoctl, fd, 0x5402, (long)&t, 0, 0, 0);
		}
	}
	lf->fd = fd;
	respond(r, nil);
}

static void
docreate(Req *r, LklFid *lf)
{
	LklStat st;
	char *np;
	int m, flags, perm;
	long fd, res;

	if(!lf->dir){
		respond(r, "create in non-directory");
		return;
	}
	np = joinpath(lf->path, r->ifcall.name);
	perm = r->ifcall.perm & 0777;
	if(r->ifcall.perm & DMDIR){
		res = lklmkdirat(LklAtFdcwd, np, perm);
		if(res < 0){
			free(np);
			respond(r, lklerr(-res));
			return;
		}
		free(lf->path);
		lf->path = np;
		lf->dir = 1;
		lf->isreg = 0;
		lf->fd = -1;
	}else{
		m = r->ifcall.mode & 3;
		flags = LklOcreat | LklOtrunc |
			(m == OWRITE ? LklOwronly : (m == ORDWR ? LklOrdwr : LklOrdonly));
		fd = lklopenat(LklAtFdcwd, np, flags, perm);
		if(fd < 0){
			free(np);
			respond(r, lklerr(-fd));
			return;
		}
		free(lf->path);
		lf->path = np;
		lf->dir = 0;
		lf->isreg = 1;
		lf->fd = fd;
	}
	if(lklstat(lf->path, &st) == 0){
		r->ofcall.qid = stat2qid(lf->path, &st);
	}else{
		r->ofcall.qid.path = hashpath(lf->path);
		r->ofcall.qid.vers = 0;
		r->ofcall.qid.type = lf->dir ? QTDIR : QTFILE;
	}
	r->fid->qid = r->ofcall.qid;
	respond(r, nil);
}

static int
lkl9p_dirgen(int i, Dir *d, void *aux)
{
	LklFid *lf = aux;

	if(i < 0 || i >= lf->ndirs)
		return -1;
	*d = lf->dirs[i];
	d->name = estrdup9p(lf->dirs[i].name);
	d->uid = estrdup9p(lf->dirs[i].uid);
	d->gid = estrdup9p(lf->dirs[i].gid);
	d->muid = estrdup9p(lf->dirs[i].muid);
	return 0;
}

/*
 * Hand a request to the worker pool.  Never blocks: a full queue is
 * an error response, so the srv() thread always keeps consuming.
 * The WorkItem lives in r->aux until lib9p destroys the Req.
 */
static void
enqueuework(Req *r, int op)
{
	WorkItem *wi;
	LklFid *lf;

	lf = r->fid->aux;
	wi = emalloc9p(sizeof *wi);
	wi->r = r;
	wi->lf = lf;
	wi->op = op;
	wi->cancelled = 0;
	r->aux = wi;
	lfhold(lf);
	qlock(&wq.lk);
	if(wq.count == ReqQsize){
		qunlock(&wq.lk);
		r->aux = nil;
		free(wi);
		lfrelease(lf);
		respond(r, "lkl server busy");
		return;
	}
	wq.q[wq.tail] = wi;
	wq.tail = (wq.tail + 1) % ReqQsize;
	wq.count++;
	rwakeup(&wq.notempty);
	qunlock(&wq.lk);
}

static int
cancelled(WorkItem *wi)
{
	return __atomic_load_n(&wi->cancelled, __ATOMIC_RELAXED);
}

static void
doread(Req *r, LklFid *lf, WorkItem *wi)
{
	long n;

	if(lf->isreg){
		n = sys0(LklNrPread64, lf->fd, (long)r->ofcall.data,
			r->ifcall.count, (long)r->ifcall.offset, 0, 0);
	}else{
		for(;;){
			if(cancelled(wi)){
				respond(r, "interrupted");
				return;
			}
			n = sys0(LklNrRead, lf->fd, (long)r->ofcall.data,
				r->ifcall.count, 0, 0, 0);
			if(n != -LklEagain)
				break;
			sleep(10);
		}
	}
	if(n < 0){
		respond(r, lklerr(-n));
		return;
	}
	r->ofcall.count = (u32)n;
	respond(r, nil);
}

static void
ipabind_thread(void *arg)
{
	long bfd, br;

	USED(arg);
	bfd = lklopenat(LklAtFdcwd, "/sys/bus/platform/drivers/ipa/bind", LklOwronly, 0);
	if(bfd >= 0){
		br = sys0(LklNrWrite, bfd, (long)"5840000.ipa", 11, 0, 0, 0);
		lklclose(bfd);
		lkl_printf("lkl9p: bind 5840000.ipa -> %ld\n", br);
	}
}

static void
dowrite(Req *r, LklFid *lf, WorkItem *wi)
{
	long n, tot;

	if(strcmp(lf->path, "/wifictl") == 0){
		char cmd[256], *p, *q;
		int len = r->ifcall.count;

		if(len >= (int)sizeof cmd)
			len = sizeof(cmd) - 1;
		memmove(cmd, r->ifcall.data, len);
		cmd[len] = 0;
		while(len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r' || cmd[len-1] == ' '))
			cmd[--len] = 0;
		if(strncmp(cmd, "connect ", 8) == 0){
			p = cmd + 8;
			while(*p == ' ')
				p++;
			q = strchr(p, ' ');
			if(q != nil){
				*q++ = 0;
				while(*q == ' ')
					q++;
			}
			qlock(&wifiscanlk);
			wificonnect(p, q);
			qunlock(&wifiscanlk);
		}else if(strcmp(cmd, "disconnect") == 0){
			qlock(&wifiscanlk);
			wifidisconnect();
			qunlock(&wifiscanlk);
		}else if(strncmp(cmd, "tcpcheck ", 9) == 0){
			int port = 80;
			p = cmd + 9;
			while(*p == ' ')
				p++;
			q = strchr(p, ' ');
			if(q != nil){
				*q++ = 0;
				port = (int)strtol(q, nil, 10);
			}
			wifitcpcheck(p, port);
		}
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	if(strcmp(lf->path, "/modemctl") == 0){
		char cmd[128];
		int len = r->ifcall.count;

		if(len >= (int)sizeof cmd)
			len = sizeof(cmd) - 1;
		memmove(cmd, r->ifcall.data, len);
		cmd[len] = 0;
		while(len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r' || cmd[len-1] == ' '))
			cmd[--len] = 0;
		qlock(&modemlk);
		if(strcmp(cmd, "probe") == 0)
			modem_probe();
		else if(strcmp(cmd, "online") == 0)
			modem_set_opmode(0);
		else if(strcmp(cmd, "lpm") == 0)
			modem_set_opmode(1);
		else if(strcmp(cmd, "offline") == 0)
			modem_set_opmode(3);
		else if(strcmp(cmd, "reset") == 0)
			modem_set_opmode(4);
		else if(strcmp(cmd, "scan") == 0)
			modem_scan();
		else if(strcmp(cmd, "calls") == 0)
			modem_voice_calls();
		else if(strncmp(cmd, "dial ", 5) == 0)
			modem_voice_dial(cmd + 5);
		else if(strncmp(cmd, "answer", 6) == 0)
			modem_voice_answer(cmd[6] == ' ' ? atoi(cmd + 7) : 0);
		else if(strncmp(cmd, "hangup", 6) == 0)
			modem_voice_hangup(cmd[6] == ' ' ? atoi(cmd + 7) : 0);
		else if(strcmp(cmd, "smslist") == 0)
			modem_wms_list();
		else if(strncmp(cmd, "sms ", 4) == 0){
			char *sp = strchr(cmd + 4, ' ');
			if(sp != nil){
				*sp++ = 0;
				modem_wms_send(cmd + 4, sp);
			}
		}
		else if(strcmp(cmd, "datastat") == 0)
			modem_wds_status();
		else if(strncmp(cmd, "dataup ", 7) == 0)
			modem_wds_start(cmd + 7);
		else if(strcmp(cmd, "datadown") == 0)
			modem_wds_stop();
		else if(strcmp(cmd, "ipabind") == 0)
			lklhost_spawn(ipabind_thread, nil);
		qunlock(&modemlk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	if(lf->isreg){
		n = sys0(LklNrPwrite64, lf->fd, (long)r->ifcall.data,
			r->ifcall.count, (long)r->ifcall.offset, 0, 0);
		if(n < 0){
			respond(r, lklerr(-n));
			return;
		}
		r->ofcall.count = (u32)n;
		respond(r, nil);
		return;
	}
	if(r->ifcall.count >= 6 && memcmp(r->ifcall.data, "TXTEST", 6) == 0){
		char line[80];
		int k, llen, loff;
		for(k = 0; k < 100; k++){
			llen = snprint(line, sizeof line,
				"TXLINE %04d 0123456789abcdef0123456789abcdef0123456789abcdef\n", k);
			loff = 0;
			while(loff < llen){
				n = sys0(LklNrWrite, lf->fd, (long)(line + loff), llen - loff, 0, 0, 0);
				if(n > 0) loff += n;
				else if(n == -LklEagain) sleep(5);
				else break;
			}
		}
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	tot = 0;
	while(tot < r->ifcall.count){
		if(cancelled(wi)){
			if(tot > 0)
				break;
			respond(r, "interrupted");
			return;
		}
		n = sys0(LklNrWrite, lf->fd,
			(long)(r->ifcall.data + tot),
			r->ifcall.count - tot, 0, 0, 0);
		if(n == -LklEagain){
			sleep(10);
			continue;
		}
		if(n < 0){
			if(tot > 0)
				break;
			respond(r, lklerr(-n));
			return;
		}
		if(n == 0)
			break;
		tot += n;
	}
	r->ofcall.count = (u32)tot;
	respond(r, nil);
}

static void
dostat(Req *r, LklFid *lf)
{
	LklStat st;
	long res;

	res = lklstat(lf->path, &st);
	if(res < 0){
		respond(r, lklerr(-res));
		return;
	}
	stat2dir(lf->path, basename(lf->path), &st, &r->d);
	respond(r, nil);
}

static void
doremove(Req *r, LklFid *lf)
{
	long res;

	res = lklunlinkat(LklAtFdcwd, lf->path, lf->dir ? LklAtRemovedir : 0);
	if(res < 0){
		respond(r, lklerr(-res));
		return;
	}
	respond(r, nil);
}

static void
worker(void *arg)
{
	WorkItem *wi;
	Req *r;
	LklFid *lf;

	USED(arg);
	for(;;){
		qlock(&wq.lk);
		while(wq.count == 0)
			rsleep(&wq.notempty);
		wi = wq.q[wq.head];
		wq.head = (wq.head + 1) % ReqQsize;
		wq.count--;
		qunlock(&wq.lk);

		r = wi->r;
		lf = wi->lf;
		if(cancelled(wi)){
			respond(r, "interrupted");
			lfrelease(lf);
			continue;
		}
		switch(wi->op){
		case Opopen:
			doopen(r, lf);
			break;
		case Opcreate:
			docreate(r, lf);
			break;
		case Opstat:
			dostat(r, lf);
			break;
		case Opremove:
			doremove(r, lf);
			break;
		case Opread:
		case Opwrite:
			if(lf->fd < 0){
				respond(r, "fid not open");
				break;
			}
			if(wi->op == Opread)
				doread(r, lf, wi);
			else
				dowrite(r, lf, wi);
			break;
		default:
			respond(r, "bad work op");
			break;
		}
		/* wi is freed by lkl9p_destroyreq; do not touch it now. */
		lfrelease(lf);
	}
}

static void
lkl9p_open(Req *r)
{
	enqueuework(r, Opopen);
}

static void
lkl9p_create(Req *r)
{
	enqueuework(r, Opcreate);
}

static void
lkl9p_read(Req *r)
{
	LklFid *lf;

	lf = r->fid->aux;
	if(lf->dir){
		/* Directory contents were loaded at open; no guest call. */
		dirread9p(r, lkl9p_dirgen, lf);
		respond(r, nil);
		return;
	}
	enqueuework(r, Opread);
}

static void
lkl9p_write(Req *r)
{
	enqueuework(r, Opwrite);
}

static void
lkl9p_stat(Req *r)
{
	enqueuework(r, Opstat);
}

static void
lkl9p_wstat(Req *r)
{
	respond(r, nil);
}

static void
lkl9p_remove(Req *r)
{
	enqueuework(r, Opremove);
}

/*
 * A flush only has to make the target request finish: flag it, and
 * lib9p answers this flush once the target is responded.  Workers
 * poll the flag between nonblocking guest calls, so a parked device
 * read is abandoned within one poll interval.
 */
static void
lkl9p_flush(Req *r)
{
	WorkItem *wi;

	if(r->oldreq != nil && (wi = r->oldreq->aux) != nil)
		__atomic_store_n(&wi->cancelled, 1, __ATOMIC_RELAXED);
	respond(r, nil);
}

static void
lkl9p_destroyreq(Req *r)
{
	free(r->aux);
	r->aux = nil;
}

static void
lkl9p_destroyfid(Fid *fid)
{
	LklFid *lf;

	lf = fid->aux;
	if(lf == nil)
		return;
	fid->aux = nil;
	lock(&lf->lk);
	lf->dead = 1;
	if(lf->inflight > 0){
		/* Workers still hold it; the last one out reaps. */
		unlock(&lf->lk);
		return;
	}
	unlock(&lf->lk);
	reapfid(lf);
}

static Srv lkl9psrv = {
	.attach		= lkl9p_attach,
	.clone		= lkl9p_clone,
	.walk1		= lkl9p_walk1,
	.open		= lkl9p_open,
	.create		= lkl9p_create,
	.read		= lkl9p_read,
	.write		= lkl9p_write,
	.stat		= lkl9p_stat,
	.wstat		= lkl9p_wstat,
	.remove		= lkl9p_remove,
	.flush		= lkl9p_flush,
	.destroyfid	= lkl9p_destroyfid,
	.destroyreq	= lkl9p_destroyreq,
};

extern char lkl_dmesg[];
extern int lkl_dmesg_len;

/*
 * Reboot-to-bootloader watcher.  The shell asks by writing
 * "bootloader" to /mnt/lkl/rebootbl (a plain file in the guest
 * root).  We answer with the Linux reboot syscall so the guest's
 * reboot-mode driver records the bootloader reason in the PMIC,
 * then reset the SoC through Harvey's /dev/reboot.  This lets the
 * host drive fastboot boot iterations without touching the phone.
 */
static void
rebootwatch(void *arg)
{
	char buf[64];
	long fd, n;
	int hfd;

	USED(arg);
	sleep(5000);
	for(;;){
		fd = lklopenat(LklAtFdcwd, "/rebootbl", LklOrdonly, 0);
		if(fd >= 0){
			memset(buf, 0, sizeof buf);
			n = sys0(LklNrRead, fd, (long)buf, sizeof(buf) - 1, 0, 0, 0);
			lklclose(fd);
			if(n > 0 && strstr(buf, "bootloader") != nil){
				lkl_printf("lkl9p: reboot to bootloader requested\n");
				sys0(LklNrReboot, 0xfee1dead, 672274793,
					0xA1B2C3D4, (long)"bootloader", 0, 0);
				/* Guest is down; reset the SoC itself. */
				hfd = open("/dev/reboot", OWRITE);
				if(hfd >= 0){
					write(hfd, "reboot", 6);
					close(hfd);
				}
				exits(nil);
			}
		}
		sleep(500);
	}
}

static u32
lp_le32(u8 *p)
{
	return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24);
}

static u64
lp_le64(u8 *p)
{
	return (u64)lp_le32(p) | ((u64)lp_le32(p + 4) << 32);
}

/*
 * Parse Android dynamic partition LP metadata inside 'super' and mount
 * vendor_a (or vendor_b) strictly read-only at /fw/vendor via /dev/loop0.
 */
static void
mount_super_vendor(char *superdev)
{
	u8 geo[64], hdr[256], *tbl, sb[128];
	u32 magic, hdrsz, tblsz, meta_max = 65536, nslots = 2, slot;
	u32 poff, pnum, pesz, eoff, enum_, eesz, i;
	u64 voff = 0, vsz = 0;
	int is_erofs = 0;
	long sfd, lfd, r;
	u8 linfo[232];

	sfd = lklopenat(LklAtFdcwd, superdev, LklOrdonly, 0);
	if(sfd < 0){
		lkl_printf("lkl9p: open %s -> %ld\n", superdev, sfd);
		return;
	}
	if(sys0(LklNrPread64, sfd, (long)geo, sizeof geo, 0x1000, 0, 0) == sizeof geo &&
	   lp_le32(geo) == 0x616c4467){
		meta_max = lp_le32(geo + 0x28);
		nslots = lp_le32(geo + 0x2c);
		if(meta_max == 0 || meta_max > 262144) meta_max = 65536;
		if(nslots == 0 || nslots > 4) nslots = 2;
	}
	for(slot = 0; slot < nslots && vsz == 0; slot++){
		u64 mbase = 0x3000ULL + (u64)slot * meta_max;
		if(sys0(LklNrPread64, sfd, (long)hdr, sizeof hdr, mbase, 0, 0) < 0x68)
			continue;
		magic = lp_le32(hdr + 0x00);
		hdrsz = lp_le32(hdr + 0x08);
		tblsz = lp_le32(hdr + 0x2c);
		if(magic != 0x414c5030 || hdrsz < 0x68 || tblsz == 0 || tblsz > 65536)
			continue;
		poff = lp_le32(hdr + 0x50);
		pnum = lp_le32(hdr + 0x54);
		pesz = lp_le32(hdr + 0x58);
		eoff = lp_le32(hdr + 0x5c);
		enum_ = lp_le32(hdr + 0x60);
		eesz = lp_le32(hdr + 0x64);
		tbl = malloc(tblsz);
		if(tbl == nil)
			break;
		if(sys0(LklNrPread64, sfd, (long)tbl, tblsz, mbase + hdrsz, 0, 0) != (long)tblsz){
			free(tbl);
			continue;
		}
		for(i = 0; i < pnum; i++){
			u8 *pe = tbl + poff + i * pesz;
			u32 eidx, en;
			if(poff + (i + 1) * pesz > tblsz)
				break;
			if(strncmp((char *)pe, "vendor_a", 36) != 0 &&
			   strncmp((char *)pe, "vendor_b", 36) != 0)
				continue;
			eidx = lp_le32(pe + 0x28);
			en = lp_le32(pe + 0x2c);
			if(en == 0 || eidx >= enum_ || eoff + (eidx + 1) * eesz > tblsz)
				continue;
			{
				u8 *ee = tbl + eoff + eidx * eesz;
				u32 ttype = lp_le32(ee + 0x08);
				u64 csz = lp_le64(ee + 0x00) * 512ULL;
				u64 coff = lp_le64(ee + 0x0c) * 512ULL;
				u32 m400 = 0, m438 = 0;
				if(ttype != 0)
					continue;
				if(sys0(LklNrPread64, sfd, (long)sb, sizeof sb, coff + 0x400, 0, 0) == sizeof sb){
					m400 = lp_le32(sb);
					m438 = lp_le32(sb + 0x38);
				}
				lkl_printf("lkl9p: super slot%ud %.16s off=%#llux sz=%#llux @400=%#ux @438=%#ux\n",
					slot, (char *)pe, coff, csz, m400, m438);
				if((m438 & 0xffff) == 0xef53){
					voff = coff; vsz = csz; is_erofs = 0;
					break;
				}
				if(m400 == 0xe0f5e1e2){
					voff = coff; vsz = csz; is_erofs = 1;
					break;
				}
			}
		}
		free(tbl);
	}
	if(vsz == 0){
		lklclose(sfd);
		return;
	}
	sys0(LklNrMknodat, LklAtFdcwd, (long)"/dev/loop0", 0060600, (7<<8) | 0, 0, 0);
	lfd = lklopenat(LklAtFdcwd, "/dev/loop0", LklOrdonly, 0);
	if(lfd < 0){
		lkl_printf("lkl9p: open /dev/loop0 -> %ld\n", lfd);
		lklclose(sfd);
		return;
	}
	r = sys0(LklNrIoctl, lfd, 0x4c00, sfd, 0, 0, 0);	/* LOOP_SET_FD */
	if(r < 0){
		lkl_printf("lkl9p: LOOP_SET_FD -> %ld\n", r);
		lklclose(lfd);
		lklclose(sfd);
		return;
	}
	memset(linfo, 0, sizeof linfo);
	*(u64 *)(linfo + 0x18) = voff;		/* lo_offset */
	*(u64 *)(linfo + 0x20) = vsz;		/* lo_sizelimit */
	*(u32 *)(linfo + 0x34) = 1;		/* LO_FLAGS_READ_ONLY */
	r = sys0(LklNrIoctl, lfd, 0x4c04, (long)linfo, 0, 0, 0);	/* LOOP_SET_STATUS64 */
	lklclose(lfd);
	lklclose(sfd);
	if(r < 0){
		lkl_printf("lkl9p: LOOP_SET_STATUS64 -> %ld\n", r);
		return;
	}
	lklmkdirat(LklAtFdcwd, "/fw/vendor", 0555);
	r = sys0(LklNrMount, (long)"/dev/loop0", (long)"/fw/vendor",
		 (long)(is_erofs ? "erofs" : "ext4"), 1, 0, 0);
	lkl_printf("lkl9p: mount /dev/loop0 /fw/vendor (%s RO) -> %ld\n",
		is_erofs ? "erofs" : "ext4", r);
}

static void
mkblocknodes(void *arg)
{
	static char *names[] = {
		"mmcblk0", "mmcblk0boot0", "mmcblk0boot1",
	};
	int tries, i;
	char spath[96], npath[96], dbuf[32];

	USED(arg);
	lklmkdirat(LklAtFdcwd, "/dev", 0755);
	for(tries = 0; tries < 120; tries++){
		int missing = 0;

		for(i = -77; i < (int)nelem(names); i++){
			long sfd, n;
			unsigned int maj, min;
			char *colon, name[32];

			if(i < 0)
				snprint(name, sizeof name, "mmcblk0p%d", -i);
			else
				snprint(name, sizeof name, "%s", names[i]);
			snprint(npath, sizeof npath, "/dev/%s", name);
			sfd = lklopenat(LklAtFdcwd, npath, LklOrdonly, 0);
			if(sfd >= 0){
				lklclose(sfd);
				continue;
			}
			snprint(spath, sizeof spath,
				"/sys/class/block/%s/dev", name);
			sfd = lklopenat(LklAtFdcwd, spath, LklOrdonly, 0);
			if(sfd < 0){
				/* partitions appear after the card */
				if(i >= 0)
					missing = 1;
				continue;
			}
			n = sys0(LklNrRead, sfd, (long)dbuf,
				 sizeof dbuf - 1, 0, 0, 0);
			lklclose(sfd);
			if(n <= 0)
				continue;
			dbuf[n] = 0;
			colon = strchr(dbuf, ':');
			if(colon == nil)
				continue;
			maj = strtoul(dbuf, nil, 10);
			min = strtoul(colon + 1, nil, 10);
			/* Linux dev_t encoding, as for input nodes */
			n = sys0(LklNrMknodat, LklAtFdcwd, (long)npath, 0060600,
			     ((maj & 0xfff) << 8) | (min & 0xff) |
			     ((min & ~0xff) << 12), 0, 0);
			lkl_printf("lkl9p: mknod %s (%ud:%ud) -> %ld\n",
				   npath, maj, min, n);
		}
		if(!missing)
			break;
		sleep(500);
	}
	/* Read-only mount of Bengal firmware partitions when present. */
	{
		long pfd = lklopenat(LklAtFdcwd, "/dev/mmcblk0p31", LklOrdonly, 0);
		if(pfd >= 0){
			long r;

			lklclose(pfd);
			lklmkdirat(LklAtFdcwd, "/fw", 0555);
			lklmkdirat(LklAtFdcwd, "/fw/modem", 0555);
			lklmkdirat(LklAtFdcwd, "/fw/bt", 0555);
			lklmkdirat(LklAtFdcwd, "/fw/dsp", 0555);
			lklmkdirat(LklAtFdcwd, "/fw/persist", 0555);
			/* Find and mount persist strictly read-only (MS_RDONLY=1) for Motorola HOB/DHOB/SHOB */
			for(i = 1; i <= 77; i++){
				char upath[96], ubuf[256], dpath[64];
				long ufd, un;
				snprint(upath, sizeof upath, "/sys/block/mmcblk0/mmcblk0p%d/uevent", i);
				ufd = lklopenat(LklAtFdcwd, upath, LklOrdonly, 0);
				if(ufd < 0)
					continue;
				un = sys0(LklNrRead, ufd, (long)ubuf, sizeof(ubuf) - 1, 0, 0, 0);
				lklclose(ufd);
				if(un <= 0)
					continue;
				ubuf[un] = 0;
				if(strstr(ubuf, "PARTNAME=persist\n") != nil){
					snprint(dpath, sizeof dpath, "/dev/mmcblk0p%d", i);
					r = sys0(LklNrMount, (long)dpath, (long)"/fw/persist",
						 (long)"ext4", 1, 0, 0);
					lkl_printf("lkl9p: mount %s /fw/persist (RO) -> %ld\n", dpath, r);
				}else if(strstr(ubuf, "PARTNAME=super\n") != nil){
					snprint(dpath, sizeof dpath, "/dev/mmcblk0p%d", i);
					mount_super_vendor(dpath);
				}
			}
			r = sys0(LklNrMount, (long)"/dev/mmcblk0p31", (long)"/fw/modem",
				 (long)"vfat", 1, 0, 0);
			if(r < 0)
				r = sys0(LklNrMount, (long)"/dev/mmcblk0p31",
					 (long)"/fw/modem", (long)"ext4", 1, 0, 0);
			lkl_printf("lkl9p: mount /fw/modem -> %ld\n", r);
			if(r == 0){
				long ffd = lklopenat(LklAtFdcwd,
					"/sys/module/firmware_class/parameters/path",
					LklOwronly, 0);
				if(ffd >= 0){
					sys0(LklNrWrite, ffd, (long)"/fw/modem/image", 15, 0, 0, 0);
					lklclose(ffd);
					lkl_printf("lkl9p: firmware_class.path=/fw/modem/image\n");
				}
				{
					extern void qcom_qrtr_start(void);
					qcom_qrtr_start();
				}
			}
			r = sys0(LklNrMount, (long)"/dev/mmcblk0p33", (long)"/fw/bt",
				 (long)"vfat", 1, 0, 0);
			if(r < 0)
				r = sys0(LklNrMount, (long)"/dev/mmcblk0p33",
					 (long)"/fw/bt", (long)"ext4", 1, 0, 0);
			lkl_printf("lkl9p: mount /fw/bt -> %ld\n", r);
			r = sys0(LklNrMount, (long)"/dev/mmcblk0p35", (long)"/fw/dsp",
				 (long)"ext4", 1, 0, 0);
			lkl_printf("lkl9p: mount /fw/dsp -> %ld\n", r);
			/*
			 * Auto-start Modem (4080000.remoteproc) once firmware
			 * partitions are mounted and QRTR services (rmtfs +
			 * tqftpserv) are published, so WCN3990 wlan0 comes up
			 * automatically without manual intervention.
			 */
			{
				extern void qcom_qrtr_wait_ready(void);
				qcom_qrtr_wait_ready();
			}
			/* Start ADSP (ab00000) first, then MPSS (6080000). */
			{
				static char *want[] = { "ab00000", "6080000" };
				int w;
				for(w = 0; w < 2; w++){
					for(i = 0; i < 4; i++){
						char rname[64], rpath[96];
						long rfd, rn;

						snprint(rpath, sizeof rpath,
							"/sys/class/remoteproc/remoteproc%d/name", i);
						rfd = lklopenat(LklAtFdcwd, rpath, LklOrdonly, 0);
						if(rfd < 0)
							continue;
						rn = sys0(LklNrRead, rfd, (long)rname, sizeof(rname) - 1, 0, 0, 0);
						lklclose(rfd);
						if(rn <= 0)
							continue;
						rname[rn] = 0;
						if(strstr(rname, want[w]) != nil){
							snprint(rpath, sizeof rpath,
								"/sys/class/remoteproc/remoteproc%d/state", i);
							rfd = lklopenat(LklAtFdcwd, rpath, LklOwronly, 0);
							if(rfd >= 0){
								rn = sys0(LklNrWrite, rfd, (long)"start", 5, 0, 0, 0);
								lklclose(rfd);
								lkl_printf("lkl9p: auto-start %s (%s) -> %ld\n", rpath, want[w], rn);
							}
							break;
						}
					}
				}
			}
		}
	}
}

static void
mkinputnodes(void *arg)
{
	int en, tries;
	char spath[64], npath[64], dbuf[32];

	USED(arg);
	lklmkdirat(LklAtFdcwd, "/dev", 0755);
	lklmkdirat(LklAtFdcwd, "/dev/input", 0755);
	for(tries = 0; tries < 120; tries++){
		int missing = 0;

		for(en = 0; en < 4; en++){
			long sfd, n;
			unsigned int maj, min;
			char *colon;

			snprint(npath, sizeof npath, "/dev/input/event%d", en);
			sfd = lklopenat(LklAtFdcwd, npath, LklOrdonly, 0);
			if(sfd >= 0){
				lklclose(sfd);
				continue;
			}
			snprint(spath, sizeof spath,
				"/sys/class/input/event%d/dev", en);
			sfd = lklopenat(LklAtFdcwd, spath, LklOrdonly, 0);
			if(sfd < 0){
				missing = 1;
				continue;
			}
			n = sys0(LklNrRead, sfd, (long)dbuf,
				 sizeof dbuf - 1, 0, 0, 0);
			lklclose(sfd);
			if(n <= 0)
				continue;
			dbuf[n] = 0;
			colon = strchr(dbuf, ':');
			if(colon == nil)
				continue;
			maj = strtoul(dbuf, nil, 10);
			min = strtoul(colon + 1, nil, 10);
			n = sys0(LklNrMknodat, LklAtFdcwd, (long)npath, 0020600,
			     (maj << 8) | min, 0, 0);
			lkl_printf("lkl9p: mknod %s (%ud:%ud) -> %ld\n",
				   npath, maj, min, n);
		}
		if(!missing)
			break;
		sleep(500);
	}
}

/* Stage embedded firmware (util/arm64/fw/) into the guest
 * /lib/firmware so request_firmware finds it. */
static void
mkfwfiles(void)
{
	int i, j;
	char dir[256];

	for(i = 0; i < nlklfwfiles; i++){
		Lklfw *f = &lklfwfiles[i];
		long fd, off, len;

		for(j = 1; f->path[j] != 0; j++){
			if(f->path[j] == '/' && j < (int)sizeof(dir) - 1){
				memmove(dir, f->path, j);
				dir[j] = 0;
				lklmkdirat(LklAtFdcwd, dir, 0755);
			}
		}
		fd = lklopenat(LklAtFdcwd, f->path,
			LklOwronly | LklOcreat | LklOtrunc, 0644);
		if(fd < 0){
			lkl_printf("lkl9p: fw %s open failed\n", f->path);
			continue;
		}
		len = f->end - f->start;
		for(off = 0; off < len; ){
			long chunk = len - off, n;

			if(chunk > 32768)
				chunk = 32768;
			n = sys0(LklNrWrite, fd, (long)(f->start + off),
				chunk, 0, 0, 0);
			if(n <= 0)
				break;
			off += n;
		}
		lklclose(fd);
		lkl_printf("lkl9p: fw %s (%ld bytes) -> %ld\n",
			f->path, len, off);
	}
}

void
lkl9p_start(char *srvname)
{
	int pfd[2], i;
	long dfd;

	lklmkdirat(LklAtFdcwd, "/proc", 0555);
	sys0(LklNrMount, (long)"proc", (long)"/proc", (long)"proc", 0, 0, 0);
	mkfwfiles();
	/*
	 * Input event nodes for inspection (there is no devtmpfs in
	 * this kernel: CONFIG_DEVTMPFS makes LKL fault early, so nodes
	 * are made by hand from the sysfs major:minor, like ttyGS0).
	 * Input devices probe asynchronously, so retry in a thread.
	 */
	if(lklhost_spawn(mkinputnodes, nil) < 0)
		lkl_printf("lkl9p: input node thread spawn failed\n");
	if(lklhost_spawn(mkblocknodes, nil) < 0)
		lkl_printf("lkl9p: block node thread spawn failed\n");
	dfd = lklopenat(LklAtFdcwd, "/dmesg", LklOwronly | LklOcreat | LklOtrunc, 0444);
	if(dfd >= 0){
		if(lkl_dmesg_len > 0)
			sys0(LklNrWrite, dfd, (long)lkl_dmesg, lkl_dmesg_len, 0, 0, 0);
		lklclose(dfd);
	}
	dfd = lklopenat(LklAtFdcwd, "/rebootbl", LklOwronly | LklOcreat | LklOtrunc, 0644);
	if(dfd >= 0)
		lklclose(dfd);
	dfd = lklopenat(LklAtFdcwd, "/wifiscan", LklOwronly | LklOcreat | LklOtrunc, 0444);
	if(dfd >= 0)
		lklclose(dfd);
	dfd = lklopenat(LklAtFdcwd, "/wifictl", LklOwronly | LklOcreat | LklOtrunc, 0644);
	if(dfd >= 0)
		lklclose(dfd);
	dfd = lklopenat(LklAtFdcwd, "/wifistatus", LklOwronly | LklOcreat | LklOtrunc, 0444);
	if(dfd >= 0)
		lklclose(dfd);
	dfd = lklopenat(LklAtFdcwd, "/modemctl", LklOwronly | LklOcreat | LklOtrunc, 0644);
	if(dfd >= 0)
		lklclose(dfd);
	dfd = lklopenat(LklAtFdcwd, "/modemstatus", LklOwronly | LklOcreat | LklOtrunc, 0444);
	if(dfd >= 0)
		lklclose(dfd);
	dfd = lklopenat(LklAtFdcwd, "/power", LklOwronly | LklOcreat | LklOtrunc, 0444);
	if(dfd >= 0)
		lklclose(dfd);

	wq.notempty.l = &wq.lk;

	for(i = 0; i < Nworkers; i++){
		if(lklhost_spawn(worker, nil) < 0){
			lkl_printf("lkl9p: worker spawn failed\n");
			return;
		}
	}
	if(pipe(pfd) < 0){
		lkl_printf("lkl9p: pipe failed: %r\n");
		return;
	}
	if(postfd(srvname, pfd[0]) < 0){
		lkl_printf("lkl9p: postfd /srv/%s failed: %r\n", srvname);
		close(pfd[0]);
		close(pfd[1]);
		return;
	}
	close(pfd[0]);
	lkl9psrv.infd = pfd[1];
	lkl9psrv.outfd = pfd[1];
	lkl9psrv.nopipe = 1;
	if(lklhost_spawn(rebootwatch, nil) < 0)
		lkl_printf("lkl9p: reboot watcher spawn failed\n");
	lkl_printf("lkl9p: serving /srv/%s\n", srvname);
	srv(&lkl9psrv);
}
