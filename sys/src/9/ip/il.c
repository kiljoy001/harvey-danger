#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#include	"ip.h"
#include	"ipv6.h"

enum				/* Connection state */
{
	Ilclosed,
	Ilsyncer,
	Ilsyncee,
	Ilestablished,
	Ildraining,
	Illistening,
	Ilclosing,
	Ilopening,		/* only for file server */
};

char	*ilstates[] =
{
	"Closed",
	"Syncer",
	"Syncee",
	"Established",
	"Draining",
	"Listen",
	"Closing",
	"Opening",		/* only for file server */
};

enum				/* Packet types */
{
	Ilsync,
	Ildata,
	Ildataquery,
	Ilack,
	Ilquery,
	Ilstate,
	Ilclose,
};

char	*iltype[] =
{
	"sync",
	"data",
	"dataquery",
	"ack",
	"query",
	"state",
	"close"
};

enum
{
	Seconds		= 1000,
	Iltickms 	= 50,		/* time base */
	AckDelay	= 2*Iltickms,	/* max time twixt message rcvd & ack sent */
	MaxTimeout 	= 30*Seconds,	/* max time between rexmit */
	QueryTime	= 10*Seconds,	/* time between subsequent queries */
	DeathTime	= 30*QueryTime,

	MaxRexmit 	= 16,		/* max retransmissions before hangup */
	Defaultwin	= 20,
	Initialwin	= 2,
	Maxflight	= 32*1024,	/* leave room in the receiver's 64K queue */

	LogAGain	= 3,
	AGain		= 1<<LogAGain,
	LogDGain	= 2,
	DGain		= 1<<LogDGain,

	DefByteRate	= 100,		/* assume a megabit link */
	DefRtt		= 50,		/* cross country on a great day */

	Maxrq		= 64*1024,
	Ilflow		= 0x80,		/* SYNC capability; other packet types retain ilspec */
	Ilcreditlen	= 8,		/* cumulative bytes removed from receive queue */
};

enum
{
	Nqt=	8,
};

typedef struct Ilcb Ilcb;
struct Ilcb			/* Control block */
{
	int	state;		/* Connection state */
	Conv	*conv;
	QLock	ackq;		/* Unacknowledged queue */
	Block	*unacked;
	Block	*unackedtail;
	ulong	unackedbytes;
	uvlong	txbytes;	/* unique payload bytes sent */
	uvlong	rxqueued;	/* payload bytes delivered to receive queue */
	uvlong	peerread;	/* peer's cumulative bytes read */
	uvlong	creditread;	/* last read count advertised */
	int	flowpeer;	/* peer advertised Ilflow on SYNC */
	Block	*pending;	/* next message held while the send window is full */
	int	flight;		/* unacknowledged messages */
	int	cwind;		/* congestion window, in messages */
	int	ssthresh;
	int	acks;		/* acknowledgements toward the next window increase */
	int	inrecovery;
	ulong	recovery;	/* highest message sent when loss was confirmed */
	QLock	outo;		/* Out of order packet queue */
	Block	*outoforder;
	ulong	next;		/* Id of next to send */
	ulong	recvd;		/* Last packet received */
	ulong	acksent;	/* Last packet acked */
	ulong	start;		/* Local start id */
	ulong	rstart;		/* Remote start id */
	int	window;		/* Maximum receive window */
	int	rxquery;	/* number of queries on this connection */
	int	rxtot;		/* number of retransmits on this connection */
	int	rexmit;		/* number of retransmits of *unacked */
	ulong	qt[Nqt+1];	/* state table for query messages */
	int	qtx;		/* ... index into qt */

	/* if set, fasttimeout causes a connection request to terminate after 4*Iltickms */
	int	fasttimeout;

	/* timers */
	ulong	lastxmit;	/* time of last xmit */
	ulong	lastrecv;	/* time of last recv */
	ulong	timeout;	/* retransmission time for *unacked */
	ulong	acktime;	/* time to send next ack */
	ulong	querytime;	/* time to send next query */

	/* adaptive measurements */
	int	delay;		/* Average of the fixed rtt delay */
	int	rate;		/* Average uchar rate */
	int	mdev;		/* Mean deviation of rtt */
	int	maxrtt;		/* largest rtt seen */
	ulong	rttack;		/* The ack we are waiting for */
	int	rttlen;		/* Length of rttack packet */
	uvlong	rttstart;	/* Time we issued rttack packet */
};

enum
{
	IL_HDRSIZE	= 18,
	IL_LISTEN	= 0,
	IL_CONNECT	= 1,
	IP_ILPROTO	= 40,
};

typedef struct Ilhdr Ilhdr;
struct Ilhdr
{
	uchar	ilsum[2];	/* Checksum including header */
	uchar	illen[2];	/* Packet length */
	uchar	iltype;		/* Packet type */
	uchar	ilspec;		/* Special */
	uchar	ilsrc[2];	/* Src port */
	uchar	ildst[2];	/* Dst port */
	uchar	ilid[4];	/* Sequence id */
	uchar	ilack[4];	/* Acked sequence */
};

static int
iliphdrlen(int version)
{
	if(version == V4)
		return IP4HDR;
	return IP6HDR;
}

static Ilhdr*
ilhdr(Block *bp, int version)
{
	return (Ilhdr*)(bp->rp + iliphdrlen(version));
}

static void
ilsetip(Block *bp, int version, uchar *src, uchar *dst)
{
	Ip4hdr *h4;
	Ip6hdr *h6;

	if(version == V4){
		h4 = (Ip4hdr*)bp->rp;
		memset(h4, 0, IP4HDR);
		h4->vihl = IP_VER4;
		h4->proto = IP_ILPROTO;
		v6tov4(h4->src, src);
		v6tov4(h4->dst, dst);
	} else {
		h6 = (Ip6hdr*)bp->rp;
		memset(h6, 0, IP6HDR);
		h6->vcf[0] = IP_VER6;
		h6->proto = IP_ILPROTO;
		ipmove(h6->src, src);
		ipmove(h6->dst, dst);
	}
}

static int
iloutput(Fs *f, Block *bp, int version, int ttl, int tos, Conv *c)
{
	if(version == V4)
		return ipoput4(f, bp, 0, ttl, tos, c);
	return ipoput6(f, bp, 0, ttl, tos, c);
}

