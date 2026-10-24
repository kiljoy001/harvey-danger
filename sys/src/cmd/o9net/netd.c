/*
 * o9net - guest user-space /net service.
 *
 * The kernel has no network stack.  This process owns /net, an IPv6 packet
 * interface, and the IL transport.  The IL state machine and wire format
 * follow sys/src/9/ip/il.c (the improved kernel IL); kernel Block/Queue/Conv
 * are replaced by the user-space packet/channel model below.  Yggdrasil is
 * a separate user program:
 * it binds /net/ipifc/0/pkt for its encrypted/routed link and is not part of
 * this server.  TCP/UDP are added on top of the same packet interface later.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

enum {
	Ilhdrlen = 18,
	Ilip6len = 40,
	Maxpkt = 65535,
	Maxdata = 4096,
	Maxconv = 256,
	Maxport = 65535,
	Firstport = 20000,
	Maxlisten = 64,

	IlSync = 0,
	IlData = 1,
	IlDataQuery = 2,
	IlAck = 3,
	IlQuery = 4,
	IlState = 5,
	IlClose = 6,
};

enum {
	Cclosed,
	Csyncer,
	Csyncee,
	Cestablished,
	Cclosing,
	Clistening,
};

typedef struct Packet Packet;
struct Packet {
	int len;
	u8 data[Maxpkt];
};

typedef struct Ifc Ifc;
typedef struct Il Il;

struct Ifc {
	int idx;
	u8 addr[16];
	int prefix;
	int pktbound;
	Channel *out;
};

typedef struct Iltx Iltx;
struct Iltx {
	Iltx *next;
	u32 id;
	int len;
	u8 *pkt;
};

typedef struct Ilbuf Ilbuf;
struct Ilbuf {
	Ilbuf *next;
	int len;
	u8 data[Maxdata];
};

typedef struct Ilrx Ilrx;
struct Ilrx {
	Ilrx *next;
	u32 id;
	int len;
	u8 data[Maxdata];
};

struct Il {
	int idx;
	int state;
	int listener;
	u16 lport;
	u16 rport;
	u8 laddr[16];
	u8 raddr[16];
	u32 start;
	u32 next;
	u32 recvd;
	u32 rstart;
	u32 window;
	u32 cwind;
	u32 ssthresh;
	u32 flight;
	u32 txbytes;
	int rexmit;
	int rxtot;
	u64 timeout;
	u64 lastrecv;
	Iltx *unacked;
	Iltx *unackedtail;
	Ilbuf *wq;
	Ilbuf *wqtail;
	Ilrx *rxq;
	Channel *rq;
	Channel *acceptq;
	File *dir;
	char err[ERRMAX];
};

enum {
	EvConnect,
	EvAnnounce,
	EvWrite,
	EvClose,
	EvInput,
	EvTick,
};

typedef struct Ilev Ilev;
struct Ilev {
	int type;
	Il *il;
	u8 *pkt;
	int len;
	char addr[256];
	int port;
};

enum {
	Nroot,
	Nildir,
	Nclone,
	Nconvdir,
	Nctl,
	Ndata,
	Nlisten,
	Nlocal,
	Nremote,
	Nstatus,
	Nerr,
	Nipifcdir,
	Nifcdir,
	Npkt,
	Nifcctl,
	Nifcclone,
};

typedef struct Node Node;
struct Node {
	int kind;
	Il *il;
	Ifc *ifc;
};

static Tree *tree;
static File *rootdir, *ildir, *ipifcdir, *ifcdir;
static Node *rootnode, *ilnode, *clonenode, *ipifcnode, *ifcnode;
static Il *convs[Maxconv];
static Ifc ifc;
static Lock convlock;
static u16 nextport = Firstport;
static Srv fs;

static void ilinput(u8 *pkt, int len);
static void ilinputsync(u8 *pkt, int len);
static Il *newconv(void);
static Channel *ilev;
static void ilworker(void *);
static void ilticker(void *);

static u16
be16(u8 *p)
{
	return (u16)(p[0]<<8 | p[1]);
}

static u32
be32(u8 *p)
{
	return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3];
}

static void
put16(u8 *p, u16 v)
{
	p[0] = v>>8;
	p[1] = v;
}

static void
put32(u8 *p, u32 v)
{
	p[0] = v>>24;
	p[1] = v>>16;
	p[2] = v>>8;
	p[3] = v;
}

static int
ip6eq(u8 *a, u8 *b)
{
	return memcmp(a, b, 16) == 0;
}

static int
ip6loopback(u8 *a)
{
	int i;
	for(i = 0; i < 15; i++)
		if(a[i] != 0)
			return 0;
	return a[15] == 1;
}

static void
ip6copy(u8 *d, u8 *s)
{
	memmove(d, s, 16);
}

static int
hexval(int c)
{
	if(c >= '0' && c <= '9')
		return c-'0';
	if(c >= 'a' && c <= 'f')
		return c-'a'+10;
	if(c >= 'A' && c <= 'F')
		return c-'A'+10;
	return -1;
}

/* Parse the subset of IPv6 usually used by IL: hex groups and ::. */
static int
parseip6(char *s, u8 *a)
{
	int i, n, gap, v;
	char *p, *q;

	memset(a, 0, 16);
	gap = -1;
	p = s;
	if(*p == '['){
		p++;
		q = strchr(p, ']');
		if(q == nil)
			return -1;
		*q = 0;
	}
	if(strcmp(p, "::") == 0)
		return 0;
	if(*p == ':' && p[1] == ':'){
		gap = 0;
		p += 2;
	}
	n = 0;
	while(*p != 0){
		if(n >= 8)
			return -1;
		if(*p == ':'){
			if(gap >= 0)
				return -1;
			gap = n;
			p++;
			if(*p == 0)
				break;
			continue;
		}
		v = 0;
		for(i = 0; i < 4 && hexval(*p) >= 0; i++){
			v = (v<<4) | hexval(*p);
			p++;
		}
		if(i == 0)
			return -1;
		a[n*2] = v>>8;
		a[n*2+1] = v;
		n++;
		if(*p == ':'){
			p++;
			if(*p == 0)
				break;
		} else if(*p != 0)
			return -1;
	}
	if(gap >= 0){
		if(n >= 8)
			return -1;
		memmove(a+(8-(n-gap))*2, a+gap*2, (n-gap)*2);
		memset(a+gap*2, 0, (8-n)*2);
	} else if(n != 8)
		return -1;
	return 0;
}

