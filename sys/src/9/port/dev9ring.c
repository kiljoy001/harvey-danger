/*
 * dev9ring.c - Duplex Lockless SPSC 9P Ring Transport (#~)
 *
 * Provides tagged 9P submission and completion rings. The host endpoint is
 * shared with 9vm; named local endpoints connect ordinary user processes.
 *
 * Mount contract:
 *     bind -a '#~' /dev
 * Endpoints:
 *     #~/client   Client write (T-slot submit), read (R-comp poll)
 *     #~/server   Server read (T-slot consume), write (R-comp complete)
 *     #~/ctl      Ring channel status and counters
 *     #~!name/announce  Hold a named local service open
 *     #~!name/listen    Accept one client; becomes a 9P byte stream
 *     #~name/connect   Connect to that service; becomes a 9P byte stream
 */

#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "9ring.h"

typedef struct RingDev RingDev;
typedef struct RingService RingService;
typedef struct RingConn RingConn;

struct RingConn {
	QLock clienttx;
	QLock clientrx;
	QLock servertx;
	QLock serverrx;
	Lock l;
	Rendez rclient;
	Rendez rserver;
	Rendez rfree;
	RingConn *next;
	P9HostShared *shared;
	u64 id;
	u32 free;
	u16 tag[P9_HOST_SLOTS];
	u8 state[P9_HOST_SLOTS];
	int ref;
	int clientopen;
	int serveropen;
	int clientcur;
	int servercur;
	u32 clientpos;
	u32 clientlen;
	u32 serverpos;
	u32 serverlen;
};

struct RingService {
	QLock ql;
	Rendez rlisten;
	RingService *next;
	RingConn *pending;
	RingConn *last;
	char *name;
	char *owner;
	int ref;
	int announced;
};

struct RingDev {
	QLock ql;
	int ref;
	u64 path;
	int client_open;
	int server_open;
	int server_role;

	P9Channel *chan;
	RingService *service;
	RingConn *conn;

	Rendez r_client;
	Rendez r_server;

	u64 t_submitted;
	u64 t_consumed;
	u64 r_completed;
	u64 r_polled;
};

static struct {
	Lock l;
	u64 path;
} ringalloc;
static struct {
	Lock l;
	int active;
} ringconns;
static QLock servicelk;
static RingService *services;

extern u64 ringhostpa;
static P9HostShared *hostshared;
static RingDev hostdev;
static QLock hosttxql;
static QLock hostrxql;
static Lock hostslotlock;
static u32 hostfree = (1U << P9_HOST_SLOTS) - 1;
static int hostopen;
static int hostcur = -1;
static u32 hostrp, hostlen;

static int
hosthasrequest(void *unused)
{
	int pending;

	USED(unused);
	lock(&hostslotlock);
	pending = hostfree != (1U << P9_HOST_SLOTS) - 1;
	unlock(&hostslotlock);
	return pending;
}

static void
hostpause(Rendez *r, int spins)
{
	Proc *up = externup();
	if(spins < 50){
		microdelay(2);
		return;
	}
	if(waserror())
		return;
	tsleep(r, return0, nil, 1);
	poperror();
}

enum {
	Qdir = 0,
	Qctl,
	Qclient,
	Qserver,
	Qring,
	Qhost,
	Qannounce,
	Qlisten,
	Qconnect,
	Qlocalclient,
	Qlocalserver,
};

static Dirtab ringdir[] = {
	{".",      {Qdir},    0, DMDIR | 0500},
	{"ctl",    {Qctl},    0, 0666},
	{"client", {Qclient}, 0, 0666},
	{"server", {Qserver}, 0, 0666},
	{"ring",   {Qring},   sizeof(P9Channel), 0666},
	{"host",   {Qhost},   0, 0660},
};
#define NRINGDIR 6

static Dirtab ringnamedir[] = {
	{".", {Qdir}, 0, DMDIR | 0500},
	{"announce", {Qannounce}, 0, 0400},
	{"listen", {Qlisten}, 0, 0600},
	{"connect", {Qconnect}, 0, 0600},
};
#define NNAMEDIR 4

#define RINGTYPE(x) (((u32)(x)) & 0x0f)
#define RINGID(x)   (((u64)(x)) >> 4)
#define RINGQID(i, t) ((((u64)(i)) << 4) | (t))

