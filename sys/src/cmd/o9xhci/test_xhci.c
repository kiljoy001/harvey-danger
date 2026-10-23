/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xhci.c"

/* Fault-injection transport. QEMU tests separately validate the hardware path. */
static Xhci x;
static Xbyte memory[Xarena];
static Xword registers[4096];
static unsigned delays, injected, produce, cycle, noevents, failio;
static Xio io;
static unsigned calls,failat;
static int transportfail(void){return failio || (++calls==failat);}

static void
emit(Xaddr pointer, unsigned type, unsigned slot, unsigned ep, unsigned cc, unsigned residue)
{
	Xbyte *b=memory+x.events-io.dma+produce*16;
	put32(b,(Xword)pointer); put32(b+4,(Xword)(pointer>>32));
	put32(b+8,(cc<<24)|residue);
	put32(b+12,T(type)|slot<<24|ep<<16|cycle);
	if(++produce==Xevents){ produce=0; cycle^=1; }
}
static int
readreg(void *arg, Xword off, Xword *v)
{
	(void)arg;
	if(transportfail() || off>=sizeof registers) return -1;
	*v=registers[off/4]; return 0;
}
static int
writereg(void *arg, Xword off, Xword v)
{
	Xaddr a;
	Xword kind, slot, ep;
	Xring *r;
	(void)arg;
	if(transportfail() || off>=sizeof registers) return -1;
	if(off==0x420 || off==0x430) v|=registers[off/4]&0x3c03;
	registers[off/4]=v;
	if(off==0x20){ registers[off/4]=v&~2U; registers[0x24/4]=(v&1)?0:1; }
	if(off==x.doorbell && x.running && !noevents){
		r=&x.command;
		a=r->base+((r->next?r->next:Xringlen-1)-1)*16;
		kind=(get32(memory+a-io.dma+12)>>10)&63;
		slot=get32(memory+a-io.dma+12)>>24;
		if(kind==9) slot=1;
		emit(a,33,slot,0,injected?injected:1,0);
	}else if(off>x.doorbell && off<=x.doorbell+Xslots*4 && x.running && !noevents){
		slot=(off-x.doorbell)/4; ep=v&31; r=&x.device[slot].ep[ep];
		a=r->base+((r->next?r->next:Xringlen-1)-1)*16;
		emit(a,32,slot,ep,injected?injected:1,0);
	}
	return 0;
}
static int
readmemory(void *arg, Xaddr a, void *buf, Xword n)
{
	(void)arg;
	if(transportfail() || a<io.dma || n>Xarena || a-io.dma>Xarena-n) return -1;
	memcpy(buf,memory+a-io.dma,n); return 0;
}
static int
writememory(void *arg, Xaddr a, const void *buf, Xword n)
{
	(void)arg;
	if(transportfail() || a<io.dma || n>Xarena || a-io.dma>Xarena-n) return -1;
	memcpy(memory+a-io.dma,buf,n); return 0;
}
static void delayfake(void *arg,Xword n){(void)arg;delays+=n;}
static void
fixture(void)
{
	memset(&x,0,sizeof x); memset(memory,0,sizeof memory); memset(registers,0,sizeof registers);
	delays=injected=produce=noevents=failio=calls=failat=0; cycle=1;
	io=(Xio){0,readreg,writereg,readmemory,writememory,delayfake,0x100000, Xarena,sizeof registers};
	registers[0]=0x1000020; registers[1]=(2<<24)|8; registers[4]=1;
	registers[5]=0x2000; registers[6]=0x1000;
	registers[0x24/4]=1; registers[0x28/4]=1;
	registers[0x420/4]=3|(4<<10)|(1<<9);
}
static const Xbyte config[]={
	9,2,44,0,1,1,0,128,50, 9,4,0,0,2,8,6,80,0,
	7,5,0x81,2,0,4,0, 6,48,0,0,0,0,
	7,5,2,2,0,4,0, 6,48,0,0,0,0
};