enum
{
	InMsgs,
	OutMsgs,
	CsumErrs,		/* checksum errors */
	HlenErrs,		/* header length error */
	LenErrs,		/* short packet */
	OutOfOrder,		/* out of order */
	Retrans,		/* retransmissions */
	DupMsg,
	DupBytes,
	DroppedMsgs,

	Nstats,
};

static char *statnames[] =
{
	[InMsgs] = "InMsgs",
	[OutMsgs] = "OutMsgs",
	[CsumErrs] = "CsumErrs",
	[HlenErrs] = "HlenErr",
	[LenErrs] = "LenErrs",
	[OutOfOrder] = "OutOfOrder",
	[Retrans] = "Retrans",
	[DupMsg] = "DupMsg",
	[DupBytes] = "DupBytes",
	[DroppedMsgs] = "DroppedMsgs",
};

typedef struct Ilpriv Ilpriv;
struct Ilpriv
{
	Ipht	ht;

	uvlong	stats[Nstats];

	ulong	csumerr;		/* checksum errors */
	ulong	hlenerr;		/* header length error */
	ulong	lenerr;			/* short packet */
	ulong	order;			/* out of order */
	ulong	rexmit;			/* retransmissions */
	ulong	dup;
	ulong	dupb;

	/* keeping track of the ack kproc */
	int	ackprocstarted;
	QLock	apl;
};

/* state for query/dataquery messages */


void	ilrcvmsg(Conv*, Block*);
void	ilpump(Conv*);
int	ilsendctl(Conv*, int, ulong, ulong, int);
void	ilackq(Ilcb*, Block*);
void	ilprocess(Conv*, Ilhdr*, Block*);
void	ilpullup(Conv*);
void	ilhangup(Conv*, char*);
void	ilfreeq(Ilcb*);
void	ilrexmit(Ilcb*);
void	ilbackoff(Ilcb*);
void	ilsettimeout(Ilcb*);
char*	ilstart(Conv*, int, int);
void	ilackproc(void*);
void	iloutoforder(Conv*, Ilhdr*, Block*);
void	iliput(Proto*, Ipifc*, Block*);
void	iladvise(Proto*, Block*, char*);
int	ilnextqt(Ilcb*);
void	ilcbinit(Ilcb*);
int	later(ulong, ulong, char*);
void	ilreject(Fs*, Block*);
void	illocalclose(Conv *c);
	int 	ilcksum = 1;
static 	int 	initseq = 25001;
static	ulong	scalediv, scalemul;
static	char	*etime = "connection timed out";

static char*
ilconnect(Conv *c, char **argv, int argc)
{
	char *e, *p;
	int fast;

	/* huge hack to quickly try an il connection */
	fast = 0;
	if(argc > 1){
		p = strstr(argv[1], "!fasttimeout");
		if(p != nil){
			*p = 0;
			fast = 1;
		}
	}

	e = Fsstdconnect(c, argv, argc);
	if(e != nil)
		return e;

	return ilstart(c, IL_CONNECT, fast);
}

static int
ilstate(Conv *c, char *state, int n)
{
	Ilcb *ic;

	ic = (Ilcb*)(c->ptcl);
	return snprint(state, n, "%s qin %d qout %d del %5.5d Br %5.5d md %5.5d una %5.5lud rex %5.5d rxq %5.5d max %5.5d cwnd %d flight %d ssthresh %d flow %d sent %llud read %llud peerread %llud\n",
		ilstates[ic->state],
		c->rq ? qlen(c->rq) : 0,
		(c->wq ? qlen(c->wq) : 0) + (ic->pending ? blocklen(ic->pending) : 0),
		ic->delay>>LogAGain, ic->rate>>LogAGain, ic->mdev>>LogDGain,
		ic->unackedbytes, ic->rxtot, ic->rxquery, ic->maxrtt,
		ic->cwind, ic->flight, ic->ssthresh, ic->flowpeer,
		ic->txbytes, ic->rxqueued - (c->rq ? qlen(c->rq) : 0), ic->peerread);
}

static int
ilinuse(Conv *c)
{
	Ilcb *ic;

	ic = (Ilcb*)(c->ptcl);
	return ic->state != Ilclosed;

}

/* called with c locked */
static char*
ilannounce(Conv *c, char **argv, int argc)
{
	char *e;

	e = Fsstdannounce(c, argv, argc);
	if(e != nil)
		return e;
	e = ilstart(c, IL_LISTEN, 0);
	if(e != nil)
		return e;
	Fsconnected(c, nil);

	return nil;
}

void
illocalclose(Conv *c)
{
	Ilcb *ic;
	Ilpriv *ipriv;

	ipriv = c->p->priv;
	ic = (Ilcb*)c->ptcl;
	ic->state = Ilclosed;
	iphtrem(&ipriv->ht, c);
	c->lport = 0;
	ipmove(c->laddr, IPnoaddr);
}

/* Finish a local close after every queued datagram has been acknowledged. */
static void
iltryclose(Conv *c)
{
	Ilcb *ic;

	ic = (Ilcb*)c->ptcl;
	if(ic->state != Ildraining || ic->flight != 0 || ic->pending != nil || qlen(c->wq) != 0)
		return;
	ic->state = Ilclosing;
	qclose(c->wq);
	ilsettimeout(ic);
	ilsendctl(c, Ilclose, ic->next, ic->recvd, 0);
	ilfreeq(ic);
}

static void
ilclose(Conv *c)
{
	Ilcb *ic;

	ic = (Ilcb*)c->ptcl;

	qclose(c->rq);
	qclose(c->eq);
	if(ic->state == Ilestablished){
		qhangup(c->wq, nil);
		ic->state = Ildraining;
		ilpump(c);
		iltryclose(c);
		return;
	}
	qclose(c->wq);

	switch(ic->state) {
	case Ilclosing:
	case Ilclosed:
		break;
	case Ilsyncer:
	case Ilsyncee:
	case Ilestablished:
		ic->state = Ilclosing;
		ilsettimeout(ic);
		ilsendctl(c, Ilclose, ic->next, ic->recvd, 0);
		break;
	case Illistening:
		illocalclose(c);
		break;
	}
	ilfreeq(ic);
}

