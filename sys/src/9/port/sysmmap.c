#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

/* Prot */
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

/* Flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANON      0x20
#define MAP_ANONYMOUS 0x20

void
sysmmap(Ar0 *ar0, ...)
{
	Proc *up = externup();
	void *addr;
	usize len;
	int prot, flags, fd;
	i64 off;
	va_list list;
	usize va;
	int sno, attr;
	Segment *s, *os;
	Chan *c = nil;
	Image *img = nil;

	va_start(list, ar0);
	addr = va_arg(list, void *);
	len = va_arg(list, usize);
	prot = va_arg(list, int);
	flags = va_arg(list, int);
	fd = va_arg(list, int);
	off = va_arg(list, i64);
	va_end(list);

	if(len == 0)
		error("zero length");

	len = BIGPGROUND(len);
	if(len == 0)
		error("length overflow");

	va = PTR2UINT(addr);
	if(va != 0 && ((va < UTZERO) || ((va & KZERO) == KZERO)))
		error("virtual address in kernel");

	/* If not anonymous, validate fd and get channel */
	if((flags & MAP_ANON) == 0 && (flags & MAP_ANONYMOUS) == 0){
		if(fd < 0)
			error(Ebadarg);
		c = fdtochan(fd, (prot & PROT_WRITE) ? ORDWR : OREAD, 0, 1);
		if(waserror()){
			cclose(c);
			nexterror();
		}
	}

	qlock(&up->seglock);
	if(waserror()){
		qunlock(&up->seglock);
		if(c != nil)
			poperror(); /* cclose */
		nexterror();
	}

	for(sno = 0; sno < NSEG; sno++)
		if(up->seg[sno] == nil)
			break;
	if(sno == NSEG)
		error("too many segments in process");

	/* Choose virtual address if not specified or not MAP_FIXED */
	if(va == 0 || (flags & MAP_FIXED) == 0){
		/* Start below the stack */
		usize top = (va != 0) ? (va & ~(BIGPGSZ - 1)) : (USTKTOP - USTKSIZE - len);
		va = top;
		for(;;){
			os = isoverlap(up, va, len);
			if(os == nil)
				break;
			va = os->base;
			if(len > va || va < UTZERO + BIGPGSZ)
				error("cannot fit segment at virtual address");
			va -= len;
		}
	} else {
		/* MAP_FIXED: must be page aligned and not overlap unless unmapped */
		va = va & ~(BIGPGSZ - 1);
		if(isoverlap(up, va, len) != nil)
			error(Esoverlap);
	}

	/* Map POSIX prot to Harvey segment type */
	attr = SG_MMAP;
	if(prot & PROT_READ)
		attr |= SG_READ;
	if(prot & PROT_WRITE)
		attr |= SG_WRITE;
	if(prot & PROT_EXEC)
		attr |= SG_EXEC;

	s = newseg(attr, va, len / BIGPGSZ);
	s->color = up->color;

	if(c != nil){
		img = attachimage(attr, c, up->color, va, len / BIGPGSZ);
		s->image = img;
		s->ldseg.pg0vaddr = va;
		s->ldseg.pg0off = 0;
		s->ldseg.pg0fileoff = off;
		s->ldseg.filesz = len;
		s->ldseg.memsz = len;
		unlock(&img->r.l);
		poperror(); /* cclose */
	}

	up->seg[sno] = s;

	poperror(); /* seglock */
	qunlock(&up->seglock);

	ar0->v = UINT2PTR(va);
}

void
sysmunmap(Ar0 *ar0, ...)
{
	Proc *up = externup();
	void *addr;
	usize len, va;
	int sno;
	Segment *s = nil;
	va_list list;

	va_start(list, ar0);
	addr = va_arg(list, void *);
	len = va_arg(list, usize);
	va_end(list);

	va = PTR2UINT(addr);
	if(len == 0 || (va & (BIGPGSZ - 1)) != 0)
		error(Ebadarg);

	qlock(&up->seglock);
	if(waserror()){
		qunlock(&up->seglock);
		nexterror();
	}

	for(sno = 0; sno < NSEG; sno++){
		s = up->seg[sno];
		if(s == nil)
			continue;
		if(s->base == va){
			/* Do not allow unmapping main text or stack segments */
			if((s->type & SG_TYPE) == SG_STACK ||
			   (s->type & SG_TYPE) == SG_TEXT)
				error(Ebadarg);

			up->seg[sno] = nil;
			putseg(s);
			mmuflush();
			break;
		}
	}

	poperror();
	qunlock(&up->seglock);

	if(sno == NSEG)
		error(Ebadarg);

	ar0->i = 0;
}

void
sysmprotect(Ar0 *ar0, ...)
{
	Proc *up = externup();
	void *addr;
	usize len, va;
	int prot;
	int sno;
	Segment *s = nil;
	va_list list;

	va_start(list, ar0);
	addr = va_arg(list, void *);
	len = va_arg(list, usize);
	prot = va_arg(list, int);
	va_end(list);

	va = PTR2UINT(addr);
	if(len == 0 || (va & (BIGPGSZ - 1)) != 0)
		error(Ebadarg);

	qlock(&up->seglock);
	if(waserror()){
		qunlock(&up->seglock);
		nexterror();
	}

	for(sno = 0; sno < NSEG; sno++){
		s = up->seg[sno];
		if(s != nil && va >= s->base && va < s->top){
			qlock(&s->lk);
			s->type &= ~(SG_READ | SG_WRITE | SG_EXEC);
			if(prot & PROT_READ)
				s->type |= SG_READ;
			if(prot & PROT_WRITE)
				s->type |= SG_WRITE;
			if(prot & PROT_EXEC)
				s->type |= SG_EXEC;
			qunlock(&s->lk);
			mmuflush();
			break;
		}
	}

	poperror();
	qunlock(&up->seglock);

	if(sno == NSEG)
		error(Ebadarg);

	ar0->i = 0;
}
