/* SPDX-License-Identifier: GPL-2.0-only */
#include <u.h>
#include <libc.h>
#include "xhci.h"
#include "../o9drvman/health.h"

static Xhci controller;
static QLock controllerlock;
static int fd=-1;
static uvlong owners[Xslots+1];
static uint handles[Xslots+1], next_handle;
static uvlong completed;

/* #P-backed board transport. */
static int boardmode;
static uchar *boardmmio, *boarddma;
static u32 boardmmiosz, boarddmasz;
static u64 boarddmabase;
enum { Bigpage = 2*1024*1024 };

static long
readtext(char *path, char *buf, long n)
{
	int f;
	long m;
	f=open(path,OREAD);
	if(f<0) return -1;
	m=read(f,buf,n-1);
	close(f);
	if(m<0) return -1;
	buf[m]=0;
	return m;
}

static void*
mapreg(u64 base, usize size, char *outname, int outlen)
{
	Dir *d;
	char path[128], buf[64], *p;
	void *m;
	u64 reg, len;
	int f, i, n;

	f=open("#P",OREAD);
	if(f<0) return nil;
	n=dirread(f,&d);
	close(f);
	if(n<=0) return nil;
	for(i=0;i<n;i++){
		if(strcmp(d[i].name,".")==0 || strcmp(d[i].name,"..")==0)
			continue;
		snprint(path,sizeof path,"#P/%s/reg",d[i].name);
		if(readtext(path,buf,sizeof buf)<0)
			continue;
		reg=strtoull(buf,&p,0);
		if(reg!=base)
			continue;
		len=(size+Bigpage-1)&~(Bigpage-1);
		m=segattach(0,d[i].name,0,len);
		if(m==(void*)-1){
			free(d);
			return nil;
		}
		if(outname!=nil && outlen>0){
			strncpy(outname,d[i].name,outlen-1);
			outname[outlen-1]=0;
		}
		free(d);
		return (uchar*)m+(base&(Bigpage-1));
	}
	free(d);
	return nil;
}

static int
board_dma(void)
{
	char buf[128], *p;
	u64 base, size;

	fprint(2,"o9xhci: dma: read grant\n");
	if(readtext("#P/dma/base",buf,sizeof buf)<0)
		return -1;
	base=strtoull(buf,&p,0);
	if(*p!=0 && *p!='\n' && *p!=' ')
		return -1;
	if(readtext("#P/dma/size",buf,sizeof buf)<0)
		return -1;
	size=strtoull(buf,&p,0);
	if(base==0 || size<Xarena)
		return -1;
	fprint(2,"o9xhci: dma: attach %llux+%llux\n",base,size);
	boarddma=segattach(0,"dma",0,size);
	if(boarddma==(void*)-1)
		return -1;
	boarddmabase=base;
	boarddmasz=size;
	return 0;
}

static int
board_findctl(u64 *base, u32 *size)
{
	Dir *d;
	char path[128], buf[128], *p;
	u64 reg, sz;
	int f,i,n;

	fprint(2,"o9xhci: controller: scan #P\n");
	f=open("#P",OREAD);
	if(f<0) return -1;
	n=dirread(f,&d);
	close(f);
	if(n<=0) return -1;
	for(i=0;i<n;i++){
		snprint(path,sizeof path,"#P/%s/class",d[i].name);
		if(readtext(path,buf,sizeof buf)<0 || strncmp(buf,"usb",3)!=0)
			continue;
		snprint(path,sizeof path,"#P/%s/compatible",d[i].name);
		if(readtext(path,buf,sizeof buf)<0 || strstr(buf,"dwc3")==nil)
			continue;
		snprint(path,sizeof path,"#P/%s/reg",d[i].name);
		if(readtext(path,buf,sizeof buf)<0)
			continue;
		reg=strtoull(buf,&p,0);
		snprint(path,sizeof path,"#P/%s/size",d[i].name);
		if(readtext(path,buf,sizeof buf)<0)
			continue;
		sz=strtoull(buf,&p,0);
		if(reg==0xfd000000ULL || *base==0){
			*base=reg;
			*size=sz;
		}
	}
	free(d);
	return *base != 0 ? 0 : -1;
}