static void
formatip6(u8 *a, char *buf, int n)
{
	snprint(buf, n, "%x:%x:%x:%x:%x:%x:%x:%x",
		be16(a), be16(a+2), be16(a+4), be16(a+6),
		be16(a+8), be16(a+10), be16(a+12), be16(a+14));
}

static u16
csum(u8 *p, int n)
{
	u32 sum;
	int i;

	sum = 0;
	for(i = 0; i < n; i += 2){
		if(i+1 < n)
			sum += (p[i]<<8) | p[i+1];
		else
			sum += p[i]<<8;
	}
	while(sum>>16)
		sum = (sum & 0xffff) + (sum>>16);
	return (u16)~sum;
}

static u16
allocport(void)
{
	int i, tries;
	u16 p;

	for(tries = 0; tries < 4096; tries++){
		p = nextport++;
		if(nextport < Firstport)
			nextport = Firstport;
		if(p == 0)
			continue;
		for(i = 0; i < Maxconv; i++)
			if(convs[i] != nil && convs[i]->lport == p)
				break;
		if(i == Maxconv)
			return p;
	}
	return 0;
}

static Il *
findlistener(u8 *addr, u16 port)
{
	int i;
	Il *il;

	for(i = 0; i < Maxconv; i++){
		il = convs[i];
		if(il == nil || il->state != Clistening || il->lport != port)
			continue;
		if(ip6eq(il->laddr, addr))
			return il;
	}
	return nil;
}

static Il *
findconn(u8 *laddr, u16 lport, u8 *raddr, u16 rport)
{
	int i;
	Il *il;

	for(i = 0; i < Maxconv; i++){
		il = convs[i];
		if(il == nil)
			continue;
		if(il->lport == lport && il->rport == rport &&
		   ip6eq(il->laddr, laddr) && ip6eq(il->raddr, raddr))
			return il;
	}
	return nil;
}

static void
deliver(Il *il, u8 *data, int n)
{
	Packet *p;

	if(il->state != Cestablished && il->state != Csyncee && il->state != Csyncer)
		return;
	p = malloc(sizeof(*p));
	if(p == nil)
		return;
	if(n > Maxpkt)
		n = Maxpkt;
	memmove(p->data, data, n);
	p->len = n;
	if(nbsendp(il->rq, p) != 1)
		free(p);
}

static u8 *
ilbuild(Il *il, int type, u32 id, u32 ack, u8 *data, int dlen, int spec, int *plen)
{
	u8 *pkt;
	u8 *ih;
	int total;
	u16 c;

	if(dlen < 0 || dlen > Maxdata)
		return nil;
	total = Ilip6len + Ilhdrlen + dlen;
	pkt = malloc(total);
	if(pkt == nil)
		return nil;
	memset(pkt, 0, total);
	pkt[0] = 0x60;
	put16(pkt+4, Ilhdrlen+dlen);
	pkt[6] = 40;
	pkt[7] = 64;
	ip6copy(pkt+8, il->laddr);
	ip6copy(pkt+24, il->raddr);
	ih = pkt+Ilip6len;
	put16(ih+2, Ilhdrlen+dlen);
	ih[4] = type;
	ih[5] = spec;
	put16(ih+6, il->lport);
	put16(ih+8, il->rport);
	put32(ih+10, id);
	put32(ih+14, ack);
	if(dlen > 0)
		memmove(ih+Ilhdrlen, data, dlen);
	c = csum(ih, Ilhdrlen+dlen);
	put16(ih, c);
	*plen = total;
	return pkt;
}

