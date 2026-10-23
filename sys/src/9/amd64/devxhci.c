/* SPDX-License-Identifier: GPL-2.0-only */
/* Trusted user-space xHCI hardware transport, #X/<controller>.
 * USB policy, rings, descriptors, enumeration and transfers live in o9xhci.
 * One open channel owns a controller. Closing it revokes DMA; all DMA memory
 * stays pinned for the lifetime of the kernel. No IOMMU isolation is claimed.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"

enum { Nctlr=8, Dmasize=4*1024*1024, Regbase=0x100000 };
#define Dmabase 0x100000000ULL

typedef struct Xtransport Xtransport;
struct Xtransport {
	QLock lock;
	Pcidev *pci;
	volatile u32 *regs;
	u32 size, op;
	void *dma;
	u64 bus;
	int owned, poisoned;
};
static Xtransport controllers[Nctlr];
static int ncontrollers;

static void
xreset(void)
{
	Pcidev *p=nil;
	Xtransport *t;
	u64 bar;
	u32 cap;
	while(ncontrollers<Nctlr && (p=pcimatch(p,0,0))!=nil){
		if(p->ccrb!=Pcibcserial || p->ccru!=Pciscusb || p->ccrp!=0x30)
			continue;
		bar=p->mem[0].bar;
		if(bar&1 || p->mem[0].size<0x1000 || p->mem[0].size>Regbase)
			continue;
		if((bar&6)==4) bar|=(u64)pcicfgr32(p,0x14)<<32;
		bar &= ~15ULL;
		if(!bar) continue;
		t=&controllers[ncontrollers];
		t->regs=vmap(bar,p->mem[0].size);
		if(t->regs==nil) continue;
		cap=t->regs[0];
		if(cap==~0U || (cap&255)<0x20 || (cap&3)){
			vunmap((void*)t->regs,p->mem[0].size);
			t->regs=nil;
			continue;
		}
		t->pci=p; t->size=p->mem[0].size; t->op=cap&255;
		ncontrollers++;
	}
}

/* Called with the transport locked, without holding an interrupt lock. */
static void
quiesce(Xtransport *t)
{
	int i;
	volatile u32 *op=t->regs+t->op/4;
	op[0]=0;
	coherence();
	for(i=0; i<100; i++){
		if(op[1]&1) break;
		delay(1);
	}
	pciclrbme(t->pci);
	coherence();
	if(i==100 || op[1]==~0U) t->poisoned=1;
	/* DMA is never freed, including on a failed stop. */
}

static void
xshutdown(void)
{
	int i;
	for(i=0; i<ncontrollers; i++)
		if(controllers[i].owned) quiesce(&controllers[i]);
}

static int
xgen(Chan *c, char *unused, Dirtab *dt, int ndt, int s, Dir *dp)
{
	Qid q;
	char name[16];
	USED(unused); USED(dt); USED(ndt);
	if(s==DEVDOTDOT){
		mkqid(&q,0,0,QTDIR);
		devdir(c,q,"#X",0,eve,DMDIR|0500,dp);
		return 1;
	}
	if(s<0 || s>=ncontrollers) return -1;
	snprint(name,sizeof name,"%d",s);
	mkqid(&q,s+1,0,QTFILE);
	devdir(c,q,name,0,eve,0600,dp);
	return 1;
}

static Chan*
xattach(char *spec)
{
	if(!iseve()) error(Eperm);
	return devattach('X',spec);
}
static Walkqid*
xwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c,nc,name,nname,nil,0,xgen);
}
static int
xstat(Chan *c, u8 *db, int n)
{
	return devstat(c,db,n,nil,0,xgen);
}
static Chan*
xopen(Chan *c, int mode)
{
	Proc *up=externup();
	Xtransport *t;
	u32 i;
	if(!iseve()) error(Eperm);
	if(c->qid.type&QTDIR) return devopen(c,mode,nil,0,xgen);
	if(openmode(mode)!=ORDWR || c->qid.path<1 || c->qid.path>(u64)ncontrollers)
		error(Eperm);
	t=&controllers[c->qid.path-1];
	qlock(&t->lock);
	if(waserror()){
		qunlock(&t->lock);
		nexterror();
	}
	if(t->owned) error(Einuse);
	if(t->poisoned) error("xhci transport quarantined until reboot");
	if(t->dma==nil){
		t->dma=mallocalign(Dmasize,65536,0,0);
		if(t->dma==nil) error(Enomem);
		t->bus=PADDR(t->dma);
		for(i=0; i<Dmasize; i+=4096)
			if(PADDR((u8*)t->dma+i)!=t->bus+i){
				free(t->dma); t->dma=nil;
				error("xhci requires contiguous DMA memory");
			}
	}
	/* The service performs BIOS ownership handoff before writing USBCMD. */
	pcisetpms(t->pci,0);
	pcisetbme(t->pci);
	t->owned=1;
	c->aux=t;
	c->mode=ORDWR; c->flag|=COPEN; c->offset=0; c->iounit=65536;
	poperror();
	qunlock(&t->lock);
	return c;
}
static void
xclose(Chan *c)
{
	Xtransport *t;
	if(!(c->flag&COPEN) || c->qid.type&QTDIR) return;
	t=c->aux;
	if(t==nil) return;
	qlock(&t->lock);
	quiesce(t);
	t->owned=0;
	qunlock(&t->lock);
}

static i32
xio(Chan *c, void *a, i32 n, i64 offset, int writing)
{
	Proc *up=externup();
	Xtransport *t=c->aux;
	u64 off=offset;
	u32 value;
	char info[160];
	if(t==nil || offset<0 || n<0) error(Ebadarg);
	qlock(&t->lock);
	if(waserror()){
		qunlock(&t->lock);
		nexterror();
	}
	if(!t->owned || t->poisoned) error(Eio);
	if(off<Regbase && !writing){
		snprint(info,sizeof info,"xhci-transport/1 %llux %ux %ux\n",t->bus,Dmasize,t->size);
		n=readstr(offset,a,n,info);
	}else if(off>=Regbase && off<Regbase+t->size){
		off-=Regbase;
		if(n!=4 || (off&3) || off>t->size-4) error(Ebadarg);
		if(writing){
			memmove(&value,a,4);
			coherence();
			t->regs[off/4]=value;
			coherence();
		}else{
			value=t->regs[off/4];
			coherence();
			memmove(a,&value,4);
		}
	}else if(off>=Dmabase){
		off-=Dmabase;
		if(n>Dmasize || off>Dmasize-n) error(Ebadarg);
		coherence();
		if(writing) memmove((u8*)t->dma+off,a,n);
		else memmove(a,(u8*)t->dma+off,n);
		coherence();
	}else error(Ebadarg);
	poperror();
	qunlock(&t->lock);
	return n;
}
static i32
xread(Chan *c, void *a, i32 n, i64 off)
{
	if(c->qid.type&QTDIR) return devdirread(c,a,n,nil,0,xgen);
	return xio(c,a,n,off,0);
}
static i32
xwrite(Chan *c, void *a, i32 n, i64 off)
{
	if(c->qid.type&QTDIR) error(Eperm);
	return xio(c,a,n,off,1);
}

Dev xhcidevtab = {
	.dc='X', .name="xhci-transport",
	.reset=xreset, .init=devinit, .shutdown=xshutdown,
	.attach=xattach, .walk=xwalk, .stat=xstat, .open=xopen,
	.create=devcreate, .close=xclose, .read=xread, .bread=devbread,
	.write=xwrite, .bwrite=devbwrite, .remove=devremove, .wstat=devwstat,
};