static int
bread32(void *u, Xword off, Xword *v)
{
	USED(u);
	if(off+4>boardmmiosz) return -1;
	*v=*(volatile u32*)(boardmmio+off);
	__sync_synchronize();
	return 0;
}
static int
bwrite32(void *u, Xword off, Xword v)
{
	USED(u);
	if(off+4>boardmmiosz) return -1;
	*(volatile u32*)(boardmmio+off)=v;
	__sync_synchronize();
	return 0;
}
static int
breadmem(void *u, Xaddr a, void *buf, Xword n)
{
	USED(u);
	if(a<boarddmabase || a-boarddmabase>boarddmasz || n>boarddmasz-(a-boarddmabase))
		return -1;
	__sync_synchronize();
	memmove(buf,boarddma+(a-boarddmabase),n);
	return 0;
}
static int
bwritemem(void *u, Xaddr a, const void *buf, Xword n)
{
	USED(u);
	if(a<boarddmabase || a-boarddmabase>boarddmasz || n>boarddmasz-(a-boarddmabase))
		return -1;
	memmove(boarddma+(a-boarddmabase),buf,n);
	__sync_synchronize();
	return 0;
}
static void
bdelay(void *u, Xword ms)
{
	USED(u);
	sleep(ms);
}

static int
waitusbpower(void)
{
	char buf[32];
	int sfd, i;
	long n;

	for(i = 0; i < 100; i++){
		sfd = open("/mnt/usbpower.ready", OREAD);
		if(sfd >= 0){
			n = read(sfd, buf, sizeof buf - 1);
			close(sfd);
			if(n > 0){
				buf[n] = 0;
				if(strncmp(buf, "ready", 5) == 0)
					return 0;
			}
		}
		sleep(100);
	}
	return -1;
}

static int
board_start(void)
{
	Xio io;
	u64 base=0;
	u32 size=0;
	char name[64];
	volatile u32 *dwc;
	u32 v;

	if(waitusbpower() < 0){
		fprint(2,"o9xhci: usbpower service not ready\n");
		return -1;
	}
	fprint(2,"o9xhci: usbpower ready\n");
	if(board_dma()<0){
		fprint(2,"o9xhci: no #P/dma grant\n");
		return -1;
	}
	fprint(2,"o9xhci: dma %llux+%ux\n",boarddmabase,boarddmasz);
	if(board_findctl(&base,&size)<0){
		fprint(2,"o9xhci: no DWC3 controller in #P\n");
		return -1;
	}
	fprint(2,"o9xhci: controller %llux+%ux\n",base,size);
	fprint(2,"o9xhci: controller: map %llux\n",base);
	boardmmio=mapreg(base,size,name,sizeof name);
	if(boardmmio==nil){
		fprint(2,"o9xhci: cannot map controller %llux\n",base);
		return -1;
	}
	boardmmiosz=size;
	fprint(2,"o9xhci: controller mapped as %s\n",name);
	/* DWC3 core mode and UTMI/PIPE interface, before xHCI reset. */
	dwc=(volatile u32*)boardmmio;
	v=dwc[0xc110/4];
	v &= ~(3U<<4);
	v = (v & ~(3U<<12)) | (1U<<12);
	dwc[0xc110/4]=v;
	v=dwc[0xc200/4];
	v &= ~((1U<<6)|(1U<<31)|(1U<<4));
	v &= ~(0xfU<<10);
	v |= (1U<<3)|(1U<<8)|(5U<<10);
	dwc[0xc200/4]=v;
	v=dwc[0xc2c0/4];
	v &= ~((1U<<17)|(1U<<27)|(1U<<31));
	dwc[0xc2c0/4]=v;
	__sync_synchronize();
	fprint(2,"o9xhci: dwc3 gctl %ux usb2 %ux usb3 %ux\n",
	       dwc[0xc110/4],dwc[0xc200/4],dwc[0xc2c0/4]);
	memset(&io,0,sizeof io);
	io.dma=boarddmabase;
	io.size=boarddmasz;
	io.mmiosize=boardmmiosz;
	io.read32=bread32; io.write32=bwrite32;
	io.readmem=breadmem; io.writemem=bwritemem;
	io.delay=bdelay; io.trace=0;
	boardmode=1;
	fprint(2,"o9xhci: xhci_init: begin\n");
	if(xhci_init(&controller,&io)<0){
		fprint(2,"o9xhci: xhci_init: failed %d (completion %u)\n",
		       (int)controller.failed,controller.completion);
		boardmode=0;
		return -1;
	}
	fprint(2,"o9xhci: xhci_init: done\n");
	return 0;
}