static void
ilsendpkt(Il *il, u8 *pkt, int len)
{
	if(pkt == nil)
		return;
	if(ip6loopback(il->raddr) || ip6eq(il->raddr, ifc.addr)){
		ilinputsync(pkt, len); /* takes ownership */
		return;
	}
	if(ifc.pktbound){
		if(nbsendp(ifc.out, pkt) == 1)
			return;
	}
	free(pkt);
}

static void
iloutput(Il *il, int type, u32 id, u32 ack, u8 *data, int dlen, int spec)
{
	u8 *pkt;
	int len;

	pkt = ilbuild(il, type, id, ack, data, dlen, spec, &len);
	ilsendpkt(il, pkt, len);
}

static void
iltimeout(Il *il, int ms)
{
	il->timeout = (u64)(nsec()/1000000) + (u64)ms;
}

static void
ilpump(Il *il)
{
	Ilbuf *b;
	u8 *pkt;
	int len;
	Iltx *tx;

	if(il->state != Cestablished)
		return;
	while(il->flight < il->cwind && (b = il->wq) != nil){
		il->wq = b->next;
		if(il->wq == nil)
			il->wqtail = nil;
		pkt = ilbuild(il, IlData, il->next, il->recvd, b->data, b->len, 0, &len);
		if(pkt == nil){
			free(b);
			continue;
		}
		tx = mallocz(sizeof(*tx), 1);
		if(tx == nil){
			free(pkt);
			free(b);
			continue;
		}
		tx->id = il->next++;
		tx->len = len;
		tx->pkt = malloc(len);
		if(tx->pkt == nil){
			free(tx);
			free(pkt);
			free(b);
			continue;
		}
		memmove(tx->pkt, pkt, len);
		if(il->unackedtail != nil)
			il->unackedtail->next = tx;
		else
			il->unacked = tx;
		il->unackedtail = tx;
		il->flight++;
		il->txbytes += b->len;
		if(il->flight == 1)
			iltimeout(il, 250);
		free(b);
		ilsendpkt(il, pkt, len);
	}
}

static void
ilackto(Il *il, u32 ack)
{
	Iltx *tx;
	int acked;

	acked = 0;
	while((tx = il->unacked) != nil && tx->id <= ack){
		il->unacked = tx->next;
		free(tx->pkt);
		free(tx);
		if(il->flight > 0)
			il->flight--;
		acked++;
	}
	if(il->unacked == nil)
		il->unackedtail = nil;
	if(acked){
		il->rexmit = 0;
		if(il->cwind < il->ssthresh){
			il->cwind += acked;
			if(il->cwind > il->window)
				il->cwind = il->window;
		}
		if(il->unacked != nil)
			iltimeout(il, 250);
		else
			il->timeout = 0;
		ilpump(il);
	}
}

static void
ilrxadd(Il *il, u32 id, u8 *data, int n)
{
	Ilrx *r, *p, **l;

	if(id <= il->recvd || id > il->recvd + il->window)
		return;
	r = mallocz(sizeof(*r), 1);
	if(r == nil)
		return;
	r->id = id;
	r->len = n;
	memmove(r->data, data, n);
	l = &il->rxq;
	for(p = *l; p != nil; p = p->next){
		if(id < p->id){
			r->next = p;
			*l = r;
			return;
		}
		if(id == p->id){
			free(r);
			return;
		}
		l = &p->next;
	}
	*l = r;
}

static void
ilrxpull(Il *il)
{
	Ilrx *r;
	Ilrx **l;

	for(;;){
		l = &il->rxq;
		while((r = *l) != nil && r->id <= il->recvd)
			*l = r->next, free(r);
		r = il->rxq;
		if(r == nil || r->id != il->recvd+1)
			break;
		il->recvd = r->id;
		deliver(il, r->data, r->len);
		il->rxq = r->next;
		free(r);
	}
}

static void
ilrxfree(Il *il)
{
	Ilrx *r, *n;

	for(r = il->rxq; r != nil; r = n){
		n = r->next;
		free(r);
	}
	il->rxq = nil;
}

static void
ilclear(Il *il)
{
	Iltx *tx, *tn;
	Ilbuf *b, *bn;

	for(tx = il->unacked; tx != nil; tx = tn){
		tn = tx->next;
		free(tx->pkt);
		free(tx);
	}
	il->unacked = nil;
	il->unackedtail = nil;
	for(b = il->wq; b != nil; b = bn){
		bn = b->next;
		free(b);
	}
	il->wq = nil;
	il->wqtail = nil;
	ilrxfree(il);
	il->flight = 0;
}

