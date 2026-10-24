#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#include "virtio10.h"

u8int
vin8(Vio *r, int p)
{
	if(r->type == Vio_port)
		return inb(r->port + p);
	return *(u8int *)(r->mem + p);
}

u16int
vin16(Vio *r, int p)
{
	if(r->type == Vio_port)
		return ins(r->port + p);
	return *(u16int *)(r->mem + p);
}

u32int
vin32(Vio *r, int p)
{
	if(r->type == Vio_port)
		return inl(r->port + p);
	return *(u32int *)(r->mem + p);
}

u64int
vin64(Vio *r, int p)
{
	if(r->type == Vio_port)
		return (u64int)inl(r->port + p) |
			((u64int)inl(r->port + p + 4) << 32);
	return *(u64int *)(r->mem + p);
}

void
vout8(Vio *r, int p, u8int v)
{
	if(r->type == Vio_port)
		outb(r->port + p, v);
	else
		*(u8int *)(r->mem + p) = v;
}

void
vout16(Vio *r, int p, u16int v)
{
	if(r->type == Vio_port)
		outs(r->port + p, v);
	else
		*(u16int *)(r->mem + p) = v;
}

void
vout32(Vio *r, int p, u32int v)
{
	if(r->type == Vio_port)
		outl(r->port + p, v);
	else
		*(u32int *)(r->mem + p) = v;
}

void
vout64(Vio *r, int p, u64int v)
{
	if(r->type == Vio_port){
		outl(r->port + p, v);
		outl(r->port + p + 4, v >> 32);
	}else
		*(u64int *)(r->mem + p) = v;
}

void
virtiounmap(Vio *r, usize sz)
{
	if(r->type == Vio_port)
		iofree(r->port);
	else if(r->mem != nil)
		vunmap(r->mem, sz);
}

Vio *
virtiomapregs(Pcidev *p, Pcicap *cap, int size, Vio *v)
{
	int bar, len;
	uvlong addr;

	if(cap == nil || cap->bar >= nelem(p->mem))
		return nil;
	bar = cap->bar;
	addr = pcicfgr32(p, PciBAR0 + 4*bar);
	len = cap->length;
	if(size <= 0)
		size = len;
	else if(len < size)
		return nil;

	if(p->mem[bar].bar & 1){
		addr = (addr & ~3ULL) + cap->offset;
		v->type = Vio_port;
		v->port = addr;
		if(ioalloc(v->port, size, 0, "virtio10") < 0)
			return nil;
	}else{
		addr = (p->mem[bar].bar & ~0xFULL) + cap->offset;
		v->type = Vio_mem;
		v->mem = vmap(addr, size);
		if(v->mem == nil)
			return nil;
	}
	return v;
}
