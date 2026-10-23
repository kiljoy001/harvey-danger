/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Prism user-space xHCI engine. See README.md for Linux source provenance.
 * One outstanding TD/command, one interrupter (polled), one ring segment.
 * A failed transport/timeout poisons the controller: DMA is not recycled
 * until the trusted backend has stopped/reset the hardware.
 */
#include "xhci.h"

extern int print(char *, ...);

#define T(t) ((Xword)(t)<<10)
#define BIT(n) ((Xword)1<<(n))
#define TRY(e) do { int xr = (e); if(xr < 0) return xr; } while(0)

enum { Run=1, Reset=2, Halted=1, Notready=1<<11, Changed=0x00fe0000 };

static void
zero(void *v, unsigned n)
{
	Xbyte *p = v;
	while(n--) *p++ = 0;
}

static Xword
get32(const Xbyte *p)
{
	return (Xword)p[0] | (Xword)p[1]<<8 | (Xword)p[2]<<16 | (Xword)p[3]<<24;
}

static unsigned
get16(const Xbyte *p)
{
	return p[0] | (unsigned)p[1]<<8;
}

static void
put32(Xbyte *p, Xword v)
{
	unsigned i;
	for(i=0; i<4; i++) p[i] = (Xbyte)(v>>(8*i));
}

static int
bad(Xhci *x, int result)
{
	x->failed = 1;
	return result;
}

static int
rd(Xhci *x, Xword reg, Xword *v)
{
	if(reg > x->io.mmiosize-4 || (reg&3) ||
	   x->io.read32(x->io.arg, reg, v) < 0 || *v == ~0U)
		return bad(x, Xioerr);
	return 0;
}

static int
wr(Xhci *x, Xword reg, Xword v)
{
	if(reg > x->io.mmiosize-4 || (reg&3) ||
	   x->io.write32(x->io.arg, reg, v) < 0)
		return bad(x, Xioerr);
	return 0;
}

static int
wr64(Xhci *x, Xword reg, Xaddr v)
{
	TRY(wr(x, reg, (Xword)v));
	return wr(x, reg+4, (Xword)(v>>32));
}

static int
memio(Xhci *x, Xaddr a, void *p, unsigned n, int write)
{
	int r;
	if(a < x->io.dma || n > x->io.size || a-x->io.dma > x->io.size-n)
		return bad(x, Xinvalid);
	if(write) r = x->io.writemem(x->io.arg, a, p, n);
	else r = x->io.readmem(x->io.arg, a, p, n);
	return r < 0 ? bad(x, Xioerr) : 0;
}

static int
mw32(Xhci *x, Xaddr a, Xword v)
{
	Xbyte b[4];
	put32(b, v);
	return memio(x, a, b, 4, 1);
}

static int
mw64(Xhci *x, Xaddr a, Xaddr v)
{
	TRY(mw32(x, a, (Xword)v));
	return mw32(x, a+4, (Xword)(v>>32));
}

static int
clear(Xhci *x, Xaddr a, unsigned n)
{
	Xbyte b[256];
	unsigned k;
	zero(b, sizeof b);
	while(n){
		k = n > sizeof b ? sizeof b : n;
		TRY(memio(x, a, b, k, 1));
		a += k;
		n -= k;
	}
	return 0;
}

static int
allocdma(Xhci *x, unsigned n, unsigned align, Xaddr *a)
{
	Xword off = (x->used+align-1)&~(align-1);
	if(n > x->io.size || off > x->io.size-n) return Xnomem;
	*a = x->io.dma+off;
	x->used = off+n;
	return clear(x, *a, n);
}

static int
waitreg(Xhci *x, Xword reg, Xword mask, Xword value)
{
	unsigned i;
	Xword v;
	for(i=0; i<2000; i++){
		TRY(rd(x, reg, &v));
		if((v&mask) == value) return 0;
		x->io.delay(x->io.arg, 1);
	}
	print("xhci: timeout reg %x mask %x want %x got %x\n",
	      reg, mask, value, v);
	return bad(x, Xtimeout);
}