enum {
	Slotfree,
	Slotsubmitted,
	Slotserving,
	Slotwaiting,
	Slotreplied,
	Maxlocalconns = 16,
};

static u64
ringnextid(void)
{
	u64 id;

	lock(&ringalloc.l);
	id = ++ringalloc.path;
	unlock(&ringalloc.l);
	if(id == 0 || id >= (1ULL << 60))
		error("9ring: connection IDs exhausted");
	return id;
}

static int
ringvalidname(char *name)
{
	int n;
	char *p;

	n = 0;
	for(p = name; *p; p++){
		if(++n > 63 || !((*p >= 'a' && *p <= 'z') ||
		   (*p >= 'A' && *p <= 'Z') ||
		   (*p >= '0' && *p <= '9') ||
		   *p == '_' || *p == '-' || *p == '.'))
			return 0;
	}
	return n != 0;
}

static RingService *
ringserviceget(char *name, int create)
{
	Proc *up;
	RingService *s;

	up = externup();
	if(!ringvalidname(name))
		error(Ebadarg);
	qlock(&servicelk);
	for(s = services; s != nil; s = s->next)
		if(strcmp(s->name, name) == 0)
			break;
	if(s == nil && create){
		s = mallocz(sizeof(*s), 1);
		if(s != nil){
			kstrdup(&s->name, name);
			kstrdup(&s->owner, up->user);
			s->next = services;
			services = s;
		}
	}
	if(s != nil){
		if(strcmp(s->owner, up->user) != 0){
			qunlock(&servicelk);
			error(Eperm);
		}
		s->ref++;
	}
	qunlock(&servicelk);
	if(s == nil)
		error(create ? Enomem : Enonexist);
	return s;
}

static void
ringserviceput(RingService *s)
{
	RingService **p;
	int dead;

	qlock(&servicelk);
	dead = --s->ref == 0;
	if(dead){
		for(p = &services; *p != nil; p = &(*p)->next)
			if(*p == s){
				*p = s->next;
				break;
			}
	}
	qunlock(&servicelk);
	if(dead){
		free(s->name);
		free(s->owner);
		free(s);
	}
}

static RingConn *
ringnewconn(void)
{
	RingConn *r;

	lock(&ringconns.l);
	if(ringconns.active >= Maxlocalconns){
		unlock(&ringconns.l);
		error("9ring: too many local connections");
	}
	ringconns.active++;
	unlock(&ringconns.l);
	r = mallocz(sizeof(*r), 1);
	if(r == nil){
		lock(&ringconns.l);
		ringconns.active--;
		unlock(&ringconns.l);
		exhausted("9ring connection");
	}
	r->shared = mallocz(sizeof(*r->shared), 1);
	if(r->shared == nil){
		free(r);
		lock(&ringconns.l);
		ringconns.active--;
		unlock(&ringconns.l);
		exhausted("9ring buffers");
	}
	r->id = ringnextid();
	r->ref = 2;
	r->clientopen = 1;
	r->serveropen = 1;
	r->free = (1U << P9_HOST_SLOTS) - 1;
	r->clientcur = -1;
	r->servercur = -1;
	return r;
}

static void
ringconnput(RingConn *r)
{
	int dead;

	lock(&r->l);
	dead = --r->ref == 0;
	unlock(&r->l);
	if(dead){
		free(r->shared);
		free(r);
		lock(&ringconns.l);
		ringconns.active--;
		unlock(&ringconns.l);
	}
}

static void
ringdevput(RingDev *r)
{
	int dead;

	qlock(&r->ql);
	dead = --r->ref == 0;
	qunlock(&r->ql);
	if(dead){
		if(r->service != nil)
			ringserviceput(r->service);
		free(r->chan);
		free(r);
	}
}

static int
ringpending(void *v)
{
	RingService *s;

	s = v;
	return s->pending != nil || !s->announced;
}

static int
ringhasfree(void *v)
{
	RingConn *r;

	r = v;
	return r->free != 0 || !r->serveropen;
}

static int
ringhasnormalfree(void *v)
{
	RingConn *r;
	u32 free;

	r = v;
	free = r->free;
	return (free & (free - 1)) != 0 || !r->serveropen;
}

static int
ringhasrequest(void *v)
{
	RingConn *r;

	r = v;
	return r->shared->channel.t_ring.head !=
		r->shared->channel.t_ring.tail || !r->clientopen;
}

