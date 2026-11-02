/*
 * drv.c - the userspace driver kit.
 *
 * Everything a driver needs from the kernel lives in #P: the register window,
 * the interrupt grant and the DMA arena.  Everything it needs to be a service
 * lives in lib9p.  This file is the glue, so a new driver is its register
 * logic plus its files, and not another copy of this.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"

/*
 * #P/<node>/<file>, without the varargs formatter: the path is built for
 * every probe and the node name already fits.
 */
static char *
pnppath(char *dst, int ndst, char *node, char *file)
{
	int n;

	n = snprint(dst, ndst, "#P/%s/%s", node, file);
	if(n < 0 || n >= ndst)
		return nil;
	return dst;
}

/* Read a small file into buf and NUL-terminate it; -1 if it is not there. */
static int
readtext(char *path, char *buf, int nbuf)
{
	int fd, n;

	if((fd = open(path, OREAD)) < 0)
		return -1;
	n = read(fd, buf, nbuf - 1);
	close(fd);
	if(n < 0)
		return -1;
	buf[n] = 0;
	return n;
}

/* Drop the newline the kernel puts on every numeric attribute. */
static char *
trimnl(char *s)
{
	int n;

	n = strlen(s);
	while(n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
	        s[n-1] == ' ' || s[n-1] == '\t'))
		s[--n] = 0;
	return s;
}

static int
nodetext(char *node, char *file, char *buf, int nbuf)
{
	char path[160];

	if(pnppath(path, sizeof path, node, file) == nil)
		return -1;
	return readtext(path, buf, nbuf);
}

int
drvfindexact(Drv *d, char *node)
{
	char buf[256];

	memset(d, 0, sizeof *d);
	d->irq = -1;
	d->waitfd = -1;
	snprint(d->name, sizeof d->name, "%s", node);

	if(nodetext(node, "reg", buf, sizeof buf) < 0)
		return -1;
	d->base = strtoull(buf, nil, 0);
	if(nodetext(node, "size", buf, sizeof buf) < 0)
		return -1;
	d->size = strtoull(buf, nil, 0);
	if(d->size == 0)
		return -1;

	if(nodetext(node, "irq", buf, sizeof buf) >= 0)
		d->irq = atoi(buf);
	if(nodetext(node, "class", buf, sizeof buf) >= 0)
		snprint(d->klass, sizeof d->klass, "%s", trimnl(buf));
	if(nodetext(node, "compatible", buf, sizeof buf) >= 0)
		snprint(d->compat, sizeof d->compat, "%s", trimnl(buf));
	if(nodetext(node, "console", buf, sizeof buf) >= 0)
		d->console = buf[0] == '1';
	return 0;
}

/*
 * Scan #P.  class matches the node's class ("*" matches every node that has
 * registers); reg, when given, matches the node's register base instead --
 * SoC glue such as the PMU and CRU has no useful class, but exactly one node
 * owns a given physical address.  A console match wins when the caller asks
 * for it; otherwise the first match is the answer and the console device is
 * only a fallback.
 */
static int
drvscan(Drv *d, char *class, u64 *reg, int wantconsole)
{
	Drv first, console, t;
	uchar dirbuf[16384];
	char strs[512];
	Dir de;
	long i, n, m;
	int fd, havefirst, haveconsole;

	havefirst = haveconsole = 0;
	fd = open("#P", OREAD);
	if(fd < 0)
		return -1;
	n = 0;
	while(n < (long)sizeof dirbuf){
		m = read(fd, dirbuf + n, sizeof dirbuf - n);
		if(m <= 0)
			break;
		n += m;
	}
	close(fd);
	if(n <= 0)
		return -1;

	for(i = 0; i + BIT16SZ <= n; i += GBIT16(dirbuf + i) + BIT16SZ){
		if(GBIT16(dirbuf + i) <= 0 ||
		   i + GBIT16(dirbuf + i) + BIT16SZ > n)
			break;
		if(convM2D(dirbuf + i, GBIT16(dirbuf + i) + BIT16SZ,
		   &de, strs) == 0)
			break;
		if(strcmp(de.name, ".") == 0 || strcmp(de.name, "dma") == 0 ||
		   strcmp(de.name, "tee") == 0)
			continue;
		if(drvfindexact(&t, de.name) < 0)
			continue;
		if(class != nil && strcmp(class, "*") != 0 &&
		   strcmp(t.klass, class) != 0)
			continue;
		if(reg != nil && t.base != *reg)
			continue;
		if(t.console){
			console = t;
			haveconsole = 1;
		}else if(!havefirst){
			first = t;
			havefirst = 1;
		}else
			continue;
		if(!wantconsole && havefirst)
			break;
	}
	if(wantconsole && haveconsole){
		*d = console;
		return 0;
	}
	if(havefirst){
		*d = first;
		return 0;
	}
	if(haveconsole){
		*d = console;
		return 0;
	}
	return -1;
}

