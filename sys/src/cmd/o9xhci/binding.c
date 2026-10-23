/* SPDX-License-Identifier: GPL-2.0-only */
#include <u.h>
#include <libc.h>
#include "xhci.h"

static Xhci controller;
static QLock controllerlock;
static int fd=-1;
static uvlong owners[Xslots+1];
static uint handles[Xslots+1], next_handle;
static uvlong completed;

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
	qlock(&controllerlock);
	if(fd>=0){ qunlock(&controllerlock); return -1; }
	if(strncmp(path,"#X/",3)!=0 || path[3]<'0' || path[3]>'7' || path[4])
		goto done;
	fd=open(path,ORDWR);
	if(fd<0){ fprint(2,"o9xhci: open %s: %r\n",path); goto done; }
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
	r=xhci_init(&controller,&io);
	if(r<0) fprint(2,"o9xhci: init error %d (completion %u): %r\n",r,controller.completion);
	if(r<0) goto fail;
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
	if(fd<0 || !owner || controller.failed){ result=Xioerr; goto done; }
	controller.completion=0;
	if(op!=1 && op!=2 && op!=8){
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
			if(fd>=0 && xhci_detach(&controller,i)<0){close(fd);fd=-1;}
			owners[i]=0; handles[i]=0;
		}
	qunlock(&controllerlock);
}

char*
o9xhci_status(void)
{
	char *s=malloc(256);
	if(s==nil) return nil;
	qlock(&controllerlock);
	snprint(s,256,"o9xhci/1 running %d failed %d ports %u slots %u completed %llud\n",
	        fd>=0 && controller.running,controller.failed,controller.ports,controller.slots,completed);
	qunlock(&controllerlock);
	return s;
}