static void
ilxmit(Conv *c, Block *bp)
{
	Ilhdr *ih;
	Ilcb *ic;
	int dlen;
	ulong id, ack;
	Fs *f;
	Ilpriv *priv;

	f = c->p->f;
	priv = c->p->priv;
	ic = (Ilcb*)c->ptcl;

	dlen = blocklen(bp);

	/* Make space to fit il & ip */
	bp = padblock(bp, iliphdrlen(c->ipversion)+IL_HDRSIZE);
	ilsetip(bp, c->ipversion, c->laddr, c->raddr);
	ih = ilhdr(bp, c->ipversion);

	/* Il fields */
	hnputs(ih->illen, dlen+IL_HDRSIZE);
	hnputs(ih->ilsrc, c->lport);
	hnputs(ih->ildst, c->rport);

	qlock(&ic->ackq);
	id = ic->next++;
	hnputl(ih->ilid, id);
	ack = ic->recvd;
	hnputl(ih->ilack, ack);
	ic->acksent = ack;
	ic->acktime = NOW + AckDelay;
	ih->iltype = Ildata;
	ih->ilspec = 0;
	ih->ilsum[0] = 0;
	ih->ilsum[1] = 0;

	/* Checksum of ilheader plus data (not ip & no pseudo header) */
	if(ilcksum)
		hnputs(ih->ilsum, ptclcsum(bp, iliphdrlen(c->ipversion), dlen+IL_HDRSIZE));

	ilackq(ic, bp);
	qunlock(&ic->ackq);

	/* Start the round trip timer for this packet if the timer is free */
	if(ic->rttack == 0) {
		ic->rttack = id;
		ic->rttstart = fastticks(nil);
		ic->rttlen = dlen + iliphdrlen(c->ipversion) + IL_HDRSIZE;
	}

	if(later(NOW, ic->timeout, nil))
		ilsettimeout(ic);
	ic->txbytes += dlen;
	iloutput(f, bp, c->ipversion, c->ttl, c->tos, c);
	priv->stats[OutMsgs]++;
}

/* Called with the conversation locked, from a writer or an incoming ack. */
void
ilpump(Conv *c)
{
	Ilcb *ic;
	Block *bp;
	int n;

	ic = (Ilcb*)c->ptcl;
	if(ic->state == Ilclosed || ic->state == Illistening || ic->state == Ilclosing)
		return;

	while(ic->flight < ic->cwind){
		bp = ic->pending;
		if(bp == nil){
			bp = qget(c->wq);
			if(bp == nil)
				break;
		}
		n = blocklen(bp) + iliphdrlen(c->ipversion) + IL_HDRSIZE;
		if(ic->flight != 0 && ic->unackedbytes + n > Maxflight){
			ic->pending = bp;
			break;
		}
		if(ic->flowpeer && ic->txbytes + blocklen(bp) > (uvlong)Maxrq + ic->peerread){
			ic->pending = bp;
			break;
		}
		ic->pending = nil;
		ilxmit(c, bp);
	}
}

static void
ilkick(void *x)
{
	Proc *up = externup();
	Conv *c;
	Ilcb *ic;

	c = x;
	ic = (Ilcb*)c->ptcl;
	if(waserror()){
		qunlock(&c->ql);
		nexterror();
	}
	qlock(&c->ql);
	if(ic->state == Ilclosed || ic->state == Illistening || ic->state == Ilclosing){
		qhangup(c->rq, nil);
	}else
		ilpump(c);
	qunlock(&c->ql);
	poperror();
}

static void
ilcreate(Conv *c)
{
	c->rq = qopen(Maxrq, 0, 0, c);
	c->wq = qopen(Maxrq, Qkick, ilkick, c);
}

int
ilxstats(Proto *il, char *buf, int len)
{
	Ilpriv *priv;
	char *p, *e;
	int i;

	priv = il->priv;
	p = buf;
	e = p+len;
	for(i = 0; i < Nstats; i++)
		p = seprint(p, e, "%s: %llud\n", statnames[i], priv->stats[i]);
	return p - buf;
}

void
ilackq(Ilcb *ic, Block *bp)
{
	Block *np;
	int n;

	n = blocklen(bp);

	/* Enqueue a copy on the unacked queue in case this one gets lost */
	np = copyblock(bp, n);
	if(ic->unacked)
		ic->unackedtail->list = np;
	else
		ic->unacked = np;
	ic->unackedtail = np;
	np->list = nil;
	ic->unackedbytes += n;
	ic->flight++;
}

static
void
ilrttcalc(Ilcb *ic, Block *bp)
{
	int rtt, tt, pt, delay, rate;

	rtt = fastticks(nil) - ic->rttstart;
	rtt = (rtt*scalemul)/scalediv;
	delay = ic->delay;
	rate = ic->rate;

	/* Guard against zero wrap */
	if(rtt > 120000 || rtt < 0)
		return;

	/* this block had to be transmitted after the one acked so count its size */
	ic->rttlen += blocklen(bp) + iliphdrlen(ic->conv->ipversion) + IL_HDRSIZE;

	if(ic->rttlen < 256){
		/* guess fixed delay as rtt of small packets */
		delay += rtt - (delay>>LogAGain);
		if(delay < AGain)
			delay = AGain;
		ic->delay = delay;
	} else {
		/* if packet took longer than avg rtt delay, recalc rate */
		tt = rtt - (delay>>LogAGain);
		if(tt > 0){
			rate += ic->rttlen/tt - (rate>>LogAGain);
			if(rate < AGain)
				rate = AGain;
			ic->rate = rate;
		}
	}

	/* mdev */
	pt = ic->rttlen/(rate>>LogAGain) + (delay>>LogAGain);
	ic->mdev += abs(rtt-pt) - (ic->mdev>>LogDGain);

	if(rtt > ic->maxrtt)
		ic->maxrtt = rtt;
}