static int
ringhasreply(void *v)
{
	RingConn *r;

	r = v;
	return r->shared->channel.r_ring.head !=
		r->shared->channel.r_ring.tail || !r->serveropen;
}

static i32
ringconnread(RingConn *r, int client, void *va, i32 n)
{
	Proc *up = externup();
	P9Slot slot;
	P9Comp comp;
	P9HostBuffer *b;
	QLock *ql;
	Rendez *ready;
	u32 *pos, *len, wirelen;
	int *cur;
	i32 count;

	if(n <= 0)
		return 0;
	ql = client ? &r->clientrx : &r->serverrx;
	ready = client ? &r->rclient : &r->rserver;
	cur = client ? &r->clientcur : &r->servercur;
	pos = client ? &r->clientpos : &r->serverpos;
	len = client ? &r->clientlen : &r->serverlen;
	qlock(ql);
	if(waserror()){
		qunlock(ql);
		nexterror();
	}
	while(*cur < 0){
		if(client){
			if(p9_rring_poll(&r->shared->channel.r_ring, &comp)){
				if(comp.result >= P9_HOST_SLOTS ||
				   comp.count < 7 || comp.count > P9_HOST_MSIZE ||
				   comp.status != 0 || r->state[comp.result] != Slotreplied ||
				   r->tag[comp.result] != comp.tag)
					error("9ring: invalid service reply");
				*cur = comp.result;
				*len = comp.count;
				break;
			}
			if(!r->serveropen){
				poperror();
				qunlock(ql);
				return 0;
			}
			sleep(ready, ringhasreply, r);
		}else{
			if(p9_tring_consume(&r->shared->channel.t_ring, &slot)){
				if(slot.buf_addr >= P9_HOST_SLOTS ||
				   slot.count < 7 || slot.count > P9_HOST_MSIZE ||
				   r->state[slot.buf_addr] != Slotsubmitted ||
				   r->tag[slot.buf_addr] != slot.tag)
					error("9ring: invalid service request");
				r->state[slot.buf_addr] = Slotserving;
				*cur = slot.buf_addr;
				*len = slot.count;
				break;
			}
			if(!r->clientopen){
				poperror();
				qunlock(ql);
				return 0;
			}
			sleep(ready, ringhasrequest, r);
		}
	}
	b = &r->shared->buffer[*cur];
	if(client){
		memmove(&wirelen, b->response, sizeof(wirelen));
		if(wirelen != *len || b->response[5] != (u8)r->tag[*cur] ||
		   b->response[6] != (u8)(r->tag[*cur] >> 8))
			error("9ring: invalid service response framing");
	}else{
		memmove(&wirelen, b->request, sizeof(wirelen));
		if(wirelen != *len)
			error("9ring: invalid service request framing");
	}
	count = *len - *pos;
	if(count > n)
		count = n;
	memmove(va, (client ? b->response : b->request) + *pos, count);
	*pos += count;
	if(*pos == *len){
		lock(&r->l);
		if(client){
			r->state[*cur] = Slotfree;
			r->free |= 1U << *cur;
		}else
			r->state[*cur] = Slotwaiting;
		unlock(&r->l);
		if(client)
			wakeup(&r->rfree);
		*cur = -1;
		*pos = 0;
		*len = 0;
	}
	poperror();
	qunlock(ql);
	return count;
}