static void
unit(void)
{
	Xbyte b[256], copy[sizeof config], setup[8]={0x80,6,0,1,0,0,18,0};
	unsigned i, slot;
	Xword v;
	Xaddr a;
	fixture();
	assert(xhci_init(0,&io)==Xinvalid);
	assert(xhci_init(&x,0)==Xinvalid);
	io.dma++; assert(xhci_init(&x,&io)==Xinvalid); io.dma--;
	assert(xhci_init(&x,&io)==0);
	for(i=0;i<600;i++) assert(xhci_noop(&x)==0);
	assert(x.eventcycle==1 && x.command.next==(601%(Xringlen-1)));
	assert(xhci_port(&x,0,&v)==Xinvalid);
	assert(xhci_port(&x,3,&v)==Xinvalid);
	assert(xhci_port(&x,1,&v)==0 && (v&1));
	assert(xhci_attach(&x,2)==Xgone);
	slot=xhci_attach(&x,1); assert(slot==1);
	assert(xhci_attach(&x,1)==Xinvalid);
	assert(xhci_control(&x,1,setup,b,8)==Xinvalid);
	assert(xhci_control(&x,1,setup,0,18)==Xinvalid);
	memory[x.device[1].buffer-io.dma+7]=9;
	assert(xhci_control(&x,1,setup,b,sizeof b)==18);
	assert(xhci_configure(&x,1,config,sizeof config)==0);
	for(i=0;i<100;i++){
		assert(xhci_transfer(&x,1,2,b,256)==256);
		assert(xhci_transfer(&x,1,0x81,b,256)==256);
	}
	assert(xhci_transfer(&x,1,0x91,b,8)==Xinvalid);
	assert(xhci_transfer(&x,1,0x81,b,65537)==Xinvalid);
	assert(xhci_transfer(&x,1,0x81,0,1)==Xinvalid);
	assert(xhci_transfer(&x,1,0x83,b,8)==Xinvalid);
	injected=6;
	assert(xhci_transfer(&x,1,0x81,b,8)==Xstall);
	assert(xhci_transfer(&x,1,0x81,b,8)==Xstall);
	injected=0;
	assert(xhci_clearhalt(&x,1,0x81)==0);
	assert(xhci_transfer(&x,1,0x81,b,8)==8);
	assert(xhci_detach(&x,1)==0);
	assert(xhci_transfer(&x,1,0x81,b,8)==Xinvalid);
	assert(xhci_attach(&x,1)==1);
	assert(xhci_detach(&x,1)==0);
	assert(xhci_stop(&x)==0);
	assert(xhci_noop(&x)==Xioerr);

	fixture(); assert(xhci_init(&x,&io)==0);
	noevents=1;
	assert(xhci_noop(&x)==Xtimeout && x.failed && delays==2000);
	assert(xhci_noop(&x)==Xioerr);
	assert(xhci_stop(&x)==0);
	fixture(); assert(xhci_init(&x,&io)==0);
	failio=1; assert(xhci_noop(&x)==Xioerr && x.failed);
	fixture(); registers[0]=~0U; assert(xhci_init(&x,&io)==Xioerr);
	fixture(); registers[0]=0x950020; assert(xhci_init(&x,&io)==Xunsupported);
	fixture(); registers[1]=0; assert(xhci_init(&x,&io)==Xunsupported);
	fixture(); registers[5]=0xfffffff0; assert(xhci_init(&x,&io)==Xinvalid);
	fixture(); registers[0x28/4]=0; assert(xhci_init(&x,&io)==Xunsupported);
	fixture(); assert(xhci_init(&x,&io)==0);
	assert(allocdma(&x,Xarena,64,&a)==Xnomem);
	assert(memio(&x,io.dma-1,b,1,0)==Xinvalid);

	assert(xhci_validate_config(config,sizeof config,4)==0);
	for(i=0;i<sizeof config;i++) assert(xhci_validate_config(config,i,4)<0);
	memcpy(copy,config,sizeof copy); copy[18]=0;
	assert(xhci_validate_config(copy,sizeof copy,4)<0);
	memcpy(copy,config,sizeof copy); copy[33]=0x81;
	assert(xhci_validate_config(copy,sizeof copy,4)<0);
	memcpy(copy,config,sizeof copy); copy[28]=1;
	assert(xhci_validate_config(copy,sizeof copy,4)==Xunsupported);
	memcpy(copy,config,sizeof copy); copy[22]=1;
	assert(xhci_validate_config(copy,sizeof copy,4)<0);
	memcpy(copy,config,sizeof copy); copy[14]=9;
	assert(xhci_validate_config(copy,sizeof copy,4)==Xunsupported);
	puts("xhci unit PASS");
}
static void
properties(void)
{
	unsigned seed=0x1234abcd, i,j,n;
	Xbyte b[512];
	for(i=0;i<20000;i++){
		seed=seed*1664525+1013904223; n=seed%sizeof b;
		for(j=0;j<n;j++){seed=seed*1664525+1013904223;b[j]=seed>>24;}
		if(i%2==0 && n>=sizeof config){
			memcpy(b,config,sizeof config); n=sizeof config;
			b[seed%n]^=(Xbyte)(seed>>8);
		}
		int r=xhci_validate_config(b,n,1+seed%4);
		assert(r==0 || r==Xinvalid || r==Xunsupported);
		/* A validated descriptor cannot have a malformed total length. */
		if(r==0) assert(n>=9 && get16(b+2)==n && b[0]==9 && b[1]==2);
	}
	puts("xhci generated descriptor properties PASS");
}
static void faults(void)
{
    unsigned limit,i,stage;
    Xbyte b[64],setup[8]={0x80,6,0,1,0,0,18,0};
    for(stage=0;stage<5;stage++){
        limit=1;
        for(i=0;i<=limit;i++){
            fixture();
            /* Test scratchpads, 64-byte contexts and supported protocol caps. */
            registers[2]=1U<<27;
            registers[4]=5|(0x200U<<16);
            registers[0x800/4]=0x03000002;
            registers[0x804/4]=0x20425355;
            registers[0x808/4]=0x201;
            if(stage>0) assert(xhci_init(&x,&io)==0);
            if(stage==4) registers[0x420/4]=3|(1<<10)|(1<<9);
            if(stage>1) assert(xhci_attach(&x,1)==1);
            if(stage==3) assert(xhci_configure(&x,1,config,sizeof config)==0);
            calls=0; failat=i;
            int r;
            if(stage==0) r=xhci_init(&x,&io);
            else if(stage==1) r=xhci_attach(&x,1);
            else if(stage==2) r=xhci_configure(&x,1,config,sizeof config);
            else if(stage==3) r=xhci_transfer(&x,1,0x81,b,64);
            else {
                memory[x.device[1].buffer-io.dma+7]=64;
                r=xhci_control(&x,1,setup,b,64);
            }
            if(!i){assert(r>=0); limit=calls;}
            else assert(r<0 && x.failed);
        }
    }
    /* Protocol capability rejection and BIOS handoff. */
    fixture(); registers[4]=1|(0x200U<<16); registers[0x800/4]=1;
    assert(xhci_init(&x,&io)==0);
    fixture(); registers[4]=1|(0x200U<<16); registers[0x800/4]=0x03000002;
    assert(xhci_init(&x,&io)==Xunsupported);
    /* Completion corruption must poison DMA reuse; a valid stall must not. */
    fixture(); assert(xhci_init(&x,&io)==0);
    emit(0x123456,33,0,0,1,0);
    assert(completion(&x,33,0,0,0x654321,0,0,0,0)==Xioerr && x.failed);
    fixture(); assert(xhci_init(&x,&io)==0);
    emit(0x123456,32,1,2,13,65);
    assert(completion(&x,32,1,2,0x123456,0,0x123456,64,0)==Xioerr && x.failed);
    fixture(); assert(xhci_init(&x,&io)==0);
    emit(0x123456,32,1,2,13,8);
    assert(completion(&x,32,1,2,0x123456,0,0x123456,64,0)==56);
    /* Exercise endpoint packet policies at speed boundaries. */
    for(unsigned speed=1;speed<=4;speed++)
        for(unsigned type=2;type<=3;type++)
            for(unsigned packet=0;packet<=1025;packet++)
                (void)packetvalid(speed,type,packet);
    fixture(); assert(xhci_init(&x,&io)==0);
    assert(xhci_attach(&x,0)==Xinvalid);
    assert(xhci_attach(&x,3)==Xinvalid);
    x.failed=1; assert(xhci_attach(&x,1)==Xinvalid); x.failed=0;
    registers[0x420/4]=3|(15<<10); assert(xhci_attach(&x,1)==Xunsupported);
    /* All legal and illegal EP0 descriptor packet encodings. */
    registers[0x420/4]=3|(1<<10); assert(xhci_attach(&x,1)==1);
    for(unsigned packet=0;packet<256;packet++){
        b[7]=packet;
        int r=learnpacket(&x,&x.device[1],1,b);
        if(packet==8 || packet==16 || packet==32 || packet==64) assert(r==0);
        else assert(r==Xinvalid);
    }
    ConfigScan cs;
    Xbyte endpoint[7]={7,5,0x81,3,8,0,1};
    for(unsigned speed=1;speed<=4;speed++)
        for(unsigned k=0;k<256;k++){
            zero(&cs,sizeof cs); cs.speed=speed; endpoint[2]=k;
            (void)scanendpoint(&cs,endpoint);
        }
    fixture(); assert(xhci_init(&x,&io)==0);
    emit(0x123456,32,1,3,1,0);
    assert(completion(&x,32,1,2,0x123456,0,0x123456,64,0)==Xioerr);
    fixture(); assert(xhci_init(&x,&io)==0);
    emit(0x123456,32,2,2,1,0);
    assert(completion(&x,32,1,2,0x123456,0,0x123456,64,0)==Xioerr);
    fixture(); assert(xhci_init(&x,&io)==0);
    emit(0x123456,37,1,2,1,0);
    assert(completion(&x,32,1,2,0x123456,0,0x123456,64,0)==Xioerr);
    fixture(); assert(xhci_init(&x,&io)==0);
    emit(0,34,0,0,1,0); emit(0x123456,32,1,2,13,8); emit(0x123466,32,1,2,1,0);
    assert(completion(&x,32,1,2,0x123466,0,0x123456,64,0)==56);
    puts("xhci transport fault injection PASS");
}
int main(void){unit();properties();faults();return 0;}