void
ilackto(Ilcb *ic, ulong ackto, Block *bp)
{
	Ilhdr *h;
	ulong id;
	int acked;

	if(ackto >= ic->next)
		return;

	if(ic->rttack == ackto)
		ilrttcalc(ic, bp);

	/* Cancel if we've passed the packet we were interested in */
	if(ic->rttack <= ackto)
		ic->rttack = 0;

	acked = 0;
	qlock(&ic->ackq);
	while(ic->unacked) {
		h = ilhdr(ic->unacked, ic->conv->ipversion);
		id = nhgetl(h->ilid);
		if(ackto < id)
			break;

		bp = ic->unacked;
		ic->unacked = bp->list;
		bp->list = nil;
		ic->unackedbytes -= blocklen(bp);
		ic->flight--;
		acked++;
		freeblist(bp);
		ic->rexmit = 0;
		ilsettimeout(ic);
	}
	if(ic->unacked == nil)
		ic->unackedtail = nil;
	qunlock(&ic->ackq);
	if(acked != 0){
		if(ic->inrecovery && ackto >= ic->recovery)
			ic->inrecovery = 0;
		if(ic->cwind < ic->ssthresh)
			ic->cwind += acked;
		else {
			ic->acks += acked;
			while(ic->acks >= ic->cwind){
				ic->acks -= ic->cwind;
				ic->cwind++;
			}
		}
		if(ic->cwind > ic->window)
			ic->cwind = ic->window;
		ilpump(ic->conv);
		iltryclose(ic->conv);
	}
}

void
iliput(Proto *il, Ipifc *ifc, Block *bp)
{
	Proc *up = externup();
	char *st;
	Ilcb *ic;
	Ilhdr *ih;
	Ip4hdr *h4;
	Ip6hdr *h6;
	uchar raddr[IPaddrlen];
	uchar laddr[IPaddrlen];
	ushort sp, dp, csum;
	int plen, illen, version, iplen;
	Conv *new, *s;
	Ilpriv *ipriv;

	ipriv = il->priv;

	plen = blocklen(bp);
	if(plen == 0)
		goto raise;
	switch(bp->rp[0] & 0xF0){
	case IP_VER4:
		version = V4;
		break;
	case IP_VER6:
		version = V6;
		break;
	default:
		goto raise;
	}
	iplen = iliphdrlen(version);

	if(plen < iplen+IL_HDRSIZE){
		netlog(il->f, Logil, "il: hlenerr\n");
		ipriv->stats[HlenErrs]++;
		goto raise;
	}

	ih = ilhdr(bp, version);
	illen = nhgets(ih->illen);
	if(illen < IL_HDRSIZE || illen+iplen > plen){
		netlog(il->f, Logil, "il: lenerr\n");
		ipriv->stats[LenErrs]++;
		goto raise;
	}

	sp = nhgets(ih->ildst);
	dp = nhgets(ih->ilsrc);
	if(version == V4){
		h4 = (Ip4hdr*)bp->rp;
		v4tov6(raddr, h4->src);
		v4tov6(laddr, h4->dst);
	} else {
		h6 = (Ip6hdr*)bp->rp;
		ipmove(raddr, h6->src);
		ipmove(laddr, h6->dst);
	}

	if((csum = ptclcsum(bp, iplen, illen)) != 0) {
		if(ih->iltype > Ilclose)
			st = "?";
		else
			st = iltype[ih->iltype];
		ipriv->stats[CsumErrs]++;
		netlog(il->f, Logil, "il: cksum %ux %s, pkt(%ux id %ud ack %I/%d->%d)\n",
			csum, st, nhgetl(ih->ilid), nhgetl(ih->ilack), raddr, sp, dp);
		goto raise;
	}
	if(ih->iltype > Ilclose)
		goto raise;

	qlock(&il->ql);
	s = iphtlook(&ipriv->ht, raddr, dp, laddr, sp);
	if(s == nil){
		if(ih->iltype == Ilsync)
			ilreject(il->f, bp);		/* no listener */
		qunlock(&il->ql);
		goto raise;
	}
	ic = (Ilcb*)s->ptcl;
	if(ic->state == Illistening){
		if(ih->iltype != Ilsync){
			qunlock(&il->ql);
			if(ih->iltype > Ilclose)
				st = "?";
			else
				st = iltype[ih->iltype];
			ilreject(il->f, bp);		/* no channel and not sync */
			netlog(il->f, Logil, "il: no channel, pkt(%s id %ud ack %ud %I/%ud->%ud)\n",
				st, nhgetl(ih->ilid), nhgetl(ih->ilack), raddr, sp, dp);
			goto raise;
		}

		new = Fsnewcall(s, raddr, dp, laddr, sp, version);
		if(new == nil){
			qunlock(&il->ql);
			netlog(il->f, Logil, "il: bad newcall %I/%ud->%ud\n", raddr, sp, dp);
			ilreject(il->f, bp);
			goto raise;
		}
		s = new;

		ic = (Ilcb*)s->ptcl;

		ic->conv = s;
		ic->state = Ilsyncee;
		ilcbinit(ic);
		ic->rstart = nhgetl(ih->ilid);
		iphtadd(&ipriv->ht, s);
	}

	qlock(&s->ql);
	qunlock(&il->ql);
	if(waserror()){
		qunlock(&s->ql);
		nexterror();
	}
	ilprocess(s, ih, bp);
	qunlock(&s->ql);
	poperror();
	return;
raise:
	freeblist(bp);
}

/* ACK and STATE carry read credit only after both SYNCs offered Ilflow. */
static int
ilcredit(Conv *s, Ilhdr *h)
{
	Ilcb *ic;
	uchar *p;
	uvlong read;

	ic = (Ilcb*)s->ptcl;
	if(!ic->flowpeer || nhgets(h->illen) < IL_HDRSIZE+Ilcreditlen)
		return 0;
	p = (uchar*)h + IL_HDRSIZE;
	read = ((uvlong)nhgetl(p) << 32) | nhgetl(p+4);
	if(read > ic->txbytes)
		read = ic->txbytes;
	if(read <= ic->peerread)
		return 0;
	ic->peerread = read;
	return 1;
}