static i32
ringconnwrite(RingConn *r, int client, void *va, i32 n)
{
	Proc *up = externup();
	P9Slot slot;
	P9Comp comp;
	QLock *ql;
	u8 *m;
	u32 wirelen;
	u16 tag;
	int i, j, allocated;

	if(n < 7 || n > P9_HOST_MSIZE)
		error("9ring: invalid service message length");
	m = va;
	memmove(&wirelen, m, sizeof(wirelen));
	if(wirelen != n)
		error("9ring: service message length mismatch");
	tag = m[5] | (m[6] << 8);
	ql = client ? &r->clienttx : &r->servertx;
	qlock(ql);
	if(waserror()){
		qunlock(ql);
		nexterror();
	}
	if(client){
		for(;;){
			allocated = 0;
			lock(&r->l);
			if(!r->serveropen){
				unlock(&r->l);
				error(Eshutdown);
			}
			for(j = 0; j < P9_HOST_SLOTS; j++)
				if(r->state[j] != Slotfree && r->tag[j] == tag){
					unlock(&r->l);
					error("9ring: duplicate request tag");
				}
			for(i = 0; i < P9_HOST_SLOTS; i++)
				if(r->free & (1U << i))
					break;
			if(i < P9_HOST_SLOTS &&
			   (m[4] == Tflush || (r->free & (r->free - 1)) != 0)){
				r->free &= ~(1U << i);
				r->tag[i] = tag;
				r->state[i] = Slotsubmitted;
				allocated = 1;
			}
			unlock(&r->l);
			if(allocated)
				break;
			poperror();
			qunlock(ql);
			sleep(&r->rfree,
				m[4] == Tflush ? ringhasfree : ringhasnormalfree, r);
			qlock(ql);
			if(waserror()){
				qunlock(ql);
				nexterror();
			}
		}
		memmove(r->shared->buffer[i].request, m, n);
		memset(&slot, 0, sizeof(slot));
		slot.tag = tag;
		slot.type = m[4];
		slot.count = n;
		slot.buf_addr = i;
		if(p9_tring_submit(&r->shared->channel.t_ring, &slot) < 0)
			error("9ring: service request ring full");
		wakeup(&r->rserver);
	}else{
		lock(&r->l);
		for(i = 0; i < P9_HOST_SLOTS; i++)
			if(r->state[i] == Slotwaiting && r->tag[i] == tag)
				break;
		if(i == P9_HOST_SLOTS || !r->clientopen){
			unlock(&r->l);
			error("9ring: reply has no waiting request");
		}
		r->state[i] = Slotreplied;
		unlock(&r->l);
		memmove(r->shared->buffer[i].response, m, n);
		memset(&comp, 0, sizeof(comp));
		comp.tag = tag;
		comp.type = m[4];
		comp.count = n;
		comp.result = i;
		if(p9_rring_complete(&r->shared->channel.r_ring, &comp) < 0)
			error("9ring: service reply ring full");
		wakeup(&r->rclient);
	}
	poperror();
	qunlock(ql);
	return n;
}

static void
ringinit(void)
{
	if(ringhostpa != 0){
		hostshared = KADDR(ringhostpa);
		if(hostshared->magic != P9_HOST_MAGIC ||
		   hostshared->version != P9_HOST_VERSION){
			print("9ring: invalid host shared memory header\n");
			hostshared = nil;
		}else
			print("9ring: host transport ready at %#llx\n", ringhostpa);
	}
}

static Chan *
ringattach(char *spec)
{
	RingDev *r;
	Chan *c;
	RingService *s;
	int serverrole;

	s = nil;
	serverrole = 0;
	if(spec != nil && *spec != 0){
		serverrole = *spec == '!';
		s = ringserviceget(spec + serverrole, serverrole);
	}
	c = devattach('~', spec);
	r = mallocz(sizeof(RingDev), 1);
	if(r == nil){
		if(s != nil)
			ringserviceput(s);
		cclose(c);
		exhausted("memory");
	}

	r->service = s;
	r->server_role = serverrole;
	if(s == nil)
		r->chan = mallocz(sizeof(P9Channel), 1);
	if(s == nil && r->chan == nil){
		free(r);
		cclose(c);
		exhausted("memory");
	}

	r->ref = 1;
	r->path = ringnextid();

	mkqid(&c->qid, RINGQID(r->path, Qdir), 0, QTDIR);
	c->aux = r;
	c->devno = 0;
	return c;
}

static int
ringgen(Chan *c, char *d, Dirtab *tab, int ntab, int i, Dir *dp)
{
	Qid q;

	if(i == DEVDOTDOT){
		devdir(c, c->qid, "#~", 0, eve, DMDIR | 0555, dp);
		return 1;
	}
	i++;
	if(tab == 0 || i >= ntab)
		return -1;

	tab += i;
	if(tab->qid.path == Qhost && hostshared == nil)
		return -1;
	mkqid(&q, RINGQID(RINGID(c->qid.path), tab->qid.path), 0, QTFILE);
	devdir(c, q, tab->name, tab->length, eve, tab->perm, dp);
	return 1;
}