static void
ilqueue(Il *il, u8 *data, int n)
{
	Ilbuf *b;
	int off, m;

	for(off = 0; off < n; off += m){
		m = n-off;
		if(m > Maxdata)
			m = Maxdata;
		b = mallocz(sizeof(*b), 1);
		if(b == nil)
			return;
		b->len = m;
		memmove(b->data, data+off, m);
		if(il->wqtail != nil)
			il->wqtail->next = b;
		else
			il->wq = b;
		il->wqtail = b;
	}
	ilpump(il);
}

static void
ilclose(Il *il)
{
	if(il->state == Cclosed)
		return;
	il->state = Cclosing;
	iloutput(il, IlClose, il->next, il->recvd, nil, 0, 0);
	iltimeout(il, 1000);
}

static void
ilinput(u8 *pkt, int len)
{
	Ilev *ev;

	ev = mallocz(sizeof(*ev), 1);
	if(ev == nil){
		free(pkt);
		return;
	}
	ev->type = EvInput;
	ev->pkt = pkt;
	ev->len = len;
	if(ilev == nil || nbsendp(ilev, ev) != 1){
		free(pkt);
		free(ev);
	}
}

static void
ilinputsync(u8 *pkt, int len)
{
	u8 *ih, *src, *dst, *data;
	u16 plen, illen, sp, dp;
	u32 id, ack;
	int type, spec, dlen;
	Il *il, *n;

	if(len < Ilip6len+Ilhdrlen)
		goto drop;
	if((pkt[0]>>4) != 6 || pkt[6] != 40)
		goto drop;
	plen = be16(pkt+4);
	if(plen < Ilhdrlen || Ilip6len+plen > len)
		goto drop;
	src = pkt+8;
	dst = pkt+24;
	ih = pkt+Ilip6len;
	illen = be16(ih+2);
	if(illen < Ilhdrlen || illen > plen)
		goto drop;
	if(csum(ih, illen) != 0)
		goto drop;
	type = ih[4];
	spec = ih[5];
	sp = be16(ih+6);
	dp = be16(ih+8);
	id = be32(ih+10);
	ack = be32(ih+14);
	data = ih+Ilhdrlen;
	dlen = illen-Ilhdrlen;

	il = findconn(dst, dp, src, sp);
	if(il == nil && type == IlSync){
		il = findlistener(dst, dp);
		if(il != nil){
			n = newconv();
			if(n != nil){
				n->listener = 0;
				n->state = Csyncee;
				n->lport = dp;
				n->rport = sp;
				ip6copy(n->laddr, dst);
				ip6copy(n->raddr, src);
				n->start = (u32)rand();
				n->next = n->start+1;
				n->recvd = id;
				n->rstart = id;
				n->window = 20;
				n->cwind = 2;
				n->ssthresh = 20;
				iloutput(n, IlSync, n->start, n->recvd, nil, 0, 0);
				iltimeout(n, 1000);
				if(nbsendp(il->acceptq, n) != 1)
					(void)0;
				il = n;
			}
		}
	}
	if(il == nil)
		goto drop;

	il->lastrecv = (u64)(nsec()/1000000);
	switch(il->state){
	case Csyncer:
		if(type == IlSync && ack == il->start){
			il->recvd = id;
			il->rstart = id;
			il->state = Cestablished;
			il->rexmit = 0;
			il->timeout = 0;
			iloutput(il, IlAck, il->next, il->recvd, nil, 0, 0);
			ilpump(il);
		} else if(type == IlClose && ack == il->start){
			il->state = Cclosed;
			snprint(il->err, sizeof il->err, "connection rejected");
			chanclose(il->rq);
		}
		break;
	case Csyncee:
		if(type == IlSync && id == il->rstart && ack == 0){
			iloutput(il, IlSync, il->start, il->recvd, nil, 0, 0);
			iltimeout(il, 1000);
		} else if(type == IlAck && ack == il->start){
			il->state = Cestablished;
			il->rexmit = 0;
			il->timeout = 0;
			ilpump(il);
		} else if(type == IlData && ack == il->start){
			il->state = Cestablished;
			if(id == il->recvd + 1){
				il->recvd = id;
				deliver(il, data, dlen);
				ilrxpull(il);
				iloutput(il, IlAck, il->next, il->recvd, nil, 0, 0);
			} else if(id > il->recvd + 1)
				ilrxadd(il, id, data, dlen);
			ilpump(il);
		}
		break;
	case Cestablished:
		if(type == IlData){
			if(id == il->recvd + 1){
				il->recvd = id;
				deliver(il, data, dlen);
				ilrxpull(il);
				iloutput(il, IlAck, il->next, il->recvd, nil, 0, 0);
			} else if(id <= il->recvd){
				iloutput(il, IlAck, il->next, il->recvd, nil, 0, 0);
			} else
				ilrxadd(il, id, data, dlen);
		} else if(type == IlAck){
			ilackto(il, ack);
		} else if(type == IlDataQuery){
			ilackto(il, ack);
			iloutput(il, IlState, il->next, il->recvd, nil, 0, spec);
		} else if(type == IlQuery || type == IlState){
			if(type == IlState)
				ilackto(il, ack);
			iloutput(il, IlState, il->next, il->recvd, nil, 0, spec);
		} else if(type == IlClose){
			il->state = Cclosed;
			iloutput(il, IlClose, il->next, il->recvd, nil, 0, 0);
			ilclear(il);
			chanclose(il->rq);
		}
		break;
	}
drop:
	free(pkt);
}

