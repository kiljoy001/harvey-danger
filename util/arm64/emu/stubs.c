/*
 * The parts of the Inferno runtime that a headless Dis command does not
 * need yet: the host process API (spawn/pexit), the 9P namespace calls
 * libinterp still references, dynamic loading, crypto and libm.
 *
 * They exist so the runner links and runs simple modules; each is a
 * documented no-op rather than a surprise at link time.  Real
 * implementations arrive with Sys->spawn, dial and the crypto modules.
 */
#include "dat.h"
#include "fns.h"
#include "error.h"
#include "interp.h"
#include "kernel.h"

/* ---- errors ---- */

void
error(char *err)
{
	if(err != up->env->errstr && up->env->errstr != nil)
		kstrcpy(up->env->errstr, err, ERRMAX);
	nexterror();
}

void
errorf(char *fmt, ...)
{
	char buf[ERRMAX];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf + sizeof buf, fmt, arg);
	va_end(arg);
	error(buf);
}

void
exhausted(char *resource)
{
	char buf[64];

	snprint(buf, sizeof buf, "out of %s", resource);
	error(buf);
}

void
panic(char *fmt, ...)
{
	char buf[256];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf + sizeof buf, fmt, arg);
	va_end(arg);
	fprint(2, "disvm: panic: %s\n", buf);
	exits("panic");
}

/* ---- host process API ---- */

long
osmillisec(void)
{
	return (long)(nsec() / 1000000ULL);
}

vlong
osusectime(void)
{
	return (vlong)(nsec() / 1000ULL);
}

int
osmillisleep(ulong ms)
{
	if(ms > 0)
		sleep((int)ms);
	return 0;
}

void
osyield(void)
{
	sleep(0);
}

void
oslopri(void)
{
}

void
osblock(void)
{
}

void
osready(Proc *p)
{
	USED(p);
}

void
oshostintr(Proc *p)
{
	USED(p);
}

void
oslongjmp(void *regs, osjmpbuf env, int val)
{
	USED(regs);
	longjmp(env, val);
}

void
kproc(char *name, void (*fn)(void *), void *arg, int flags)
{
	USED(name);
	USED(flags);
	fn(arg);
}

void
pexit(char *msg, int freep)
{
	USED(msg);
	USED(freep);
	exits("pexit");
}

void
cleanexit(int x)
{
	USED(x);
	exits(nil);
}

void
setid(char *name, int owner)
{
	USED(name);
	USED(owner);
}

int
kbind(char *name, char *old, int flag)
{
	USED(name);
	USED(old);
	USED(flag);
	return bind(name, old, flag);
}

/* ---- Inferno namespace bits libinterp still mentions ---- */

Chan *
namec(char *name, int how, int perm, ulong mode)
{
	USED(name); USED(how); USED(perm); USED(mode);
	error("inferno namespace not built");
	return nil;
}

void
cclose(Chan *c)
{
	USED(c);
}

Chan *
fdtochan(Fgrp *f, int fd, int mode, int chk, int x)
{
	USED(f); USED(fd); USED(mode); USED(chk); USED(x);
	error("inferno channels not built");
	return nil;
}

/* ---- dynamic loading (dlm-Plan9 without the kernel dynld) ---- */

void
newdyndata(Modlink *ml)
{
	USED(ml);
}

void
freedyndata(Modlink *ml)
{
	USED(ml);
}

Module *
newdyncode(int type, char *path, Dir *d)
{
	USED(type); USED(path); USED(d);
	return nil;
}

void
freedyncode(Module *m)
{
	USED(m);
}

int
dynldable(int type)
{
	USED(type);
	return 0;
}

int
compile(Module *m, int type, Modlink *ml)
{
	USED(m); USED(type); USED(ml);
	return -1;
}

void
modinit(void)
{
}

WORD
modstatus(REG *r, char *msg, int n)
{
	USED(r); USED(msg); USED(n);
	return 0;
}

/* ---- libm ---- */