static Walkqid *
ringwalk(Chan *c, Chan *nc, char **name, int nname)
{
	Walkqid *wq;
	RingDev *r;

	r = c->aux;
	if((RINGTYPE(c->qid.path) == Qlocalclient ||
	    RINGTYPE(c->qid.path) == Qlocalserver) && c->flag & COPEN)
		error(Eperm);
	if(r->service != nil)
		wq = devwalk(c, nc, name, nname, ringnamedir, NNAMEDIR, ringgen);
	else
		wq = devwalk(c, nc, name, nname, ringdir, NRINGDIR, ringgen);
	if(wq != nil && wq->clone != nil && wq->clone != c){
		r = c->aux;
		qlock(&r->ql);
		r->ref++;
		if(c->flag & COPEN){
			switch(RINGTYPE(c->qid.path)){
			case Qclient:
				r->client_open++;
				break;
			case Qserver:
				r->server_open++;
				break;
			}
		}
		qunlock(&r->ql);
	}
	return wq;
}

static i32
ringstat(Chan *c, u8 *db, i32 n)
{
	Dir dir;

	switch(RINGTYPE(c->qid.path)){
	case Qdir:
		devdir(c, c->qid, ".", 0, eve, DMDIR | 0555, &dir);
		break;
	case Qctl:
		devdir(c, c->qid, "ctl", 0, eve, 0666, &dir);
		break;
	case Qclient:
		devdir(c, c->qid, "client", 0, eve, 0666, &dir);
		break;
	case Qserver:
		devdir(c, c->qid, "server", 0, eve, 0666, &dir);
		break;
	case Qring:
		devdir(c, c->qid, "ring", sizeof(P9Channel), eve, 0666, &dir);
		break;
	case Qhost:
		devdir(c, c->qid, "host", 0, eve, 0660, &dir);
		break;
	case Qannounce:
		devdir(c, c->qid, "announce", 0, eve, 0400, &dir);
		break;
	case Qlisten:
	case Qlocalserver:
		devdir(c, c->qid, "listen", 0, eve, 0600, &dir);
		break;
	case Qconnect:
	case Qlocalclient:
		devdir(c, c->qid, "connect", 0, eve, 0600, &dir);
		break;
	default:
		panic("ringstat");
	}
	n = convD2M(&dir, db, n);
	if(n < BIT16SZ)
		error(Eshortstat);
	return n;
}

static RingDev *
ringstreamdev(RingConn *conn)
{
	RingDev *r;

	r = mallocz(sizeof(*r), 1);
	if(r == nil){
		ringconnput(conn);
		ringconnput(conn);
		exhausted("9ring endpoint");
	}
	r->ref = 1;
	r->path = conn->id;
	r->conn = conn;
	return r;
}

static Chan *
ringaccept(Chan *c, int omode)
{
	Proc *up = externup();
	RingDev *old, *stream;
	RingService *s;
	RingConn *conn;

	old = c->aux;
	s = old->service;
	if(s == nil || !old->server_role || openmode(omode) != ORDWR)
		error(Eperm);
	stream = mallocz(sizeof(*stream), 1);
	if(stream == nil)
		exhausted("9ring endpoint");
	stream->ref = 1;
	for(;;){
		qlock(&s->ql);
		if(s->pending != nil){
			conn = s->pending;
			s->pending = conn->next;
			if(s->pending == nil)
				s->last = nil;
			conn->next = nil;
			qunlock(&s->ql);
			break;
		}
		if(!s->announced){
			qunlock(&s->ql);
			free(stream);
			error(Eshutdown);
		}
		qunlock(&s->ql);
		if(waserror()){
			free(stream);
			nexterror();
		}
		sleep(&s->rlisten, ringpending, s);
		poperror();
	}
	stream->path = conn->id;
	stream->conn = conn;
	c->aux = stream;
	mkqid(&c->qid, RINGQID(conn->id, Qlocalserver), 0, QTFILE);
	c->mode = ORDWR;
	c->flag |= COPEN;
	c->offset = 0;
	c->iounit = P9_HOST_MSIZE;
	ringdevput(old);
	return c;
}

static Chan *
ringconnect(Chan *c, int omode)
{
	RingDev *old, *stream;
	RingService *s;
	RingConn *conn;

	old = c->aux;
	s = old->service;
	if(s == nil || old->server_role || openmode(omode) != ORDWR)
		error(Eperm);
	conn = ringnewconn();
	stream = ringstreamdev(conn);
	qlock(&s->ql);
	if(!s->announced){
		qunlock(&s->ql);
		free(stream);
		ringconnput(conn);
		ringconnput(conn);
		error(Eshutdown);
	}
	if(s->last == nil)
		s->pending = conn;
	else
		s->last->next = conn;
	s->last = conn;
	qunlock(&s->ql);
	wakeup(&s->rlisten);
	c->aux = stream;
	mkqid(&c->qid, RINGQID(conn->id, Qlocalclient), 0, QTFILE);
	c->mode = ORDWR;
	c->flag |= COPEN;
	c->offset = 0;
	c->iounit = P9_HOST_MSIZE;
	ringdevput(old);
	return c;
}