static void
iltick(void)
{
	Iltx *tx;
	u8 *pkt;
	int len;
	u64 now;
	int i;
	Il *il;

	now = (u64)(nsec()/1000000);
	for(i = 0; i < Maxconv; i++){
		il = convs[i];
		if(il == nil)
			continue;
		if((il->state == Csyncer || il->state == Csyncee) && il->timeout && now >= il->timeout){
			il->rexmit++;
			if(il->rexmit > 16){
				il->state = Cclosed;
				snprint(il->err, sizeof il->err, "connection timed out");
				ilclear(il);
				chanclose(il->rq);
				continue;
			}
			iloutput(il, IlSync, il->start, il->recvd, nil, 0, 0);
			iltimeout(il, 250 << (il->rexmit > 5 ? 5 : il->rexmit));
		} else if(il->state == Cclosing && il->timeout && now >= il->timeout){
			il->rexmit++;
			if(il->rexmit > 8){
				il->state = Cclosed;
				ilclear(il);
				chanclose(il->rq);
				continue;
			}
			iloutput(il, IlClose, il->next, il->recvd, nil, 0, 0);
			iltimeout(il, 500 << (il->rexmit > 4 ? 4 : il->rexmit));
		} else if(il->state == Cestablished && il->timeout && now >= il->timeout && il->unacked != nil){
			tx = il->unacked;
			il->rexmit++;
			il->rxtot++;
			if(il->rexmit > 16){
				il->state = Cclosed;
				snprint(il->err, sizeof il->err, "connection timed out");
				ilclear(il);
				chanclose(il->rq);
				continue;
			}
			len = tx->len;
			pkt = malloc(len);
			if(pkt != nil){
				memmove(pkt, tx->pkt, len);
				ilsendpkt(il, pkt, len);
			}
			iltimeout(il, 250 << (il->rexmit > 5 ? 6 : il->rexmit));
		}
	}
}

static void
ilworker(void *v)
{
	Ilev *ev;
	Il *il;
	u8 addr[16];

	USED(v);
	for(;;){
		if(recv(ilev, &ev) <= 0)
			break;
		il = ev->il;
		switch(ev->type){
		case EvInput:
			ilinputsync(ev->pkt, ev->len);
			ev->pkt = nil;
			break;
		case EvConnect:
		case EvAnnounce:
			if(il == nil || parseip6(ev->addr, addr) < 0)
				break;
			ip6copy(il->laddr, ifc.addr);
			if(ev->type == EvAnnounce){
				ip6copy(il->laddr, addr);
				il->lport = ev->port;
				il->state = Clistening;
				il->listener = 1;
			} else {
				il->lport = allocport();
				il->rport = ev->port;
				ip6copy(il->raddr, addr);
				il->state = Csyncer;
				il->start = (u32)rand();
				il->next = il->start+1;
				il->recvd = 0;
				il->rstart = 0;
				il->window = 20;
				il->cwind = 2;
				il->ssthresh = 20;
				iloutput(il, IlSync, il->start, 0, nil, 0, 0);
				iltimeout(il, 250);
			}
			break;
		case EvWrite:
			if(il != nil && ev->pkt != nil)
				ilqueue(il, ev->pkt, ev->len);
			break;
		case EvClose:
			if(il != nil)
				ilclose(il);
			break;
		case EvTick:
			iltick();
			break;
		}
		free(ev->pkt);
		free(ev);
	}
}

static void
ilticker(void *v)
{
	Ilev *ev;

	USED(v);
	for(;;){
		sleep(20);
		ev = mallocz(sizeof(*ev), 1);
		if(ev == nil)
			continue;
		ev->type = EvTick;
		if(ilev == nil || nbsendp(ilev, ev) != 1)
			free(ev);
	}
}