static void
trace(void *unused, const char *format, ...)
{
	va_list args;
	USED(unused);
	va_start(args, format);
	vfprint(2, (char*)format, args);
	va_end(args);
}

static int
read32(void *unused, Xword offset, Xword *value)
{
	USED(unused);
	return pread(fd,value,4,0x100000ULL+offset)==4 ? 0 : -1;
}
static int
write32(void *unused, Xword offset, Xword value)
{
	USED(unused);
	return pwrite(fd,&value,4,0x100000ULL+offset)==4 ? 0 : -1;
}
static int
readmem(void *unused, Xaddr address, void *buf, Xword n)
{
	USED(unused);
	return pread(fd,buf,n,0x100000000ULL+address-controller.io.dma)==n ? 0 : -1;
}
static int
writemem(void *unused, Xaddr address, const void *buf, Xword n)
{
	USED(unused);
	return pwrite(fd,buf,n,0x100000000ULL+address-controller.io.dma)==n ? 0 : -1;
}
static void
pausems(void *unused, Xword ms)
{
	USED(unused);
	sleep(ms);
}

int
o9xhci_start(char *path)
{
	Xio io;
	char info[160], *f[5], *end;
	int n,r=-1;
	fprint(2,"o9xhci: start path %s\n",path == nil ? "(nil)" : path);
	qlock(&controllerlock);
	if(fd>=0 || boardmode){ qunlock(&controllerlock); return -1; }
	if(strncmp(path,"#X/",3)!=0 || path[3]<'0' || path[3]>'7' || path[4])
		goto board;
	fprint(2,"o9xhci: start: open %s\n",path);
	fd=open(path,ORDWR);
	if(fd<0){
		fd=-1;
		fprint(2,"o9xhci: start: #X unavailable\n");
		goto board;
	}
	n=pread(fd,info,sizeof info-1,0);
	if(n<=0) goto fail;
	info[n]=0;
	fprint(2,"o9xhci: transport %s",info);
	if(tokenize(info,f,nelem(f))!=4 || strcmp(f[0],"xhci-transport/1")) goto fail;
	memset(&io,0,sizeof io);
	io.dma=strtoull(f[1],&end,16); if(*end) goto fail;
	io.size=strtoul(f[2],&end,16); if(*end) goto fail;
	io.mmiosize=strtoul(f[3],&end,16); if(*end) goto fail;
	io.read32=read32; io.write32=write32;
	io.readmem=readmem; io.writemem=writemem; io.delay=pausems;
	io.trace=trace;
	r=xhci_init(&controller,&io);
	if(r<0) fprint(2,"o9xhci: init error %d (completion %u): %r\n",r,controller.completion);
	if(r<0) goto fail;
	goto done;
board:
	fprint(2,"o9xhci: start: board transport\n");
	r=board_start();
	if(r<0) fprint(2,"o9xhci: board transport unavailable: %r\n");
	goto done;
fail:
	close(fd); fd=-1;
done:
	qunlock(&controllerlock);
	return r;
}