static int
ringinit(Xhci *x, Xring *r)
{
	r->next = 0;
	r->cycle = 1;
	if(!r->base) TRY(allocdma(x, Xringlen*16, 64, &r->base));
	else TRY(clear(x, r->base, Xringlen*16));
	TRY(mw64(x, r->base+(Xringlen-1)*16, r->base));
	return mw32(x, r->base+(Xringlen-1)*16+12, T(6)|BIT(1));
}

/* The first TRB in a TD can be withheld until all later TRBs are visible. */
static int
queue(Xhci *x, Xring *r, Xaddr parameter, Xword status, Xword control,
      int withhold, Xaddr *address, Xword *publish)
{
	Xbyte b[16];
	*address = r->base+r->next*16;
	*publish = control|r->cycle;
	put32(b, (Xword)parameter);
	put32(b+4, (Xword)(parameter>>32));
	put32(b+8, status);
	put32(b+12, *publish^(withhold?1:0));
	TRY(memio(x, *address, b, 16, 1));
	if(++r->next == Xringlen-1){
		TRY(mw32(x, r->base+r->next*16+12, T(6)|BIT(1)|r->cycle));
		r->next = 0;
		r->cycle ^= 1;
	}
	return 0;
}

static int
event(Xhci *x, Xword v[4])
{
	Xbyte b[16];
	unsigned i;
	TRY(memio(x, x->events+x->eventnext*16, b, 16, 0));
	for(i=0; i<4; i++) v[i] = get32(b+i*4);
	if((v[3]&1) != x->eventcycle) return 0;
	if(++x->eventnext == Xevents){ x->eventnext=0; x->eventcycle^=1; }
	TRY(wr64(x, x->runtime+0x38, (x->events+x->eventnext*16)|8));
	return 1;
}

static int
completion(Xhci *x, unsigned type, unsigned slot, unsigned ep,
           Xaddr last, Xaddr first, Xaddr data, unsigned length, unsigned *outslot)
{
	Xword v[4], cc, kind;
	Xaddr p;
	unsigned i, count=0, actual=length;
	int r;
	for(i=0; i<2000; ){
		r = event(x, v);
		if(r < 0) return r;
		if(r == 0){ x->io.delay(x->io.arg, 1); i++; continue; }
		if(++count > 4096) return bad(x, Xioerr);
		kind = (v[3]>>10)&63;
		if(kind == 34) continue; /* port changes are observed through PORTSC */
		if(kind != type) return bad(x, Xioerr);
		p = (Xaddr)v[0]|(Xaddr)v[1]<<32;
		if((p != last && p != first && p != data) || (v[3]&BIT(2)))
			return bad(x, Xioerr);
		if(type == 32 && ((v[3]>>24) != slot || ((v[3]>>16)&31) != ep))
			return bad(x, Xioerr);
		cc = v[2]>>24;
		x->completion = cc;
		if(outslot) *outslot = v[3]>>24;
		if(cc == 6) return Xstall;
		if(cc != 1 && cc != 13) return Xioerr;
		if(type == 32 && p == data){
			if((v[2]&0xffffff) > length) return bad(x, Xioerr);
			actual = length-(v[2]&0xffffff);
		}
		if(p == last) return type == 32 ? (int)actual : 0;
		if(cc != 13 || p != data) return bad(x, Xioerr);
	}
	print("xhci: completion timeout type %u slot %u ep %u\n", type, slot, ep);
	return bad(x, Xtimeout);
}

static int
command(Xhci *x, unsigned type, unsigned slot, unsigned flags, Xaddr p, unsigned *out)
{
	Xaddr a;
	Xword c;
	int r;
	if(x->failed || !x->running) return Xioerr;
	print("xhci: command %u slot %u\n", type, slot);
	TRY(queue(x, &x->command, p, 0, T(type)|((Xword)slot<<24)|flags, 0, &a, &c));
	TRY(wr(x, x->doorbell, 0));
	r = completion(x, 33, 0, 0, a, a, a, 0, out);
	print("xhci: command %u result %d\n", type, r);
	return r;
}