double	__ieee754_acos(double x) { USED(x); return 0; }
double	__ieee754_acosh(double x) { USED(x); return 0; }
double	__ieee754_asin(double x) { USED(x); return 0; }
double	__ieee754_atanh(double x) { USED(x); return 0; }
double	__ieee754_atan2(double x, double y) { USED(x); USED(y); return 0; }
double	__ieee754_cosh(double x) { USED(x); return 0; }
double	__ieee754_exp(double x) { USED(x); return 0; }
double	__ieee754_fmod(double x, double y) { USED(x); USED(y); return 0; }
double	__ieee754_hypot(double x, double y) { USED(x); USED(y); return 0; }
double	__ieee754_j0(double x) { USED(x); return 0; }
double	__ieee754_j1(double x) { USED(x); return 0; }
double	__ieee754_jn(int n, double x) { USED(n); USED(x); return 0; }
double	__ieee754_lgamma_r(double x, int *s) { USED(x); if(s) *s = 1; return 0; }
double	__ieee754_log(double x) { USED(x); return 0; }
double	__ieee754_log10(double x) { USED(x); return 0; }
double	__ieee754_pow(double x, double y) { USED(x); USED(y); return 0; }
double	__ieee754_remainder(double x, double y) { USED(x); USED(y); return 0; }
double	__ieee754_sinh(double x) { USED(x); return 0; }
double	__ieee754_sqrt(double x) { USED(x); return 0; }
double	__ieee754_y0(double x) { USED(x); return 0; }
double	__ieee754_y1(double x) { USED(x); return 0; }
double	__ieee754_yn(int n, double x) { USED(n); USED(x); return 0; }
double	asinh(double x) { USED(x); return 0; }
double	cbrt(double x) { USED(x); return 0; }
double	copysign(double x, double y) { USED(x); USED(y); return 0; }
double	erf(double x) { USED(x); return 0; }
double	erfc(double x) { USED(x); return 0; }
double	expm1(double x) { USED(x); return 0; }
double	fdim(double x, double y) { USED(x); USED(y); return 0; }
double	finite(double x) { USED(x); return 1; }
double	fmax(double x, double y) { USED(x); USED(y); return 0; }
double	fmin(double x, double y) { USED(x); USED(y); return 0; }
int	gfltconv(Fmt *f) { USED(f); return 0; }
int	ilogb(double x) { USED(x); return 0; }
double	log1p(double x) { USED(x); return 0; }
double	nextafter(double x, double y) { USED(x); USED(y); return 0; }
double	rint(double x) { USED(x); return 0; }
double	scalbn(double x, int n) { USED(x); USED(n); return 0; }
double	ipow10(int n) { USED(n); return 1; }
double	dot(int n, double *x, double *y) { USED(n); USED(x); USED(y); return 0; }
void	gemm(int a, int b, int c, int d, int e, double f,
             double *g, int h, double *i, int j, double k, double *l, int m)
{
	USED(a); USED(b); USED(c); USED(d); USED(e); USED(f);
	USED(g); USED(h); USED(i); USED(j); USED(k); USED(l); USED(m);
}
double	norm1(int n, double *x) { USED(n); USED(x); return 0; }
double	norm2(int n, double *x) { USED(n); USED(x); return 0; }
int	iamax(int n, double *x) { USED(n); USED(x); return 0; }
ulong	FPcontrol(ulong set, ulong mask) { USED(set); USED(mask); return 0; }
ulong	FPstatus(ulong set, ulong mask) { USED(set); USED(mask); return 0; }
ulong	getFPstatus(void) { return 0; }
ulong	getFPcontrol(void) { return 0; }
char	*g_fmt(char *b, double x, int n) { USED(x); USED(n); if(b) b[0]=0; return b; }

void
nexterror(void)
{
	oslongjmp(nil, up->estack[--up->nerr], 1);
}

int
limbosleep(ulong ms)
{
	if(ms > 0)
		sleep((int)ms);
	return 0;
}

void
dbgexit(Prog *p, int n, char *msg)
{
	USED(p); USED(n); USED(msg);
	exits("dbgexit");
}