static uint
get(const uchar *p)
{
	return (uint)p[0]|(uint)p[1]<<8|(uint)p[2]<<16|(uint)p[3]<<24;
}

int
o9xhci_stop(void)
{
	int r=0;
	qlock(&controllerlock);
	if(fd>=0 || boardmode){
		r=xhci_stop(&controller);
		if(fd>=0)
			close(fd);
		fd=-1;
		boardmode=0;
	}
	memset(owners,0,sizeof owners);
	memset(handles,0,sizeof handles);
	qunlock(&controllerlock);
	return r;
}
static void
put(uchar *p,uint v)
{
	int i;
	for(i=0;i<4;i++) p[i]=v>>(8*i);
}

/* Response: signed result, handle, payload length, xHCI completion code,
 * then binary payload. One independent result per mounted view. */
uchar*
o9xhci_request(uvlong owner, uchar *request, int n, int *outlen)
{
	uchar *reply, *payload;
	uint op, handle, endpoint, count, slot=0, i, status;
	int result=Xinvalid, length=0, incoming;
	reply=mallocz(16+Xmaxdata,1);
	if(reply==nil){ *outlen=0; return nil; }
	payload=reply+16;
	qlock(&controllerlock);
	if(n<24 || n>24+8+Xmaxdata || memcmp(request,"XHC1",4) || get(request+20))
		goto done;
	op=get(request+4); handle=get(request+8); endpoint=get(request+12); count=get(request+16);
	if((fd<0 && !boardmode) || !owner || controller.failed){ result=Xioerr; goto done; }
	controller.completion=0;
	if(op!=1 && op!=2 && op!=8 && op!=18 && op!=19){
		for(i=1;i<=controller.slots;i++)
			if(owners[i]==owner && handles[i]==handle && controller.device[i].slot){slot=i;break;}
		if(!slot) goto done;
	}
	switch(op){
	case 1: /* PORT: handle field carries root port */
		if(n!=24 || count || endpoint) break;
		result=xhci_port(&controller,handle,&status);
		if(result==0){put(payload,status);length=4;}
		break;
	case 2: /* ATTACH: reset, enable slot, address device */
		if(n!=24 || count || endpoint || next_handle==~0U) break;
		result=xhci_attach(&controller,handle);
		if(result>0){
			slot=result; handles[slot]=++next_handle; owners[slot]=owner;
			put(reply+4,handles[slot]); result=0;
		}
		break;
	case 3:
		if(n!=24 || count || endpoint) break;
		result=xhci_detach(&controller,slot);
		if(result==0){ owners[slot]=0; handles[slot]=0; }
		break;
	case 4: /* CONTROL: setup followed by OUT bytes; IN bytes in reply */
		if(n<32 || endpoint || count>Xmaxdata) break;
		incoming=(request[24]&128)!=0;
		if(count!=(uint)(request[30]|request[31]<<8) || n!=32+(incoming?0:count)) break;
		if(!incoming && count) memmove(payload,request+32,count);
		result=xhci_control(&controller,slot,request+24,payload,count);
		if(result>=0 && incoming) length=result;
		break;
	case 5: /* CONFIGURE: complete configuration descriptor */
		if(endpoint || count>Xmaxdata || n!=24+count) break;
		result=xhci_configure(&controller,slot,request+24,count);
		break;
	case 6: /* TRANSFER: endpoint address, length, OUT data */
		if(count>Xmaxdata || endpoint>255 || n!=24+((endpoint&128)?0:count)) break;
		if(!(endpoint&128) && count) memmove(payload,request+24,count);
		result=xhci_transfer(&controller,slot,endpoint,payload,count);
		if(result>=0 && (endpoint&128)) length=result;
		break;
	case 7:
		if(n!=24 || count) break;
		result=xhci_clearhalt(&controller,slot,endpoint);
		break;
	case 8:
		if(n!=24 || handle || endpoint || count) break;
		result=xhci_noop(&controller);
		break;
	case 16: /* SET_INTERFACE: endpoint=interface, count=alt */
		if(n!=24 || endpoint>255 || count>255) break;
		result=xhci_setinterface(&controller,slot,endpoint,count);
		break;
	case 17: /* ISOC IN: endpoint, count, frame at request[24..27] */
		if(n!=32 || endpoint>255 || count>Xmaxdata) break;
		result=xhci_isoc(&controller,slot,endpoint,payload,count,
		                 get(request+24));
		if(result>=0) length=result;
		break;
	case 18: /* FRAME: current MFINDEX */
		if(n!=24 || handle || endpoint || count) break;
		result=xhci_frame(&controller,(unsigned *)&status);
		if(result==0){put(payload,status);length=4;}
		break;
	case 19: /* UAC1: parse configuration descriptor in request+24 */
		if(endpoint || count>Xmaxdata || n!=24+count) break;
		{
			Xuac1 u;
			result=xhci_uac1_parse(request+24,count,&u);
			if(result==0){
				put(payload,u.iface); put(payload+4,u.alt);
				put(payload+8,u.ep); put(payload+12,u.packet);
				put(payload+16,u.interval); put(payload+20,u.channels);
				put(payload+24,u.subframe); put(payload+28,u.bits);
				put(payload+32,u.rate); length=9*4;
			}
		}
		break;
	}
	if(result>=0) completed++;
	/* Stop/revoke immediately after poisoned state, not at eventual exit. */
	if(controller.failed){ close(fd); fd=-1; }
 done:
	put(reply,(uint)result);
	put(reply+8,length);
	put(reply+12,controller.completion);
	*outlen=16+length;
	qunlock(&controllerlock);
	return reply;
}