int
xhci_noop(Xhci *x)
{
	return command(x, 23, 0, 0, 0, 0);
}

static int
protocolcap(Xhci *x,Xword off,Xword cap)
{
	Xword name,ports,type;
	unsigned start,n,j;
			TRY(rd(x, off+4, &name));
			TRY(rd(x, off+8, &ports));
			TRY(rd(x, off+12, &type));
			start = ports&255; n = (ports>>8)&255;
			if(name != 0x20425355 || !start || !n || start+n-1 > x->ports)
				return Xunsupported;
			/* Custom protocol speed IDs require a PSI decoder. Fail explicitly. */
			if(ports>>28) return Xunsupported;
			for(j=start; j<start+n; j++){
				x->protocol[j] = cap>>24;
				x->slottype[j] = type&31;
			}
	return 0;
}
static int
extcaps(Xhci *x, Xword hcc)
{
	Xword off = (hcc>>16)*4, cap, name;
	unsigned i;
	for(i=0; off && i<256; i++){
		TRY(rd(x, off, &cap));
		if((cap&255) == 1){
			TRY(wr(x, off, cap|BIT(24)));
			TRY(waitreg(x, off, BIT(16), 0));
			/* Disable legacy SMI enables and acknowledge their RW1C status. */
			TRY(rd(x, off+4, &name));
			TRY(wr(x, off+4, (name&~0x0000e011U)|0xe0000000U));
		}
		if((cap&255)==2) TRY(protocolcap(x,off,cap));
		if(((cap>>8)&255) == 0) return 0;
		off += ((cap>>8)&255)*4;
	}
	return off ? Xunsupported : 0;
}