void
_ilprocess(Conv *s, Ilhdr *h, Block *bp)
{
	Ilcb *ic;
	ulong id, ack;
	Ilpriv *priv;

	id = nhgetl(h->ilid);
	ack = nhgetl(h->ilack);

	ic = (Ilcb*)s->ptcl;

	ic->lastrecv = NOW;
	ic->querytime = NOW + QueryTime;
	priv = s->p->priv;
	priv->stats[InMsgs]++;

	switch(ic->state) {
	default:
		netlog(s->p->f, Logil, "il: unknown state %d\n", ic->state);
	case Ilclosed:
		freeblist(bp);
		break;
	case Ilsyncer:
		switch(h->iltype) {
		default:
			break;
		case Ilsync:
			if(ack != ic->start)
				ilhangup(s, "connection rejected");
			else {
				ic->flowpeer = (h->ilspec & Ilflow) != 0;
				ic->recvd = id;
				ic->rstart = id;
				ilsendctl(s, Ilack, ic->next, ic->recvd, 0);
				ic->state = Ilestablished;
				ic->fasttimeout = 0;
				ic->rexmit = 0;
				Fsconnected(s, nil);
				ilpullup(s);
			}
			break;
		case Ilclose:
			if(ack == ic->start)
				ilhangup(s, "connection rejected");
			break;
		}
		freeblist(bp);
		break;
	case Ilsyncee:
		switch(h->iltype) {
		default:
			break;
		case Ilsync:
			if(id != ic->rstart || ack != 0){
				illocalclose(s);
			} else {
				ic->flowpeer = (h->ilspec & Ilflow) != 0;
				ic->recvd = id;
				ilsendctl(s, Ilsync, ic->start, ic->recvd, 0);
			}
			break;
		case Ilack:
			if(ack == ic->start) {
				ic->state = Ilestablished;
				ic->fasttimeout = 0;
				ic->rexmit = 0;
				ilpullup(s);
			}
			break;
		case Ildata:
			if(ack == ic->start) {
				ic->state = Ilestablished;
				ic->fasttimeout = 0;
				ic->rexmit = 0;
				goto established;
			}
			break;
		case Ilclose:
			if(ack == ic->start)
				ilhangup(s, "remote close");
			break;
		}
		freeblist(bp);
		break;
	case Ildraining:
	case Ilestablished:
	established:
		switch(h->iltype) {
		case Ilsync:
			if(id != ic->rstart)
				ilhangup(s, "remote close");
			else
				ilsendctl(s, Ilack, ic->next, ic->rstart, 0);
			freeblist(bp);
			break;
		case Ildata:
			/*
			 * avoid consuming all the mount rpc buffers in the
			 * system.  if the input queue is too long, drop this
			 * packet.
			 */
			if (s->rq && qlen(s->rq) >= Maxrq) {
				priv->stats[DroppedMsgs]++;
				freeblist(bp);
				break;
			}

			ilackto(ic, ack, bp);
			iloutoforder(s, h, bp);
			ilpullup(s);
			break;
		case Ildataquery:
			ilackto(ic, ack, bp);
			iloutoforder(s, h, bp);
			ilpullup(s);
			ilsendctl(s, Ilstate, ic->next, ic->recvd, h->ilspec);
			break;
		case Ilack:
			id = ilcredit(s, h);
			ilackto(ic, ack, bp);
			if(id)
				ilpump(s);
			freeblist(bp);
			break;
		case Ilquery:
			ilackto(ic, ack, bp);
			ilsendctl(s, Ilstate, ic->next, ic->recvd, h->ilspec);
			freeblist(bp);
			break;
		case Ilstate:
			if(ack >= ic->rttack)
				ic->rttack = 0;
			id = ilcredit(s, h);
			ilackto(ic, ack, bp);
			if(id)
				ilpump(s);
			if(h->ilspec > Nqt)
				h->ilspec = 0;
			if(ic->qt[h->ilspec] > ack){
				ilrexmit(ic);
				ilsettimeout(ic);
			}
			freeblist(bp);
			break;
		case Ilclose:
			freeblist(bp);
			if(ack < ic->start || ack > ic->next)
				break;
			ic->recvd = id;
			ilsendctl(s, Ilclose, ic->next, ic->recvd, 0);
			ic->state = Ilclosing;
			ilsettimeout(ic);
			ilfreeq(ic);
			break;
		}
		break;
	case Illistening:
		freeblist(bp);
		break;
	case Ilclosing:
		switch(h->iltype) {
		case Ilclose:
			ic->recvd = id;
			ilsendctl(s, Ilclose, ic->next, ic->recvd, 0);
			if(ack == ic->next)
				ilhangup(s, nil);
			break;
		default:
			break;
		}
		freeblist(bp);
		break;
	}
}

void
ilrexmit(Ilcb *ic)
{
	Ilhdr *h;
	Block *nb;
	Conv *c;
	ulong id;
	Ilpriv *priv;

	nb = nil;
	qlock(&ic->ackq);
	if(ic->unacked)
		nb = copyblock(ic->unacked, blocklen(ic->unacked));
	qunlock(&ic->ackq);

	if(nb == nil)
		return;

	c = ic->conv;
	h = ilhdr(nb, c->ipversion);

	h->iltype = Ildataquery;
	hnputl(h->ilack, ic->recvd);
	h->ilspec = ilnextqt(ic);
	h->ilsum[0] = 0;
	h->ilsum[1] = 0;
	hnputs(h->ilsum, ptclcsum(nb, iliphdrlen(c->ipversion), nhgets(h->illen)));

	id = nhgetl(h->ilid);
	/* The state exchange confirmed loss; a timeout query alone does not. */
	if(!ic->inrecovery){
		ic->ssthresh = ic->cwind / 2;
		if(ic->ssthresh < 2)
			ic->ssthresh = 2;
		ic->cwind = ic->ssthresh;
		ic->acks = 0;
		ic->recovery = ic->next - 1;
		ic->inrecovery = 1;
	}
	/* Acknowledgements after a retransmission cannot give a valid RTT. */
	ic->rttack = 0;
	netlog(c->p->f, Logil, "il: rexmit %lud %lud: %d %lud: %I %d/%d\n", id, ic->recvd,
		ic->rexmit, ic->timeout,
		c->raddr, c->lport, c->rport);

	ilbackoff(ic);

	iloutput(c->p->f, nb, c->ipversion, c->ttl, c->tos, c);

	/* statistics */
	ic->rxtot++;
	priv = c->p->priv;
	priv->rexmit++;
}