static Chan *
ringopen(Chan *c, int omode)
{
	RingDev *r;

	if(c->qid.type & QTDIR){
		if(omode != OREAD)
			error(Ebadarg);
		c->mode = omode;
		c->flag |= COPEN;
		c->offset = 0;
		return c;
	}

	r = c->aux;
	if(RINGTYPE(c->qid.path) == Qannounce){
		if(r->service == nil || !r->server_role ||
		   openmode(omode) != OREAD)
			error(Eperm);
		qlock(&r->service->ql);
		if(r->service->announced){
			qunlock(&r->service->ql);
			error(Einuse);
		}
		r->service->announced = 1;
		qunlock(&r->service->ql);
		c->mode = OREAD;
		c->flag |= COPEN;
		return c;
	}
	if(RINGTYPE(c->qid.path) == Qlisten)
		return ringaccept(c, omode);
	if(RINGTYPE(c->qid.path) == Qconnect)
		return ringconnect(c, omode);
	if(RINGTYPE(c->qid.path) == Qhost){
		if(hostshared == nil)
			error(Enonexist);
		lock(&hostslotlock);
		if(hostopen){
			unlock(&hostslotlock);
			error(Einuse);
		}
		hostopen = 1;
		unlock(&hostslotlock);
		c->mode = openmode(omode);
		c->flag |= COPEN;
		c->offset = 0;
		/* mntversion uses this as the maximum 9P message size. */
		c->iounit = P9_HOST_MSIZE;
		return c;
	}
	if(r->service != nil)
		error(Eperm);
	qlock(&r->ql);
	switch(RINGTYPE(c->qid.path)){
	case Qclient:
		r->client_open++;
		break;
	case Qserver:
		r->server_open++;
		break;
	}
	qunlock(&r->ql);

	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	c->iounit = sizeof(P9Slot);
	return c;
}

static void
ringretire(RingService *s)
{
	RingConn *r, *next;

	qlock(&s->ql);
	s->announced = 0;
	r = s->pending;
	s->pending = nil;
	s->last = nil;
	qunlock(&s->ql);
	wakeup(&s->rlisten);
	for(; r != nil; r = next){
		next = r->next;
		lock(&r->l);
		r->serveropen = 0;
		unlock(&r->l);
		wakeup(&r->rclient);
		wakeup(&r->rfree);
		ringconnput(r);
	}
}

static void
ringclose(Chan *c)
{
	RingDev *r;
	RingConn *conn;
	int type;

	r = c->aux;
	type = RINGTYPE(c->qid.path);
	if(type == Qannounce && c->flag & COPEN)
		ringretire(r->service);
	if((type == Qlocalclient || type == Qlocalserver) && c->flag & COPEN){
		conn = r->conn;
		lock(&conn->l);
		if(type == Qlocalclient)
			conn->clientopen = 0;
		else
			conn->serveropen = 0;
		unlock(&conn->l);
		wakeup(&conn->rclient);
		wakeup(&conn->rserver);
		wakeup(&conn->rfree);
		ringconnput(conn);
	}
	if(type == Qhost){
		if(c->flag & COPEN){
			lock(&hostslotlock);
			hostopen = 0;
			unlock(&hostslotlock);
		}
	}
	if(r->service == nil && r->conn == nil){
		qlock(&r->ql);
		if(c->flag & COPEN){
			switch(type){
		case Qclient:
			r->client_open--;
			wakeup(&r->r_server);
			break;
		case Qserver:
			r->server_open--;
			wakeup(&r->r_client);
			break;
		}
		}
		qunlock(&r->ql);
	}
	ringdevput(r);
}

static int
tring_has_data(void *v)
{
	RingDev *r = v;
	u32 head = __atomic_load_n(&r->chan->t_ring.head, __ATOMIC_ACQUIRE);
	u32 tail = __atomic_load_n(&r->chan->t_ring.tail, __ATOMIC_RELAXED);
	return (head != tail) || (r->client_open == 0);
}