static int
capabilities(Xhci *x, Xword *hccout)
{
	Xword cap,hcs,hcc,v;
	TRY(rd(x, 0, &cap));
	if((cap>>16) < 0x100 || (cap&255) < 0x20 || (cap&3)) return Xunsupported;
	x->op = cap&255;
	TRY(rd(x, 4, &hcs));
	x->slots = (hcs&255) > Xslots ? Xslots : hcs&255;
	x->ports = hcs>>24;
	if(!x->slots || !x->ports) return Xunsupported;
	TRY(rd(x, 0x10, &hcc));
	*hccout=hcc;
	if(!(hcc&1) && x->io.dma+x->io.size > 0x100000000ULL) return Xunsupported;
	x->ctxsize = (hcc&4) ? 64 : 32;
	TRY(rd(x, 0x14, &v)); x->doorbell = v&~3U;
	TRY(rd(x, 0x18, &v)); x->runtime = v&~31U;
	if(x->doorbell > x->io.mmiosize-(x->slots+1)*4 ||
	   x->runtime > x->io.mmiosize-0x40 ||
	   x->op+0x400+x->ports*16 > x->io.mmiosize) return Xinvalid;
	return 0;
}
static int
resetcontroller(Xhci *x, Xword hcc)
{
	Xword v;
	TRY(extcaps(x, hcc));
	TRY(wr(x, x->op, 0));
	TRY(waitreg(x, x->op+4, Halted, Halted));
	TRY(wr(x, x->op, Reset));
	TRY(waitreg(x, x->op, Reset, 0));
	TRY(waitreg(x, x->op+4, Notready, 0));
	TRY(rd(x, x->op+8, &v));
	if(!(v&1)) return Xunsupported;
	return 0;
}
static int
allocrings(Xhci *x)
{
	TRY(allocdma(x, (x->slots+1)*8, 64, &x->dcbaa));
	TRY(ringinit(x, &x->command));
	TRY(allocdma(x, Xevents*16, 64, &x->events));
	TRY(allocdma(x, 16, 64, &x->erst));
	TRY(mw64(x, x->erst, x->events));
	TRY(mw32(x, x->erst+8, Xevents));
	return 0;
}
static int
scratchpads(Xhci *x)
{
	Xword v,scratch;
	Xaddr array,page;
	unsigned i;
	TRY(rd(x, 8, &v));
	scratch = ((v>>27)&31) | ((v>>16)&0x3e0);
	if(scratch){
		TRY(allocdma(x, scratch*8, 64, &array));
		for(i=0; i<scratch; i++){
			TRY(allocdma(x, 4096, 4096, &page));
			TRY(mw64(x, array+i*8, page));
		}
		TRY(mw64(x, x->dcbaa, array));
	}
	return 0;
}
static int
startcontroller(Xhci *x)
{
	print("xhci: starting op %x runtime %x doorbell %x\n",
	      x->op, x->runtime, x->doorbell);
	x->eventcycle = 1;
	TRY(wr64(x, x->op+0x30, x->dcbaa));
	TRY(wr64(x, x->op+0x18, x->command.base|1));
	TRY(wr(x, x->op+0x38, x->slots));
	TRY(wr(x, x->runtime+0x20, 0)); /* interrupter disabled; poll events */
	TRY(wr(x, x->runtime+0x28, 1));
	TRY(wr64(x, x->runtime+0x30, x->erst));
	TRY(wr64(x, x->runtime+0x38, x->events));
	TRY(wr(x, x->op, Run));
	TRY(waitreg(x, x->op+4, Halted, 0));
	x->running = 1;
	print("xhci: started\n");
	return 0;
}
static int
powerports(Xhci *x)
{
	Xword v;
	unsigned i;
	for(i=1; i<=x->ports; i++){
		TRY(rd(x, x->op+0x400+(i-1)*16, &v));
		TRY(wr(x, x->op+0x400+(i-1)*16, (v&0x0e00c3e0U)|BIT(9)));
	}
	return 0;
}
int
xhci_init(Xhci *x, const Xio *io)
{
	Xword hcc;
	if(!x || !io) return Xinvalid;
	if(!io->read32 || !io->write32 || !io->readmem || !io->writemem || !io->delay)
		return Xinvalid;
	if(io->mmiosize < 0x1000 || io->size < Xarena || (io->dma&65535) ||
	   io->dma+io->size < io->dma) return Xinvalid;
	zero(x,sizeof *x);
	x->io=*io;
	TRY(capabilities(x,&hcc));
	TRY(resetcontroller(x,hcc));
	TRY(allocrings(x));
	TRY(scratchpads(x));
	TRY(startcontroller(x));
	TRY(powerports(x));
	{
		unsigned i, j;
		Xword v;
		/* DWC3 needs time after HCRST/port power before CCS appears. */
		for(i=1; i<=x->ports; i++)
			for(j=0; j<100; j++){
				xhci_port(x, i, &v);
				if(v&1) break;
				x->io.delay(x->io.arg, 10);
			}
	}
	return xhci_noop(x);
}

int
xhci_stop(Xhci *x)
{
	int r;
	if(!x || !x->io.write32) return Xinvalid;
	x->running = 0;
	x->failed = 1;
	r = wr(x, x->op, 0);
	if(r < 0) return r;
	return waitreg(x, x->op+4, Halted, Halted);
}

int
xhci_port(Xhci *x, unsigned port, Xword *status)
{
	if(!port || port>x->ports || !status || x->failed) return Xinvalid;
	return rd(x, x->op+0x400+(port-1)*16, status);
}

static int
validdev(Xhci *x, unsigned slot)
{
	return !x->failed && x->running && slot && slot<=x->slots && x->device[slot].slot;
}

static int
inputslot(Xhci *x, Xdevice *d)
{
	TRY(clear(x, d->input, x->ctxsize*33));
	TRY(mw32(x, d->input+x->ctxsize, (d->maxep<<27)|(d->speed<<20)));
	return mw32(x, d->input+x->ctxsize+4, d->port<<16);
}