static Il *
newconv(void)
{
	Il *il;
	int idx;
	char name[32];
	Node *n;
	File *d, *f;

	lock(&convlock);
	for(idx = 0; idx < Maxconv; idx++)
		if(convs[idx] == nil)
			break;
	if(idx == Maxconv){
		unlock(&convlock);
		return nil;
	}
	il = mallocz(sizeof(*il), 1);
	if(il == nil){
		unlock(&convlock);
		return nil;
	}
	il->idx = idx;
	il->state = Cclosed;
	il->rq = chancreate(sizeof(Packet*), 64);
	il->acceptq = chancreate(sizeof(Il*), Maxlisten);
	if(il->rq == nil || il->acceptq == nil){
		unlock(&convlock);
		return nil;
	}
	snprint(name, sizeof name, "%d", idx);
	n = mallocz(sizeof(*n), 1);
	n->kind = Nconvdir;
	n->il = il;
	d = createfile(ildir, name, "net", DMDIR|0777, n);
	if(d == nil){
		unlock(&convlock);
		return nil;
	}
	il->dir = d;
#define MKFILE(nm, k) do { \
	Node *x = mallocz(sizeof(*x), 1); x->kind = (k); x->il = il; \
	f = createfile(d, (nm), "net", 0666, x); (void)f; \
} while(0)
	MKFILE("ctl", Nctl);
	MKFILE("data", Ndata);
	MKFILE("listen", Nlisten);
	MKFILE("local", Nlocal);
	MKFILE("remote", Nremote);
	MKFILE("status", Nstatus);
	MKFILE("err", Nerr);
#undef MKFILE
	convs[idx] = il;
	unlock(&convlock);
	return il;
}

static int
connsetup(Il *il, char *dest, int announce)
{
	char *bang;
	Ilev *ev;
	u8 addr[16];
	int port;

	bang = strrchr(dest, '!');
	if(bang == nil)
		return -1;
	*bang++ = 0;
	if(parseip6(dest, addr) < 0)
		return -1;
	port = atoi(bang);
	if(port <= 0 || port > Maxport)
		return -1;
	ev = mallocz(sizeof(*ev), 1);
	if(ev == nil)
		return -1;
	ev->type = announce ? EvAnnounce : EvConnect;
	ev->il = il;
	ev->port = port;
	snprint(ev->addr, sizeof ev->addr, "%s", dest);
	if(ilev == nil || nbsendp(ilev, ev) != 1){
		free(ev);
		return -1;
	}
	return 0;
}

static int
ilwrite(Il *il, u8 *data, int n)
{
	Ilev *ev;

	if(n < 0)
		return -1;
	if(n == 0)
		return 0;
	ev = mallocz(sizeof(*ev), 1);
	if(ev == nil)
		return -1;
	ev->type = EvWrite;
	ev->il = il;
	ev->pkt = malloc(n);
	if(ev->pkt == nil){
		free(ev);
		return -1;
	}
	memmove(ev->pkt, data, n);
	ev->len = n;
	if(ilev == nil || nbsendp(ilev, ev) != 1){
		free(ev->pkt);
		free(ev);
		return -1;
	}
	return n;
}

static void
ilcloseevent(Il *il)
{
	Ilev *ev;

	ev = mallocz(sizeof(*ev), 1);
	if(ev == nil)
		return;
	ev->type = EvClose;
	ev->il = il;
	if(ilev == nil || nbsendp(ilev, ev) != 1)
		free(ev);
}

static int
dirgen(int i, Dir *d, void *aux)
{
	File *dir = aux;
	File *f = nil;
	char *name = nil;
	char buf[32];
	int j, k;

	if(dir == rootdir){
		if(i == 0) name = "il";
		else if(i == 1) name = "ipifc";
		else return -1;
	} else if(dir == ildir){
		if(i == 0) name = "clone";
		else {
			name = nil;
			lock(&convlock);
			j = 0;
			for(k = 0; k < Maxconv; k++){
				if(convs[k] == nil || convs[k]->dir == nil)
					continue;
				if(j == i-1){
					snprint(buf, sizeof buf, "%d", convs[k]->idx);
					name = buf;
					f = convs[k]->dir;
					break;
				}
				j++;
			}
			unlock(&convlock);
			if(name == nil)
				return -1;
		}
	} else if(dir == ipifcdir){
		if(i == 0) name = "clone";
		else if(i == 1) name = "0";
		else return -1;
		f = walkfile(dir, name);
	} else if(dir == ifcdir){
		static char *names[] = {"ctl", "pkt", "local", "status"};
		if(i < 0 || (u32int)i >= (u32int)nelem(names)) return -1;
		name = names[i];
	} else {
		static char *names[] = {"ctl", "data", "listen", "local", "remote", "status", "err"};
		if(i < 0 || (u32int)i >= (u32int)nelem(names)) return -1;
		name = names[i];
	}
	if(f == nil && name != nil)
		f = walkfile(dir, name);
	memset(d, 0, sizeof *d);
	d->name = strdup(name);
	d->uid = strdup("net");
	d->gid = strdup("net");
	d->muid = strdup("net");
	if(f != nil){
		d->qid = f->qid;
		d->mode = f->mode;
		d->length = f->length;
	} else {
		d->qid.type = QTFILE;
		d->qid.path = 0;
		d->mode = 0666;
	}
	return 0;
}