/* DEBUG */
void
ilprocess(Conv *s, Ilhdr *h, Block *bp)
{
	Ilcb *ic;

	ic = (Ilcb*)s->ptcl;

	USED(ic);
	netlog(s->p->f, Logipmsg, "%11s rcv %lud/%lud snt %lud/%lud pkt(%s id %d ack %ud %ud->%ud) ",
		ilstates[ic->state],  ic->rstart, ic->recvd, ic->start,
		ic->next, iltype[h->iltype], nhgetl(h->ilid),
		nhgetl(h->ilack), nhgets(h->ilsrc), nhgets(h->ildst));

	_ilprocess(s, h, bp);

	netlog(s->p->f, Logipmsg, "%11s rcv %lud snt %lud\n", ilstates[ic->state], ic->recvd, ic->next);
}

void
ilhangup(Conv *s, char *msg)
{
	Ilcb *ic;
	int callout;

	netlog(s->p->f, Logil, "il: hangup! %I %d/%d: %s\n", s->raddr,
		s->lport, s->rport, msg?msg:"no reason");

	ic = (Ilcb*)s->ptcl;
	callout = ic->state == Ilsyncer;
	illocalclose(s);

	qhangup(s->rq, msg);
	qclose(s->wq);
	qhangup(s->wq, msg);
	ilfreeq(ic);

	if(callout)
		Fsconnected(s, msg);
}

void
ilpullup(Conv *s)
{
	Ilcb *ic;
	Ilhdr *oh;
	Block *bp;
	ulong oid, dlen;
	Ilpriv *ipriv;

	ic = (Ilcb*)s->ptcl;
	if(ic->state != Ilestablished)
		return;

	qlock(&ic->outo);
	while(ic->outoforder) {
		bp = ic->outoforder;
		oh = ilhdr(bp, s->ipversion);
		oid = nhgetl(oh->ilid);
		if(oid <= ic->recvd) {
			ic->outoforder = bp->list;
			freeblist(bp);
			continue;
		}
		if(oid != ic->recvd+1){
			ipriv = s->p->priv;
			ipriv->stats[OutOfOrder]++;
			break;
		}
		dlen = nhgets(oh->illen)-IL_HDRSIZE;
		if(qlen(s->rq) + dlen > Maxrq)
			break;

		ic->recvd = oid;
		ic->outoforder = bp->list;

		bp->list = nil;
		bp = trimblock(bp, iliphdrlen(s->ipversion)+IL_HDRSIZE, dlen);

		/*
		 * Upper levels don't know about multiple-block
		 * messages so copy all into one (yick).
		 */
		/* Capacity was checked above; Qflow also counts block allocation. */
		if(qpassnolim(s->rq, packblock(concatblock(bp))) == dlen)
			ic->rxqueued += dlen;
	}
	qunlock(&ic->outo);
}

void
iloutoforder(Conv *s, Ilhdr *h, Block *bp)
{
	Ilcb *ic;
	uchar *lid;
	Block *f, **l;
	ulong id, newid;
	Ilpriv *ipriv;

	ipriv = s->p->priv;
	ic = (Ilcb*)s->ptcl;
	bp->list = nil;

	id = nhgetl(h->ilid);
	/* Window checks */
	if(id <= ic->recvd || id > ic->recvd+ic->window) {
		netlog(s->p->f, Logil, "il: message outside window %lud <%lud-%lud>: %I %d/%d\n",
			id, ic->recvd, ic->recvd+ic->window, s->raddr, s->lport, s->rport);
		freeblist(bp);
		return;
	}

	/* Packet is acceptable so sort onto receive queue for pullup */
	qlock(&ic->outo);
	if(ic->outoforder == nil)
		ic->outoforder = bp;
	else {
		l = &ic->outoforder;
		for(f = *l; f; f = f->list) {
			lid = ilhdr(f, s->ipversion)->ilid;
			newid = nhgetl(lid);
			if(id <= newid) {
				if(id == newid) {
					ipriv->stats[DupMsg]++;
					ipriv->stats[DupBytes] += blocklen(bp);
					qunlock(&ic->outo);
					freeblist(bp);
					return;
				}
				bp->list = f;
				*l = bp;
				qunlock(&ic->outo);
				return;
			}
			l = &f->list;
		}
		*l = bp;
	}
	qunlock(&ic->outo);
}

int
ilsendctl(Conv *ipc, int type, ulong id, ulong ack, int ilspec)
{
	Ilhdr *ih;
	Ilcb *ic;
	Block *bp;
	uchar *p;
	uvlong read;
	int iplen, clen;

	ic = (Ilcb*)ipc->ptcl;
	clen = ic->flowpeer && (type == Ilack || type == Ilstate) ? Ilcreditlen : 0;
	iplen = iliphdrlen(ipc->ipversion);
	bp = allocb(iplen+IL_HDRSIZE+clen);
	bp->wp += iplen+IL_HDRSIZE+clen;
	ilsetip(bp, ipc->ipversion, ipc->laddr, ipc->raddr);
	ih = ilhdr(bp, ipc->ipversion);
	hnputs(ih->illen, IL_HDRSIZE+clen);
	hnputs(ih->ilsrc, ipc->lport);
	hnputs(ih->ildst, ipc->rport);
	hnputl(ih->ilid, id);
	hnputl(ih->ilack, ack);
	ic->acksent = ack;
	ic->acktime = NOW;
	ih->iltype = type;
	ih->ilspec = type == Ilsync ? Ilflow : ilspec;
	if(clen){
		/* Closing the queue discards unread data; it grants no new credit. */
		read = ic->state == Ildraining ? ic->creditread : ic->rxqueued - qlen(ipc->rq);
		p = (uchar*)ih + IL_HDRSIZE;
		hnputl(p, read >> 32);
		hnputl(p+4, read);
		ic->creditread = read;
	}
	ih->ilsum[0] = 0;
	ih->ilsum[1] = 0;

	if(ilcksum)
		hnputs(ih->ilsum, ptclcsum(bp, iplen, IL_HDRSIZE+clen));

	netlog(ipc->p->f, Logipmsg, "ctl(%s id %d ack %d %d->%d)\n",
		iltype[ih->iltype], nhgetl(ih->ilid), nhgetl(ih->ilack),
		nhgets(ih->ilsrc), nhgets(ih->ildst));

	return iloutput(ipc->p->f, bp, ipc->ipversion, ipc->ttl, ipc->tos, ipc);
}