static int
rring_has_data(void *v)
{
	RingDev *r = v;
	u32 head = __atomic_load_n(&r->chan->r_ring.head, __ATOMIC_ACQUIRE);
	u32 tail = __atomic_load_n(&r->chan->r_ring.tail, __ATOMIC_RELAXED);
	return (head != tail) || (r->server_open == 0);
}

static i32
hostread(void *va, i32 n)
{
	Proc *up = externup();
	P9Comp comp;
	P9HostBuffer *b;
	u32 wirelen;
	i32 count;
	int spins;

	if(n <= 0)
		return 0;
	qlock(&hostrxql);
	if(waserror()){
		qunlock(&hostrxql);
		nexterror();
	}
	spins = 0;
	while(hostcur < 0){
		if(p9_rring_poll(&hostshared->channel.r_ring, &comp)){
			if(comp.result >= P9_HOST_SLOTS)
				error("9ring: invalid host completion slot");
			if(comp.status != 0 || comp.count < 7 ||
			   comp.count > P9_HOST_MSIZE){
				lock(&hostslotlock);
				hostfree |= 1U << comp.result;
				unlock(&hostslotlock);
				error("9ring: host request failed");
			}
			b = &hostshared->buffer[comp.result];
			memmove(&wirelen, b->response, sizeof(wirelen));
			if(wirelen != comp.count || b->response[5] != (u8)comp.tag ||
			   b->response[6] != (u8)(comp.tag >> 8)){
				lock(&hostslotlock);
				hostfree |= 1U << comp.result;
				unlock(&hostslotlock);
				error("9ring: invalid host response");
			}
			hostcur = comp.result;
			hostlen = comp.count;
			hostrp = 0;
			break;
		}
		if(!hosthasrequest(nil)){
			sleep(&hostdev.r_client, hosthasrequest, nil);
			spins = 0;
			continue;
		}
		/* The host cannot wake a guest Rendez without an interrupt. */
		hostpause(&hostdev.r_client, spins++);
	}
	count = hostlen - hostrp;
	if(count > n)
		count = n;
	memmove(va, hostshared->buffer[hostcur].response + hostrp, count);
	hostrp += count;
	if(hostrp == hostlen){
		lock(&hostslotlock);
		hostfree |= 1U << hostcur;
		unlock(&hostslotlock);
		hostcur = -1;
	}
	poperror();
	qunlock(&hostrxql);
	return count;
}

static i32
hostwrite(void *va, i32 n)
{
	Proc *up = externup();
	P9Slot slot;
	u8 *m;
	int i;
	u32 wirelen;
	int spins;

	if(n < 7 || n > P9_HOST_MSIZE)
		error("9ring: invalid 9P message length");
	m = va;
	memmove(&wirelen, m, sizeof(wirelen));
	if(wirelen != n)
		error("9ring: 9P length does not match write");
	qlock(&hosttxql);
	if(waserror()){
		qunlock(&hosttxql);
		nexterror();
	}
	spins = 0;
	for(;;){
		lock(&hostslotlock);
		for(i = 0; i < P9_HOST_SLOTS; i++)
			if(hostfree & (1U << i))
				break;
		if(i < P9_HOST_SLOTS)
			hostfree &= ~(1U << i);
		unlock(&hostslotlock);
		if(i < P9_HOST_SLOTS)
			break;
		hostpause(&hostdev.r_server, spins++);
	}
	memmove(hostshared->buffer[i].request, m, n);
	memset(&slot, 0, sizeof(slot));
	slot.tag = m[5] | (m[6] << 8);
	slot.type = m[4];
	slot.count = n;
	slot.buf_addr = i;
	spins = 0;
	while(p9_tring_submit(&hostshared->channel.t_ring, &slot) < 0)
		hostpause(&hostdev.r_server, spins++);
	wakeup(&hostdev.r_client);
	poperror();
	qunlock(&hosttxql);
	return n;
}