static void
fsread(Req *r)
{
	File *fidfile;
	Node *n;
	char buf[512];
	u32 count;
	Il *il;
	Ifc *ifc0;
	Packet *p;

	fidfile = r->fid->file;
	n = fidfile->aux;
	if((fidfile->mode & DMDIR) != 0){
		dirread9p(r, dirgen, fidfile);
		respond(r, nil);
		return;
	}
	if(n == nil){
		respond(r, "bad file");
		return;
	}
	count = r->ifcall.count;
	switch(n->kind){
	case Nclone:
		if(r->fid->aux == nil){
			il = newconv();
			if(il == nil){
				respond(r, "out of conversations");
				return;
			}
			r->fid->aux = il;
		} else
			il = r->fid->aux;
		snprint(buf, sizeof buf, "%d", il->idx);
		readstr(r, buf);
		respond(r, nil);
		return;
	case Ndata:
		il = n->il;
		if(recv(il->rq, &p) <= 0){
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		if(count > (u32)p->len)
			count = p->len;
		memmove(r->ofcall.data, p->data, count);
		r->ofcall.count = count;
		free(p);
		respond(r, nil);
		return;
	case Nlisten:
		il = n->il;
		if(recv(il->acceptq, &il) <= 0){
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		snprint(buf, sizeof buf, "%d", il->idx);
		readstr(r, buf);
		respond(r, nil);
		return;
	case Nlocal:
		il = n->il;
		formatip6(il->laddr, buf, 64);
		strcat(buf, "!");
		snprint(buf+strlen(buf), 32, "%d", il->lport);
		strcat(buf, "\n");
		readstr(r, buf);
		respond(r, nil);
		return;
	case Nremote:
		il = n->il;
		if(il->rport == 0){
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		formatip6(il->raddr, buf, 64);
		strcat(buf, "!");
		snprint(buf+strlen(buf), 32, "%d", il->rport);
		strcat(buf, "\n");
		readstr(r, buf);
		respond(r, nil);
		return;
	case Nstatus:
		il = n->il;
		snprint(buf, sizeof buf, "%s\n",
			il->state == Cestablished ? "Established" :
			il->state == Clistening ? "Listen" :
			il->state == Cclosed ? "Closed" : "Connecting");
		readstr(r, buf);
		respond(r, nil);
		return;
	case Nerr:
		il = n->il;
		if(il->err[0] == 0){
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		readstr(r, il->err);
		respond(r, nil);
		return;
	case Npkt:
		ifc0 = n->ifc;
		if(recv(ifc0->out, &p) <= 0){
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		if(count > (u32)p->len)
			count = p->len;
		memmove(r->ofcall.data, p->data, count);
		r->ofcall.count = count;
		free(p);
		respond(r, nil);
		return;
	case Nifcclone:
		readstr(r, "0");
		respond(r, nil);
		return;
	case Nifcctl:
		readstr(r, "pkt\n");
		respond(r, nil);
		return;
	case Nifcdir:
	case Nroot:
	case Nildir:
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}
	respond(r, "unsupported read");
}

static void
fswrite(Req *r)
{
	Node *n;
	Il *il;
	char cmd[1024];
	int count;
	u8 *data;

	n = r->fid->file->aux;
	data = (u8*)r->ifcall.data;
	count = r->ifcall.count;
	if(n == nil){
		respond(r, "bad file");
		return;
	}
	switch(n->kind){
	case Nclone:
		il = r->fid->aux;
		if(il == nil){
			respond(r, "clone not opened");
			return;
		}
		goto ctl;
	case Nctl:
		il = n->il;
ctl:
		if(count >= (int)sizeof cmd)
			count = sizeof cmd - 1;
		memmove(cmd, data, count);
		cmd[count] = 0;
		while(count > 0 && (cmd[count-1] == '\n' || cmd[count-1] == ' '))
			cmd[--count] = 0;
		if(strncmp(cmd, "connect ", 8) == 0){
			if(connsetup(il, cmd+8, 0) < 0){
				respond(r, "bad address");
				return;
			}
		} else if(strncmp(cmd, "announce ", 9) == 0){
			if(connsetup(il, cmd+9, 1) < 0){
				respond(r, "bad address");
				return;
			}
		} else if(strcmp(cmd, "hangup") == 0 || strcmp(cmd, "close") == 0){
			ilcloseevent(il);
		} else {
			respond(r, "unsupported ctl command");
			return;
		}
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	case Ndata:
		il = n->il;
		if(ilwrite(il, data, count) < 0){
			respond(r, il->err[0] ? il->err : "not connected");
			return;
		}
		r->ofcall.count = count;
		respond(r, nil);
		return;
	case Nlisten:
		/* accept/reject are advisory; accept is followed by opening data. */
		r->ofcall.count = count;
		respond(r, nil);
		return;
	case Npkt:
		if(count <= 0 || count > Maxpkt){
			respond(r, "bad packet");
			return;
		}
		{
			u8 *pkt = malloc(count);
			if(pkt == nil){
				respond(r, "out of memory");
				return;
			}
			memmove(pkt, data, count);
			ilinput(pkt, count);
		}
		r->ofcall.count = count;
		respond(r, nil);
		return;
	case Nifcctl:
		if(count >= (int)sizeof cmd)
			count = sizeof cmd - 1;
		memmove(cmd, data, count);
		cmd[count] = 0;
		if(strncmp(cmd, "bind pkt", 8) == 0)
			ifc.pktbound = 1;
		else if(strncmp(cmd, "add ", 4) == 0){
			char *p = cmd+4;
			char *sp = strchr(p, ' ');
			if(sp != nil) *sp = 0;
			parseip6(p, ifc.addr);
		}
		r->ofcall.count = count;
		respond(r, nil);
		return;
	}
	respond(r, "unsupported write");
}

static void
destroyfile(File *f)
{
	Node *n;

	n = f->aux;
	if(n != nil)
		free(n);
}

static void
fsopen(Req *r)
{
	respond(r, nil);
}

static void
netdmain(int argc, char **argv)
{
	char *srvname;
	int fd[2];
	File *f;

	USED(argc);
	USED(argv);
	srvname = "netd";
	rand(); /* plan9 rand has no seed argument */

	memset(&ifc, 0, sizeof ifc);
	ifc.idx = 0;
	parseip6("::1", ifc.addr);
	ifc.prefix = 128;
	ifc.out = chancreate(sizeof(Packet*), 64);

	tree = alloctree("net", "net", DMDIR|0777, destroyfile);
	tree->qidgen = 1;
	rootdir = tree->root;
	rootnode = mallocz(sizeof(*rootnode), 1);
	rootnode->kind = Nroot;
	rootdir->aux = rootnode;

	ildir = createfile(rootdir, "il", "net", DMDIR|0777, nil);
	ilnode = mallocz(sizeof(*ilnode), 1);
	ilnode->kind = Nildir;
	ildir->aux = ilnode;
	f = createfile(ildir, "clone", "net", 0666, nil);
	clonenode = mallocz(sizeof(*clonenode), 1);
	clonenode->kind = Nclone;
	f->aux = clonenode;

	ipifcdir = createfile(rootdir, "ipifc", "net", DMDIR|0777, nil);
	ipifcnode = mallocz(sizeof(*ipifcnode), 1);
	ipifcnode->kind = Nipifcdir;
	ipifcdir->aux = ipifcnode;
	{
		Node *x = mallocz(sizeof(*x), 1);
		x->kind = Nifcclone;
		createfile(ipifcdir, "clone", "net", 0666, x);
	}
	ifcdir = createfile(ipifcdir, "0", "net", DMDIR|0777, nil);
	ifcnode = mallocz(sizeof(*ifcnode), 1);
	ifcnode->kind = Nifcdir;
	ifcdir->aux = ifcnode;
	{
		Node *x;
		x = mallocz(sizeof(*x), 1); x->kind = Nifcctl; x->ifc = &ifc;
		createfile(ifcdir, "ctl", "net", 0666, x);
		x = mallocz(sizeof(*x), 1); x->kind = Npkt; x->ifc = &ifc;
		createfile(ifcdir, "pkt", "net", 0666, x);
	}

	memset(&fs, 0, sizeof fs);
	fs.tree = tree;
	fs.threaded = 1;
	fs.open = fsopen;
	fs.read = fsread;
	fs.write = fswrite;
	fs.destroyfid = nil;

	ilev = chancreate(sizeof(Ilev*), 512);
	if(ilev == nil)
		sysfatal("il event channel");
	if(proccreate(ilworker, nil, 65536) < 0)
		sysfatal("il worker");
	if(proccreate(ilticker, nil, 8192) < 0)
		sysfatal("il ticker");

	if(pipe(fd) < 0)
		sysfatal("pipe: %r");
	if(postfd(srvname, fd[0]) < 0)
		sysfatal("postfd: %r");
	close(fd[0]);
	fs.infd = fs.outfd = fd[1];
	srv(&fs);
	threadexits(nil);
}

void
threadmain(int argc, char **argv)
{
	netdmain(argc, argv);
}