void	(*comvec)(void);

int
mustbesigned(char *path, uchar *sig, ulong nsig, Dir *d)
{
	USED(path); USED(sig); USED(nsig); USED(d);
	return 0;
}

int
verifysigner(uchar *msg, int nmsg, uchar *sig, ulong nsig)
{
	USED(msg); USED(nmsg); USED(sig); USED(nsig);
	return 0;
}

void
closesigs(Skeyset *s)
{
	USED(s);
}

int
export(int fd, char *dir, int auth)
{
	USED(fd); USED(dir); USED(auth);
	return -1;
}

int
srvf2c(char *dir, char *file, void *io)
{
	USED(dir); USED(file); USED(io);
	return -1;
}

Fgrp *
dupfgrp(Fgrp *f)
{
	USED(f);
	return nil;
}

void
pgrpcpy(Pgrp *to, Pgrp *from)
{
	USED(to); USED(from);
}

Chan *
cclone(Chan *c)
{
	USED(c);
	return nil;
}

Cname *
newcname(char *s)
{
	USED(s);
	return nil;
}

void
cnameclose(Cname *n)
{
	USED(n);
}

/* The Inferno namespace has no devices here. */
Dev	*devtab[] = { nil };

/* ---- crypto (Sys->dial/ssl and the crypt module need a real lib) ---- */

int	aesCBCencrypt(uchar *p, int n, uchar *k) { USED(p); USED(n); USED(k); return -1; }
int	aesCBCdecrypt(uchar *p, int n, uchar *k) { USED(p); USED(n); USED(k); return -1; }
int	bfCBCencrypt(uchar *p, int n, uchar *k) { USED(p); USED(n); USED(k); return -1; }
int	bfCBCdecrypt(uchar *p, int n, uchar *k) { USED(p); USED(n); USED(k); return -1; }
void	setupAESstate(void *s, uchar *k, int n) { USED(s); USED(k); USED(n); }
void	setupBFstate(void *s, uchar *k, int n) { USED(s); USED(k); USED(n); }
void	setupDESstate(void *s, uchar *k, int n) { USED(s); USED(k); USED(n); }
void	setupIDEAstate(void *s, uchar *k, uchar *iv) { USED(s); USED(k); USED(iv); }
void	setupRC4state(void *s, uchar *k, int n) { USED(s); USED(k); USED(n); }
void	rc4(void *s, uchar *p, int n) { USED(s); USED(p); USED(n); }
void	rc4skip(void *s, int n) { USED(s); USED(n); }
void	rc4back(void *s, int n) { USED(s); USED(n); }
void	md4(uchar *p, ulong n, uchar *d, uchar *s) { USED(p); USED(n); USED(d); USED(s); }
void	md5(uchar *p, ulong n, uchar *d, uchar *s) { USED(p); USED(n); USED(d); USED(s); }
void	sha1(uchar *p, ulong n, uchar *d, uchar *s) { USED(p); USED(n); USED(d); USED(s); }
void	sha224(uchar *p, ulong n, uchar *d, uchar *s) { USED(p); USED(n); USED(d); USED(s); }
void	sha256(uchar *p, ulong n, uchar *d, uchar *s) { USED(p); USED(n); USED(d); USED(s); }
void	sha384(uchar *p, ulong n, uchar *d, uchar *s) { USED(p); USED(n); USED(d); USED(s); }
void	sha512(uchar *p, ulong n, uchar *d, uchar *s) { USED(p); USED(n); USED(d); USED(s); }
void	hmac_md5(uchar *p, ulong n, uchar *k, ulong kn, uchar *d, uchar *s) { USED(p); USED(n); USED(k); USED(kn); USED(d); USED(s); }
void	hmac_sha1(uchar *p, ulong n, uchar *k, ulong kn, uchar *d, uchar *s) { USED(p); USED(n); USED(k); USED(kn); USED(d); USED(s); }
void	genrandom(uchar *p, int n) { USED(p); USED(n); }