static int
inputep(Xhci *x, Xdevice *d, unsigned ep, unsigned type, unsigned packet,
        unsigned interval, unsigned burst, unsigned payload)
{
	Xaddr a = d->input+(ep+1)*x->ctxsize;
	TRY(mw32(x, a, interval<<16));
	TRY(mw32(x, a+4, (packet<<16)|(burst<<8)|(type<<3)|(3<<1)));
	TRY(mw64(x, a+8, d->ep[ep].base|1));
	return mw32(x, a+16, (payload<<16)|(type==4 ? 8 : packet));
}

static int
addressdevice(Xhci *x, Xdevice *d, unsigned slot)
{
	int r;
	if(!d->context){
		r=allocdma(x, x->ctxsize*32, 64, &d->context);
		if(r<0) return r;
	}
	if(!d->input){
		r=allocdma(x, x->ctxsize*33, 64, &d->input);
		if(r<0) return r;
	}
	if(!d->buffer){
		r=allocdma(x, Xmaxdata, 65536, &d->buffer);
		if(r<0) return r;
	}
	r=clear(x, d->context, x->ctxsize*32);
	if(r<0) return r;
	r=ringinit(x, &d->ep[1]); if(r<0) return r;
	r=inputslot(x, d); if(r<0) return r;
	r=mw32(x, d->input+4, 3); if(r<0) return r;
	r=inputep(x, d, 1, 4, d->maxpkt, 0, 0, 0); if(r<0) return r;
	r=mw64(x, x->dcbaa+slot*8, d->context); if(r<0) return r;
	r=command(x, 11, slot, 0, d->input, 0); if(r<0) return r;
	return 0;
}

static int
resetport(Xhci *x,unsigned port,unsigned *speed)
{
	Xword v,reg;
	TRY(xhci_port(x, port, &v));
	if(!(v&1)) return Xgone;
	reg = x->op+0x400+(port-1)*16;
	/* USB3 links normally reach enabled/U0 without a reset; USB2 needs PR. */
	if(!(v&2)){
		TRY(wr(x, reg, (v&0x0e00c3e0U)|BIT(9)|
		       (x->protocol[port]==3 ? BIT(31) : BIT(4))));
		TRY(waitreg(x, reg, BIT(4)|BIT(31), 0));
		TRY(waitreg(x, reg, 3, 3));
	}
	TRY(rd(x, reg, &v));
	TRY(wr(x, reg, (v&0x0e00c3e0U)|(v&Changed)));
	*speed = (v>>10)&15;
	if(*speed<1 || *speed>4) return Xunsupported;
	return 0;
}

int
xhci_attach(Xhci *x, unsigned port)
{
	unsigned slot=0, speed, i;
	Xdevice *d;
	int r;
	if(x->failed || !x->running || !port || port>x->ports) return Xinvalid;
	for(i=1; i<=x->slots; i++)
		if(x->device[i].slot && x->device[i].port==port) return Xinvalid;
	TRY(resetport(x,port,&speed));
	TRY(command(x, 9, 0, x->slottype[port]<<16, 0, &slot));
	if(!slot || slot>x->slots || x->device[slot].slot) return bad(x, Xioerr);
	d = &x->device[slot];
	d->slot=slot; d->port=port; d->speed=speed; d->maxep=1;
	d->maxpkt = speed==4 ? 512 : speed==3 ? 64 : 8;
	d->enabled=2; d->halted=0; d->configured=0;
	r=addressdevice(x,d,slot);
	if(r<0) goto fail;
	return (int)slot;
fail:
	if(!x->failed) xhci_detach(x, slot);
	return r;
}

int
xhci_detach(Xhci *x, unsigned slot)
{
	if(!validdev(x, slot)) return Xinvalid;
	TRY(command(x, 10, slot, 0, 0, 0));
	TRY(mw64(x, x->dcbaa+slot*8, 0));
	x->device[slot].slot=0;
	return 0;
}

static int
recover(Xhci *x, Xdevice *d, unsigned ep)
{
	Xring *r = &d->ep[ep];
	TRY(command(x, 14, d->slot, ep<<16, 0, 0));
	TRY(command(x, 16, d->slot, ep<<16, (r->base+r->next*16)|r->cycle, 0));
	d->halted &= ~BIT(ep);
	return 0;
}

