/*
 * The only part of Inferno's kernel a Dis program actually needs:
 * the k* entry points the Sys module and the module loader call.
 *
 * Every one is a thin wrapper over Harvey's libc.  This file replaces
 * emu/port/chan.c, the dev* devices and the Inferno namespace: a Dis
 * program opens, reads and writes Harvey paths directly.
 */
#include "dat.h"
#include "fns.h"
#include "error.h"

extern	void	k9_libcfree(void*);

int
kopen(char *s, int mode)
{
	int fd;

	fd = open(s, mode);
	return fd < 0 ? -1 : fd;
}

long
kread(int fd, void *buf, long n)
{
	long r;

	r = read(fd, buf, n);
	return r < 0 ? -1 : r;
}

long
kwrite(int fd, void *buf, long n)
{
	long r;

	r = write(fd, buf, n);
	return r < 0 ? -1 : r;
}

long
kpread(int fd, void *buf, long n, vlong off)
{
	long r;

	r = pread(fd, buf, n, off);
	return r < 0 ? -1 : r;
}

long
kpwrite(int fd, void *buf, long n, vlong off)
{
	long r;

	r = pwrite(fd, buf, n, off);
	return r < 0 ? -1 : r;
}

vlong
kseek(int fd, vlong off, int start)
{
	return seek(fd, off, start);
}

int
kclose(int fd)
{
	return close(fd);
}

int
kcreate(char *s, int mode, ulong perm)
{
	int fd;

	fd = create(s, mode, perm);
	return fd < 0 ? -1 : fd;
}

int
kremove(char *s)
{
	return remove(s);
}

/*
 * Inferno code frees the Dir it gets from kdirstat/kdirfstat/kdirread
 * with emu's free.  libc's Dir and its strings belong to libc, so copy
 * them into one emu block (structs followed by their strings).
 */
static Dir *
dirdup(Dir *src, int n)
{
	Dir *d;
	char *p;
	usize size;
	int i;

	if(n <= 0 || src == nil)
		return nil;
	size = (usize)n * sizeof(Dir);
	for(i = 0; i < n; i++)
		size += strlen(src[i].name) + 1
		      + strlen(src[i].uid) + 1
		      + strlen(src[i].gid) + 1
		      + strlen(src[i].muid) + 1;
	d = emu_malloc(size);
	if(d == nil)
		return nil;
	memmove(d, src, (usize)n * sizeof(Dir));
	p = (char *)(d + n);
	for(i = 0; i < n; i++){
		#define DUP(f) do { \
			usize l = strlen(src[i].f) + 1; \
			memmove(p, src[i].f, l); d[i].f = p; p += l; \
		} while(0)
		DUP(name); DUP(uid); DUP(gid); DUP(muid);
		#undef DUP
	}
	return d;
}

/*
 * Inferno marshals a Dir in the same 9P2000 layout Plan 9 uses, so
 * convD2M/convM2D do the work.
 */
int
kstat(char *s, uchar *buf, int n)
{
	Dir *d;
	int r;

	if((d = dirstat(s)) == nil)
		return -1;
	r = convD2M(d, buf, n);
	/* dirstat's Dir comes from libc's allocator; leaking one per stat
	 * is cheaper than mixing the two allocators. */
	if(r <= BIT16SZ)
		return -1;
	return r;
}

int
kfstat(int fd, uchar *buf, int n)
{
	Dir *d;
	int r;

	if((d = dirfstat(fd)) == nil)
		return -1;
	r = convD2M(d, buf, n);
	if(r <= BIT16SZ)
		return -1;
	return r;
}

Dir *
kdirstat(char *s)
{
	Dir *d, *e;

	if((d = dirstat(s)) == nil)
		return nil;
	e = dirdup(d, 1);
	k9_libcfree(d);
	return e;
}

Dir *
kdirfstat(int fd)
{
	Dir *d, *e;

	if((d = dirfstat(fd)) == nil)
		return nil;
	e = dirdup(d, 1);
	k9_libcfree(d);
	return e;
}