int
drvfind(Drv *d, char *class, int wantconsole)
{
	return drvscan(d, class, nil, wantconsole);
}

int
drvfindreg(Drv *d, u64 base)
{
	return drvscan(d, nil, &base, 0);
}

/*
 * Registers are granted on a 2 MiB boundary, so segattach the aligned window
 * and offset into it.  The kernel keeps the grant alive; the mapping is plain
 * device memory and there is nothing to flush.
 */
int
drvmap(Drv *d)
{
	u64 len;
	void *m;

	if(d->regs != nil)
		return 0;
	if(d->size == 0 || d->name[0] == 0)
		return -1;
	len = (d->size + 0x1fffff) & ~0x1fffffULL;
	m = segattach(0, d->name, 0, len);
	if(m == (void *)-1){
		d->regs = nil;
		return -1;
	}
	d->regs = (u8 *)m + (d->base & 0x1fffff);
	return 0;
}

void
drvunmap(Drv *d)
{
	/* The grant is kernel-owned; dropping the mapping is not worth the
	 * address-space surgery in a driver that runs until it is stopped. */
	d->regs = nil;
}

u32
drd(Drv *d, u32 off)
{
	return *(volatile u32 *)((u8 *)d->regs + off);
}

void
drw(Drv *d, u32 off, u32 v)
{
	*(volatile u32 *)((u8 *)d->regs + off) = v;
}

int
drvwaitfd(Drv *d)
{
	char path[160];

	if(d->waitfd >= 0)
		return d->waitfd;
	if(pnppath(path, sizeof path, d->name, "wait") == nil)
		return -1;
	/*
	 * Opened lazily, in the process that waits: a descriptor inherited
	 * across postmountsrv's fork is not valid in the child.
	 */
	d->waitfd = open(path, OREAD);
	return d->waitfd;
}

/* Block until the grant reports an interrupt.  The driver still has to
 * acknowledge the device and clear its own status. */
int
drvwait(Drv *d)
{
	char buf[32];
	int fd;

	if((fd = drvwaitfd(d)) < 0)
		return -1;
	if(read(fd, buf, sizeof buf) <= 0)
		return -1;
	return 0;
}

static u64
readnum(char *path)
{
	char buf[64];

	if(readtext(path, buf, sizeof buf) < 0)
		return 0;
	return strtoull(buf, nil, 0);
}

/*
 * Map the DMA arena once.  The physical addresses the device needs are
 * offsets into this same window; drvpa turns one back into a user address.
 */
int
drvdmamap(Drv *d)
{
	void *m;

	if(d->dmava != nil)
		return 0;
	d->dmabase = readnum("#P/dma/base");
	d->dmasize = readnum("#P/dma/size");
	if(d->dmasize == 0)
		return -1;
	m = segattach(0, "dma", 0, d->dmasize);
	if(m == (void *)-1){
		d->dmasize = 0;
		return -1;
	}
	d->dmava = m;
	return 0;
}

/*
 * Allocate from the arena.  The kernel's allocator answers with the physical
 * address; the driver gets the matching user address and the physical one it
 * must program into the device.
 */
void *
drvdma(Drv *d, u64 size, u64 align, u64 *pa)
{
	char buf[64];
	int fd, n;
	u64 p;

	if((fd = open("#P/dma/alloc", ORDWR)) < 0)
		return nil;
	snprint(buf, sizeof buf, "%llud %llud\n", size, align);
	if(write(fd, buf, strlen(buf)) != strlen(buf)){
		close(fd);
		return nil;
	}
	n = pread(fd, buf, sizeof buf - 1, 0);
	close(fd);
	if(n <= 0)
		return nil;
	buf[n] = 0;
	p = strtoull(buf, nil, 0);

	if(d->dmava == nil && drvdmamap(d) < 0)
		return nil;
	if(p < d->dmabase || p - d->dmabase >= d->dmasize)
		return nil;
	if(pa != nil)
		*pa = p;
	return (u8 *)d->dmava + (p - d->dmabase);
}

void
drvdmafree(u64 pa, u64 size)
{
	char buf[64];
	int fd;

	if((fd = open("#P/dma/free", OWRITE)) < 0)
		return;
	snprint(buf, sizeof buf, "%#llux %llud\n", pa, size);
	write(fd, buf, strlen(buf));
	close(fd);
}