static int
learnpacket(Xhci *x, Xdevice *d, unsigned slot, void *buf)
{
	/* Full-speed EP0 starts at 8; learn its actual packet size. */
	{
		unsigned packet=((Xbyte *)buf)[7];
		if(d->speed==4){ if(packet!=9) return Xinvalid; packet=512; }
		if(packet!=8 && packet!=16 && packet!=32 && packet!=64 && packet!=512)
			return Xinvalid;
		if(packet!=d->maxpkt){
			Xbyte context[64];
			TRY(clear(x, d->input, x->ctxsize*33));
			TRY(mw32(x, d->input+4, 2));
			TRY(memio(x, d->context+x->ctxsize, context, x->ctxsize, 0));
			put32(context+4, (get32(context+4)&65535)|(packet<<16));
			TRY(memio(x, d->input+2*x->ctxsize, context, x->ctxsize, 1));
			TRY(command(x, 13, slot, 0, d->input, 0));
			d->maxpkt=packet;
		}
	}
	return 0;
}

static int
controltd(Xhci *x, Xdevice *d, const Xbyte setup[8], unsigned length, unsigned in,
          Xaddr *firstout, Xaddr *dataout, Xaddr *lastout)
{
	Xaddr first,data=0,last,immediate;
	Xword c,publish,ignored;
	immediate = (Xaddr)get32(setup)|(Xaddr)get32(setup+4)<<32;
	c = T(2)|BIT(6)|(length ? ((in?3U:2U)<<16) : 0);
	TRY(queue(x, &d->ep[1], immediate, 8, c, 1, &first, &publish));
	if(length)
		TRY(queue(x, &d->ep[1], d->buffer, length,
		          T(3)|(in ? BIT(16)|BIT(2) : 0), 0, &data, &ignored));
	TRY(queue(x, &d->ep[1], 0, 0, T(4)|BIT(5)|((!length || !in)?BIT(16):0),
	          0, &last, &ignored));
	TRY(mw32(x, first+12, publish));
	TRY(wr(x, x->doorbell+d->slot*4, 1));
	*firstout=first; *dataout=data; *lastout=last;
	return 0;
}

static int
controlvalid(const Xbyte *setup,void *buf,unsigned n)
{
	unsigned length=get16(setup+6);
	if(length>n || (length && !buf)) return Xinvalid;
	if(setup[0]==0 && (setup[1]==5 || setup[1]==9)) return Xunsupported;
	if(setup[0]==1 && setup[1]==11) return Xunsupported;
	return 0;
}
static int
isdevicedesc(const Xbyte *setup,int result)
{
	return result>=8 && setup[0]==0x80 && setup[1]==6 && setup[3]==1;
}
int
xhci_control(Xhci *x, unsigned slot, const Xbyte setup[8], void *buf, unsigned n)
{
	Xdevice *d;
	Xaddr first, data=0, last;
	unsigned length, in;
	int r;
	if(!validdev(x, slot) || !setup) return Xinvalid;
	length = get16(setup+6); in = (setup[0]&128)!=0;
	/* Address Device owns SET_ADDRESS; configuration uses Configure Endpoint. */
	TRY(controlvalid(setup,buf,n));
	d=&x->device[slot];
	if(d->halted&2) TRY(recover(x, d, 1));
	if(length && !in) TRY(memio(x, d->buffer, buf, length, 1));
	TRY(controltd(x,d,setup,length,in,&first,&data,&last));
	r=completion(x, 32, slot, 1, last, first, data, length, 0);
	if(r==Xstall) d->halted |= 2;
	else if(r<0) return bad(x, r);
	if(r>=0 && length && in) TRY(memio(x, d->buffer, buf, (unsigned)r, 0));
	if(isdevicedesc(setup,r))
		TRY(learnpacket(x,d,slot,buf));
	return r;
}