void
o9xhci_release(uvlong owner)
{
	uint i;
	qlock(&controllerlock);
	for(i=1;i<=controller.slots;i++)
		if(owners[i]==owner){
			if((fd>=0 || boardmode) && xhci_detach(&controller,i)<0){
				if(fd>=0) close(fd);
				fd=-1; boardmode=0;
			}
			owners[i]=0; handles[i]=0;
		}
	qunlock(&controllerlock);
}

static DrvHealth *driverhealth;

int
o9xhci_health_start(void)
{
	driverhealth=drvhealth_openenv();
	if(driverhealth==nil)
		return -1;
	return drvhealth_ready(driverhealth);
}

int
o9xhci_health_tick(void)
{
	int failed;
	uvlong done;

	if(driverhealth==nil)
		return 1;
	qlock(&controllerlock);
	failed=controller.failed;
	done=completed;
	qunlock(&controllerlock);
	drvhealth_report(driverhealth,
	                 failed ? DrvHealthFailed : DrvHealthHealthy,
	                 0, 0, done, failed ? 1 : 0, 0, 0,
	                 failed ? DrvHealthRecommendRestart : DrvHealthRecommendNone,
	                 failed ? "xhci-failed" : "none");
	if(drvhealth_stopwait(driverhealth, 1000)){
		drvhealth_close(driverhealth);
		driverhealth=nil;
		return 1;
	}
	return 0;
}

void
o9xhci_health_close(void)
{
	if(driverhealth!=nil){
		drvhealth_close(driverhealth);
		driverhealth=nil;
	}
}

int
o9xhci_health_loop(void)
{
	if(o9xhci_health_start() < 0)
		return 0;
	while(o9xhci_health_tick() == 0)
		;
	o9xhci_health_close();
	return 0;
}

char*
o9xhci_status(void)
{	char *s=malloc(256);
	if(s==nil) return nil;
	qlock(&controllerlock);
	snprint(s,256,"o9xhci/1 running %d failed %d ports %u slots %u completed %llu\n",
	        (fd>=0 || boardmode) && controller.running,controller.failed,
	        controller.ports,controller.slots,completed);
	qunlock(&controllerlock);
	return s;
}