int
kwstat(char *s, uchar *buf, int n)
{
	Dir d;

	if(convM2D(buf, n, &d, nil) <= 0)
		return -1;
	return dirwstat(s, &d);
}

int
kdirwstat(char *s, Dir *d)
{
	return dirwstat(s, d);
}

int
kdirfwstat(int fd, Dir *d)
{
	return dirfwstat(fd, d);
}

long
kdirread(int fd, Dir **d)
{
	Dir *l;
	int n;

	n = dirread(fd, &l);
	if(n <= 0){
		*d = nil;
		return n;
	}
	*d = dirdup(l, n);
	k9_libcfree(l);
	if(*d == nil)
		return -1;
	return n;
}

int
kpipe(int *fd)
{
	return pipe(fd);
}

int
kdup(int old, int new)
{
	return dup(old, new);
}

int
kmount(int fd, int afd, char *old, int flag, char *spec)
{
	return mount(fd, afd, old, flag, spec, 0);
}

int
kunmount(char *old, char *spec)
{
	return unmount(old, spec);
}

int
kfd2path(int fd, char *buf, int n)
{
	return fd2path(fd, buf, n);
}

int
kiounit(int fd)
{
	return iounit(fd);
}

int
kfversion(int fd, int msize, char *vers, int *n)
{
	int r;

	r = fversion(fd, msize, vers, n == nil ? 0 : *n);
	if(r > 0 && n != nil)
		*n = r;
	return r;
}

int
kfauth(int fd, char *aname, int n)
{
	USED(n);
	return fauth(fd, aname);
}

void
kwerrstr(char *fmt, ...)
{
	char buf[ERRMAX];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf + sizeof buf, fmt, arg);
	va_end(arg);
	werrstr("%s", buf);
}

int
incref(Ref *r)
{
	return ++r->ref;
}

int
decref(Ref *r)
{
	return --r->ref;
}

/*
 * fd groups: the Sys module only refcounts these; the descriptors
 * themselves are plain ints in the Dis FD object.
 */
Fgrp *
newfgrp(Fgrp *old)
{
	Fgrp *f;

	USED(old);
	f = mallocz(sizeof *f, 1);
	if(f != nil)
		f->r.ref = 1;
	return f;
}

void
closefgrp(Fgrp *f)
{
	if(f == nil || decref(&f->r) != 0)
		return;
	free(f);
}

Pgrp *
newpgrp(void)
{
	Pgrp *p;

	p = mallocz(sizeof *p, 1);
	if(p != nil)
		p->r.ref = 1;
	return p;
}

void
closepgrp(Pgrp *p)
{
	if(p == nil || decref(&p->r) != 0)
		return;
	free(p);
}

Egrp *
newegrp(void)
{
	Egrp *e;

	e = mallocz(sizeof *e, 1);
	if(e != nil)
		e->r.ref = 1;
	return e;
}

void
closeegrp(Egrp *e)
{
	if(e == nil || decref(&e->r) != 0)
		return;
	free(e);
}

int
kchdir(char *path)
{
	return chdir(path);
}

int
kdial(char *dest, char *local, char *dir, int *cfdp)
{
	USED(dest); USED(local); USED(dir); USED(cfdp);
	return -1;
}

int
kannounce(char *addr, char *dir)
{
	USED(addr); USED(dir);
	return -1;
}

int
klisten(char *dir, char *newdir)
{
	USED(dir); USED(newdir);
	return -1;
}

void
kstrcpy(char *s, char *t, int ns)
{
	if(ns <= 0)
		return;
	strncpy(s, t, (usize)ns - 1);
	s[ns-1] = 0;
}

void
kstrdup(char **p, char *s)
{
	char *t;
	usize n;

	*p = nil;
	if(s == nil)
		return;
	n = strlen(s) + 1;
	t = emu_malloc(n);
	if(t == nil)
		error("out of memory");
	memmove(t, s, n);
	*p = t;
}

int
kfgrpclose(Fgrp *f, int fd)
{
	USED(f);
	return close(fd);
}