/* Alt setting zero, bulk/interrupt only; SS companions are mandatory. */
typedef struct ConfigScan ConfigScan;
struct ConfigScan {
	unsigned active,seen,companion,expected,count,speed,packet,type;
};
static int
packetvalid(unsigned speed, unsigned type, unsigned packet)
{
	if(!packet || packet>1024) return Xinvalid;
	if(speed==2 && (type!=3 || packet>8)) return Xinvalid;
	if(speed==1 && packet>64) return Xinvalid;
	if(type==2 && speed==3 && packet!=512) return Xinvalid;
	if(type==2 && speed==4 && packet!=1024) return Xinvalid;
	return 0;
}
static int
scaninterface(ConfigScan *c, const Xbyte *b)
{
	if(c->active && c->expected!=c->count) return Xinvalid;
	if(b[0]<9) return Xinvalid;
	c->active=b[3]==0; c->expected=b[4]; c->count=0;
	if(c->active && b[5]==9) return Xunsupported;
	return 0;
}
static int
scanendpoint(ConfigScan *c, const Xbyte *b)
{
	unsigned ep,address;
	if(b[0]<7) return Xinvalid;
	address=b[2]; c->type=b[3]&3; c->packet=get16(b+4);
	if((address&0x70) || !(address&15)) return Xinvalid;
	ep=(address&15)*2+((address>>7)&1);
	if(c->seen&BIT(ep)) return Xinvalid;
	if(c->type!=2 && c->type!=3) return Xunsupported;
	TRY(packetvalid(c->speed,c->type,c->packet));
	if(c->type==3 && (!b[6] || (c->speed>=3 && b[6]>16))) return Xinvalid;
	c->seen|=BIT(ep); c->companion=c->speed==4; c->count++;
	return 0;
}
static int
scancompanion(ConfigScan *c, const Xbyte *b)
{
	unsigned payload;
	if(!c->companion || b[0]<6 || b[2]>15 || b[3]!=0) return Xunsupported;
	payload=get16(b+4);
	if(c->type==3 && (!payload || payload>(b[2]+1)*c->packet)) return Xinvalid;
	if(c->type==2 && payload) return Xinvalid;
	c->companion=0;
	return 0;
}
static int
scandesc(ConfigScan *c, const Xbyte *b)
{
	if(c->companion && b[1]!=48) return Xinvalid;
	if(b[1]==4) return scaninterface(c,b);
	if(!c->active) return 0;
	if(b[1]==5) return scanendpoint(c,b);
	if(b[1]==48) return scancompanion(c,b);
	return 0;
}
int
xhci_validate_config(const Xbyte *b, unsigned n, unsigned speed)
{
	unsigned off,len;
	ConfigScan c;
	if(!b || n<9) return Xinvalid;
	if(b[0]!=9 || b[1]!=2 || get16(b+2)!=n || !b[5]) return Xinvalid;
	if(speed<1 || speed>4) return Xinvalid;
	zero(&c,sizeof c); c.speed=speed;
	for(off=9; off<n; off+=len){
		if(n-off<2) return Xinvalid;
		len=b[off];
		if(len<2 || len>n-off) return Xinvalid;
		TRY(scandesc(&c,b+off));
	}
	if(c.companion || (c.active && c.expected!=c.count) || !c.seen) return Xinvalid;
	return 0;
}