int
drvdmacache(int op, u64 pa, u64 len)
{
	static char *name[] = { "clean", "inval", "cleaninval" };
	char buf[64], path[32];
	int fd, ok;

	if(op < 0 || op >= (int)nelem(name))
		return -1;
	snprint(path, sizeof path, "#P/dma/%s", name[op]);
	if((fd = open(path, OWRITE)) < 0)
		return -1;
	snprint(buf, sizeof buf, "%#llux %llud\n", pa, len);
	ok = write(fd, buf, strlen(buf)) == strlen(buf);
	close(fd);
	return ok ? 0 : -1;
}

/* A physical address inside the mapped DMA arena, or nil if it is elsewhere
 * (for example memory the kernel allocated for a caller). */
void *
drvpa(Drv *d, u64 pa)
{
	if(d->dmava == nil || pa < d->dmabase ||
	   pa - d->dmabase >= d->dmasize)
		return nil;
	return (u8 *)d->dmava + (pa - d->dmabase);
}

void
drvlog(char *fmt, ...)
{
	char buf[512];
	va_list a;

	va_start(a, fmt);
	vseprint(buf, buf + sizeof buf, fmt, a);
	va_end(a);
	print("%s\n", buf);
}

void
drvfail(char *fmt, ...)
{
	char buf[512];
	va_list a;

	va_start(a, fmt);
	vseprint(buf, buf + sizeof buf, fmt, a);
	va_end(a);
	fprint(2, "%s\n", buf);
	exits("drv");
}

/* ---------------------------------------------------------------- 9P files */

static void
srvread(Req *r)
{
	DrvFile *f;

	f = r->fid->file->aux;
	if(f == nil || f->read == nil){
		respond(r, "read not permitted");
		return;
	}
	f->read(r, f->aux);
}

static void
srvwrite(Req *r)
{
	DrvFile *f;

	f = r->fid->file->aux;
	if(f == nil || f->write == nil){
		respond(r, "write not permitted");
		return;
	}
	f->write(r, f->aux);
}

static void
srvdestroyfile(File *f)
{
	USED(f);
}

DrvFile *
drvfile(Req *r)
{
	return r->fid->file->aux;
}

DrvSrv *
drvsrv(char *name, DrvFile *files, int nfiles)
{
	DrvSrv *s;
	File *f;
	int i;

	s = mallocz(sizeof *s, 1);
	if(s == nil)
		return nil;
	snprint(s->name, sizeof s->name, "%s", name);
	s->files = files;
	s->nfiles = nfiles;

	s->tree = alloctree(nil, nil, DMDIR | 0777, srvdestroyfile);
	if(s->tree == nil){
		free(s);
		return nil;
	}
	for(i = 0; i < nfiles; i++){
		f = createfile(s->tree->root, files[i].name, nil,
		              files[i].mode, &files[i]);
		if(f == nil){
			/* alloctree owns the files it already made */
			free(s);
			return nil;
		}
	}
	s->srv.tree = s->tree;
	s->srv.read = srvread;
	s->srv.write = srvwrite;
	return s;
}

void
drvsrvpost(DrvSrv *s)
{
	postmountsrv(&s->srv, s->name, nil, MREPL | MCREATE);
}

/* ------------------------------------------------------------------ health */

DrvHealth *
drvhealthopen(char *name)
{
	USED(name);
	return drvhealth_openenv();
}

void
drvhealthreport(DrvHealth *h, int state, char *code)
{
	if(h == nil)
		return;
	if(code == nil || *code == 0)
		code = "none";
	drvhealth_report(h, state, 0, 0, 0, 0, 0, 0,
	                 DrvHealthRecommendNone, code);
}

int
drvhealthstop(DrvHealth *h, int milliseconds)
{
	if(h == nil)
		return 0;
	return drvhealth_stopwait(h, milliseconds);
}

void
drvhealthclose(DrvHealth *h)
{
	if(h != nil)
		drvhealth_close(h);
}

/*
 * Report healthy once a second until drvman asks the driver to stop.  Without
 * a health environment (no /dev/drvctl from the manager) this parks the
 * process the way every daemon does, so a driver started from rc stays up.
 */
void
drvhealthrun(char *name)
{
	DrvHealth *h;

	h = drvhealthopen(name);
	if(h == nil){
		for(;;)
			sleep(1000);
	}
	drvhealth_ready(h);
	for(;;){
		if(drvhealth_report(h, DrvHealthHealthy, 0, 0, 0, 0, 0, 0,
		                    DrvHealthRecommendNone, "none") < 0)
			break;
		if(drvhealth_stopwait(h, 1000))
			break;
	}
	drvhealthclose(h);
	exits(nil);
}