void
ilreject(Fs *f, Block *inbp)
{
	Ilhdr *in, *ih;
	Ip4hdr *h4;
	Ip6hdr *h6;
	Block *bp;
	uchar src[IPaddrlen], dst[IPaddrlen];
	int version, iplen;

	version = (inbp->rp[0] & 0xF0) == IP_VER6 ? V6 : V4;
	iplen = iliphdrlen(version);
	in = ilhdr(inbp, version);
	if(version == V4){
		h4 = (Ip4hdr*)inbp->rp;
		v4tov6(src, h4->src);
		v4tov6(dst, h4->dst);
	} else {
		h6 = (Ip6hdr*)inbp->rp;
		ipmove(src, h6->src);
		ipmove(dst, h6->dst);
	}

	bp = allocb(iplen+IL_HDRSIZE);
	bp->wp += iplen+IL_HDRSIZE;
	ilsetip(bp, version, dst, src);
	ih = ilhdr(bp, version);
	hnputs(ih->illen, IL_HDRSIZE);
	hnputs(ih->ilsrc, nhgets(in->ildst));
	hnputs(ih->ildst, nhgets(in->ilsrc));
	hnputl(ih->ilid, nhgetl(in->ilack));
	hnputl(ih->ilack, nhgetl(in->ilid));
	ih->iltype = Ilclose;
	ih->ilspec = 0;
	ih->ilsum[0] = 0;
	ih->ilsum[1] = 0;

	if(ilcksum)
		hnputs(ih->ilsum, ptclcsum(bp, iplen, IL_HDRSIZE));

	iloutput(f, bp, version, MAXTTL, DFLTTOS, nil);
}

void
ilsettimeout(Ilcb *ic)
{
	ulong pt;

	pt = (ic->delay>>LogAGain)
		+ ic->unackedbytes/(ic->rate>>LogAGain)
		+ (ic->mdev>>(LogDGain-1))
		+ AckDelay;
	if(pt > MaxTimeout)
		pt = MaxTimeout;
	ic->timeout = NOW + pt;
}

void
ilbackoff(Ilcb *ic)
{
	ulong pt;
	int i;

	pt = (ic->delay>>LogAGain)
		+ ic->unackedbytes/(ic->rate>>LogAGain)
		+ (ic->mdev>>(LogDGain-1))
		+ AckDelay;
	for(i = 0; i < ic->rexmit; i++)
		pt = pt + (pt>>1);
	if(pt > MaxTimeout)
		pt = MaxTimeout;
	ic->timeout = NOW + pt;

	if(ic->fasttimeout)
		ic->timeout = NOW+Iltickms;

	ic->rexmit++;
}

// complain if two numbers not within an hour of each other
#define Tfuture (1000*60*60)
int
later(ulong t1, ulong t2, char *x)
{
	int dt;

	dt = t1 - t2;
	if(dt > 0) {
		if(x != nil && dt > Tfuture)
			print("%s: way future %d\n", x, dt);
		return 1;
	}
	if(dt < -Tfuture) {
		if(x != nil)
			print("%s: way past %d\n", x, -dt);
		return 1;
	}
	return 0;
}

void
ilackproc(void *x)
{
	Proc *up = externup();
	Ilcb *ic;
	Conv **s, *p;
	Proto *il;

	il = x;

	while(waserror())
		;
loop:
	tsleep(&up->sleep, return0, 0, Iltickms);
	for(s = il->conv; s && *s; s++) {
		p = *s;
		qlock(&p->ql);
		if(waserror()){
			qunlock(&p->ql);
			continue;
		}
		ic = (Ilcb*)p->ptcl;

		switch(ic->state) {
		case Ilclosed:
		case Illistening:
			break;
		case Ilclosing:
			if(later(NOW, ic->timeout, "timeout0")) {
				if(ic->rexmit > MaxRexmit){
					ilhangup(p, nil);
					break;
				}
				ilsendctl(p, Ilclose, ic->next, ic->recvd, 0);
				ilbackoff(ic);
			}
			break;

		case Ilsyncee:
		case Ilsyncer:
			if(later(NOW, ic->timeout, "timeout1")) {
				if(ic->rexmit > MaxRexmit){
					ilhangup(p, etime);
					break;
				}
				ilsendctl(p, Ilsync, ic->start, ic->recvd, 0);
				ilbackoff(ic);
			}
			break;

		case Ildraining:
		case Ilestablished:
			if(ic->outoforder != nil)
				ilpullup(p);
			if(ic->recvd != ic->acksent)
			if(later(NOW, ic->acktime, "acktime"))
				ilsendctl(p, Ilack, ic->next, ic->recvd, 0);
			if(ic->state == Ilestablished && ic->flowpeer && ic->rxqueued - qlen(p->rq) > ic->creditread)
				ilsendctl(p, Ilack, ic->next, ic->recvd, 0);

			if(later(NOW, ic->querytime, "querytime")){
				if(later(NOW, ic->lastrecv+DeathTime, "deathtime")){
					netlog(il->f, Logil, "il: hangup: deathtime\n");
					ilhangup(p, etime);
					break;
				}
				ilsendctl(p, Ilquery, ic->next, ic->recvd, ilnextqt(ic));
				ic->querytime = NOW + QueryTime;
			}

			if(ic->unacked != nil)
			if(later(NOW, ic->timeout, "timeout2")) {
				if(ic->rexmit > MaxRexmit){
					netlog(il->f, Logil, "il: hangup: too many rexmits\n");
					ilhangup(p, etime);
					break;
				}
				ilsendctl(p, Ilquery, ic->next, ic->recvd, ilnextqt(ic));
				ic->rxquery++;
				ilbackoff(ic);
			}
			break;
		}
		qunlock(&p->ql);
		poperror();
	}
	goto loop;
}