static i32
ringread(Chan *c, void *va, i32 n, i64 offset)
{
	RingDev *r;
	char buf[256];
	P9Slot slot;
	P9Comp comp;

	r = c->aux;
	switch(RINGTYPE(c->qid.path)){
	case Qhost:
		return hostread(va, n);
	case Qlocalclient:
		return ringconnread(r->conn, 1, va, n);
	case Qlocalserver:
		return ringconnread(r->conn, 0, va, n);
	case Qdir:
		if(r->service != nil)
			return devdirread(c, va, n, ringnamedir, NNAMEDIR, ringgen);
		return devdirread(c, va, n, ringdir, NRINGDIR, ringgen);

	case Qctl:
		snprint(buf, sizeof(buf),
			"path %llud t_head %ud t_tail %ud r_head %ud r_tail %ud "
			"t_sub %llud t_con %llud r_comp %llud r_poll %llud\n",
			r->path,
			r->chan->t_ring.head, r->chan->t_ring.tail,
			r->chan->r_ring.head, r->chan->r_ring.tail,
			r->t_submitted, r->t_consumed,
			r->r_completed, r->r_polled);
		return readstr(offset, va, n, buf);

	case Qclient:
		/* Client reads completions from R-Ring */
		if(n < (i32)sizeof(P9Comp))
			error(Etoosmall);
		while(!p9_rring_poll(&r->chan->r_ring, &comp)){
			if(r->server_open == 0)
				return 0; /* EOF: server closed */
			sleep(&r->r_client, rring_has_data, r);
		}
		memmove(va, &comp, sizeof(P9Comp));
		r->r_polled++;
		return sizeof(P9Comp);

	case Qserver:
		/* Server reads submissions from T-Ring */
		if(n < (i32)sizeof(P9Slot))
			error(Etoosmall);
		while(!p9_tring_consume(&r->chan->t_ring, &slot)){
			if(r->client_open == 0)
				return 0; /* EOF: client closed */
			sleep(&r->r_server, tring_has_data, r);
		}
		memmove(va, &slot, sizeof(P9Slot));
		r->t_consumed++;
		return sizeof(P9Slot);

	case Qring:
		/* Direct read of shared ring structure */
		if(offset >= (i64)sizeof(P9Channel))
			return 0;
		if(offset + n > (i64)sizeof(P9Channel))
			n = (i32)(sizeof(P9Channel) - offset);
		memmove(va, (u8*)r->chan + offset, n);
		return n;

	default:
		panic("ringread");
	}
	return -1;
}

static i32
ringwrite(Chan *c, void *va, i32 n, i64 offset)
{
	RingDev *r;
	P9Slot slot;
	P9Comp comp;

	r = c->aux;
	switch(RINGTYPE(c->qid.path)){
	case Qhost:
		return hostwrite(va, n);
	case Qlocalclient:
		return ringconnwrite(r->conn, 1, va, n);
	case Qlocalserver:
		return ringconnwrite(r->conn, 0, va, n);
	case Qctl:
		/* Control messages can reset or configure ring */
		return n;

	case Qclient:
		/* Client writes submission into T-Ring */
		if(n < (i32)sizeof(P9Slot))
			error(Etoosmall);
		memmove(&slot, va, sizeof(P9Slot));
		if(p9_tring_submit(&r->chan->t_ring, &slot) < 0)
			error("ring full");
		r->t_submitted++;
		wakeup(&r->r_server);
		return sizeof(P9Slot);

	case Qserver:
		/* Server writes completion into R-Ring */
		if(n < (i32)sizeof(P9Comp))
			error(Etoosmall);
		memmove(&comp, va, sizeof(P9Comp));
		if(p9_rring_complete(&r->chan->r_ring, &comp) < 0)
			error("ring full");
		r->r_completed++;
		wakeup(&r->r_client);
		return sizeof(P9Comp);

	case Qring:
		/* Direct write into shared ring buffer */
		if(offset >= (i64)sizeof(P9Channel))
			return 0;
		if(offset + n > (i64)sizeof(P9Channel))
			n = (i32)(sizeof(P9Channel) - offset);
		memmove((u8*)r->chan + offset, va, n);
		return n;

	default:
		panic("ringwrite");
	}
	return -1;
}

Dev ringdevtab = {
	.dc = '~',
	.name = "9ring",

	.reset = devreset,
	.init = ringinit,
	.shutdown = devshutdown,
	.attach = ringattach,
	.walk = ringwalk,
	.stat = ringstat,
	.open = ringopen,
	.create = devcreate,
	.close = ringclose,
	.read = ringread,
	.bread = devbread,
	.write = ringwrite,
	.bwrite = devbwrite,
	.remove = devremove,
	.wstat = devwstat,
};