static int
configureep(Xhci *x,Xdevice *d,const Xbyte *b,unsigned *enabled)
{
	unsigned ep,type,packet,interval,burst,payload;
		ep=(b[2]&15)*2+(b[2]>>7);
		type=(b[3]&3)==2 ? 2 : 3;
		packet=get16(b+4); interval=0; burst=0; payload=0;
		if(type==3){
			if(d->speed>=3) interval=b[6]-1;
			else { unsigned frames=b[6]; interval=3; while(frames>1){ frames>>=1; interval++; } }
			payload=packet;
		}
		if(d->speed==4){
			const Xbyte *ss=b+b[0];
			burst=ss[2];
			if(type==3){
				payload=get16(ss+4);
			}
		}
		type += (ep&1)?4:0;
		TRY(ringinit(x,&d->ep[ep]));
		TRY(inputep(x,d,ep,type,packet,interval,burst,payload));
		d->packet[ep]=packet; d->type[ep]=type;
		*enabled|=BIT(ep);
		if(ep>d->maxep) d->maxep=ep;
	return 0;
}
static int
setconfiguration(Xhci *x,Xdevice *d,unsigned configuration)
{
	Xbyte setup[8]={0,9,0,0,0,0,0,0};
	Xaddr first,last;
	Xword publish,ignored;
	/* SET_CONFIGURATION is issued only after all endpoint contexts succeeded. */
	setup[2]=configuration;
	TRY(queue(x,&d->ep[1],(Xaddr)get32(setup),8,T(2)|BIT(6),1,&first,&publish));
	TRY(queue(x,&d->ep[1],0,0,T(4)|BIT(5)|BIT(16),0,&last,&ignored));
	TRY(mw32(x,first+12,publish));
	TRY(wr(x,x->doorbell+d->slot*4,1));
	{
		int r=completion(x,32,d->slot,1,last,first,0,0,0);
		if(r<0) return bad(x,r); /* no partially configured device is usable */
	}
	return 0;
}
int
xhci_configure(Xhci *x, unsigned slot, const Xbyte *b, unsigned n)
{
	Xdevice *d;
	unsigned off, active=0, enabled=2;
	if(!validdev(x, slot)) return Xinvalid;
	d=&x->device[slot];
	if(d->configured) return Xinvalid;
	TRY(xhci_validate_config(b,n,d->speed));
	if(d->halted&2) TRY(recover(x,d,1));
	TRY(inputslot(x,d));
	for(off=9; off<n; off+=b[off]){
		if(b[off+1]==4) active=b[off+3]==0;
		if(b[off+1]!=5 || !active) continue;
		TRY(configureep(x,d,b+off,&enabled));
	}
	TRY(mw32(x,d->input+x->ctxsize,(d->maxep<<27)|(d->speed<<20)));
	TRY(mw32(x,d->input+4,(enabled&~2U)|1));
	TRY(command(x,12,slot,0,d->input,0));
	TRY(setconfiguration(x,d,b[5]));
	d->enabled=enabled; d->configured=b[5];
	return 0;
}

int
xhci_transfer(Xhci *x, unsigned slot, unsigned address, void *buf, unsigned n)
{
	Xdevice *d;
	Xaddr a;
	Xword c;
	unsigned ep, in=address>>7;
	int r;
	if(!validdev(x,slot) || address>255 || (address&0x70) || !(address&15) ||
	   n>Xmaxdata || (n && !buf)) return Xinvalid;
	ep=(address&15)*2+in; d=&x->device[slot];
	if(!(d->enabled&BIT(ep)) || !d->configured) return Xinvalid;
	if(d->halted&BIT(ep)) return Xstall;
	if(n && !in) TRY(memio(x,d->buffer,buf,n,1));
	TRY(queue(x,&d->ep[ep],d->buffer,n,T(1)|BIT(5)|(in?BIT(2):0),0,&a,&c));
	TRY(wr(x,x->doorbell+slot*4,ep));
	r=completion(x,32,slot,ep,a,a,a,n,0);
	if(r==Xstall) d->halted|=BIT(ep);
	else if(r<0) return bad(x,r);
	if(r>=0 && in && n) TRY(memio(x,d->buffer,buf,(unsigned)r,0));
	return r;
}

int
xhci_clearhalt(Xhci *x, unsigned slot, unsigned address)
{
	Xbyte setup[8]={2,1,0,0,0,0,0,0};
	unsigned ep;
	if(!validdev(x,slot) || address>255 || (address&0x70) || !(address&15)) return Xinvalid;
	ep=(address&15)*2+(address>>7);
	if(!(x->device[slot].halted&BIT(ep))) return Xinvalid;
	setup[4]=(Xbyte)address;
	TRY(xhci_control(x,slot,setup,0,0));
	return recover(x,&x->device[slot],ep);
}