void
ilcbinit(Ilcb *ic)
{
	ic->start = nrand(0x1000000);
	ic->next = ic->start+1;
	ic->recvd = 0;
	ic->window = Defaultwin;
	ic->cwind = Initialwin;
	ic->ssthresh = Defaultwin;
	ic->acks = 0;
	ic->inrecovery = 0;
	ic->recovery = 0;
	ic->flight = 0;
	ic->pending = nil;
	ic->unackedbytes = 0;
	ic->txbytes = 0;
	ic->rxqueued = 0;
	ic->peerread = 0;
	ic->creditread = 0;
	ic->flowpeer = 0;
	ic->unacked = nil;
	ic->outoforder = nil;
	ic->rexmit = 0;
	ic->rxtot = 0;
	ic->rxquery = 0;
	ic->qtx = 1;
	ic->fasttimeout = 0;

	/* timers */
	ic->delay = DefRtt<<LogAGain;
	ic->mdev = DefRtt<<LogDGain;
	ic->rate = DefByteRate<<LogAGain;
	ic->querytime = NOW + QueryTime;
	ic->lastrecv = NOW;	/* or we'll timeout right away */
	ilsettimeout(ic);
}

char*
ilstart(Conv *c, int type, int fasttimeout)
{
	Ilcb *ic;
	Ilpriv *ipriv;
	char kpname[KNAMELEN];

	ipriv = c->p->priv;

	if(ipriv->ackprocstarted == 0){
		qlock(&ipriv->apl);
		if(ipriv->ackprocstarted == 0){
			sprint(kpname, "#I%dilack", c->p->f->dev);
			kproc(kpname, ilackproc, c->p);
			ipriv->ackprocstarted = 1;
		}
		qunlock(&ipriv->apl);
	}

	ic = (Ilcb*)c->ptcl;
	ic->conv = c;

	if(ic->state != Ilclosed)
		return nil;

	ilcbinit(ic);

	if(fasttimeout){
		/* timeout if we can't connect quickly */
		ic->fasttimeout = 1;
		ic->timeout = NOW+Iltickms;
		ic->rexmit = MaxRexmit - 4;
	};

	switch(type) {
	default:
		netlog(c->p->f, Logil, "il: start: type %d\n", type);
		break;
	case IL_LISTEN:
		ic->state = Illistening;
		iphtadd(&ipriv->ht, c);
		break;
	case IL_CONNECT:
		ic->state = Ilsyncer;
		iphtadd(&ipriv->ht, c);
		if(ilsendctl(c, Ilsync, ic->start, ic->recvd, 0) < 0)
			ilhangup(c, "no route");
		break;
	}

	return nil;
}

void
ilfreeq(Ilcb *ic)
{
	Block *bp, *next;

	qlock(&ic->ackq);
	for(bp = ic->unacked; bp; bp = next) {
		next = bp->list;
		freeblist(bp);
	}
	ic->unacked = nil;
	ic->unackedtail = nil;
	ic->unackedbytes = 0;
	ic->flight = 0;
	qunlock(&ic->ackq);
	freeblist(ic->pending);
	ic->pending = nil;

	qlock(&ic->outo);
	for(bp = ic->outoforder; bp; bp = next) {
		next = bp->list;
		freeblist(bp);
	}
	ic->outoforder = nil;
	qunlock(&ic->outo);
}

void
iladvise(Proto *il, Block *bp, char *msg)
{
	uchar source[IPaddrlen], dest[IPaddrlen];
	ushort psource, pdest;
	Ilhdr *h;
	Ip4hdr *h4;
	Ip6hdr *h6;
	Ilcb *ic;
	Conv *s;
	int version, iplen;

	if(BLEN(bp) == 0){
		freeblist(bp);
		return;
	}
	version = (bp->rp[0] & 0xF0) == IP_VER6 ? V6 : V4;
	iplen = iliphdrlen(version);
	if(BLEN(bp) < iplen+10){
		freeblist(bp);
		return;
	}

	h = ilhdr(bp, version);
	if(version == V4){
		h4 = (Ip4hdr*)bp->rp;
		v4tov6(dest, h4->dst);
		v4tov6(source, h4->src);
	} else {
		h6 = (Ip6hdr*)bp->rp;
		ipmove(dest, h6->dst);
		ipmove(source, h6->src);
	}
	pdest = nhgets(h->ildst);
	psource = nhgets(h->ilsrc);

	/* Look for a connection (source/dest reversed; this is the original packet we sent) */
	qlock(&il->ql);
	s = iphtlook(&((Ilpriv*)il->priv)->ht, dest, pdest, source, psource);
	if(s == nil)
		goto raise;
	if(s->ignoreadvice || s->state == Announced)
		goto raise;
	qlock(&s->ql);
	qunlock(&il->ql);
	ic = (Ilcb*)s->ptcl;
	if(ic->state == Ilsyncer)
		ilhangup(s, msg);
	qunlock(&s->ql);
	freeblist(bp);
	return;
raise:
	qunlock(&il->ql);
	freeblist(bp);
}

int
ilnextqt(Ilcb *ic)
{
	int x;

	qlock(&ic->ackq);
	x = ic->qtx;
	if(++x > Nqt)
		x = 1;
	ic->qtx = x;
	ic->qt[x] = ic->next-1;	/* highest xmitted packet */
	ic->qt[0] = ic->qt[x];	/* compatibility with old implementations */
	qunlock(&ic->ackq);

	return x;
}

/* calculate scale constants that converts fast ticks to ms (more or less) */
static void
inittimescale(void)
{
	uvlong hz;

	fastticks(&hz);
	if(hz > 1000){
		scalediv = hz/1000;
		scalemul = 1;
	} else {
		scalediv = 1;
		scalemul = 1000/hz;
	}
}

void
ilinit(Fs *f)
{
	Proto *il;

	inittimescale();

	il = smalloc(sizeof(Proto));
	il->priv = smalloc(sizeof(Ilpriv));
	il->name = "il";
	il->connect = ilconnect;
	il->announce = ilannounce;
	il->state = ilstate;
	il->create = ilcreate;
	il->close = ilclose;
	il->rcv = iliput;
	il->ctl = nil;
	il->advise = iladvise;
	il->stats = ilxstats;
	il->inuse = ilinuse;
	il->gc = nil;
	il->ipproto = IP_ILPROTO;
	il->nc = scalednconv();
	il->ptclsize = sizeof(Ilcb);
	Fsproto(f, il);
}
