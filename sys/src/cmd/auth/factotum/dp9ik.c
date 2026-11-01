/*
 * dp9ik - Plan 9 secret key authentication with AuthPAK.
 *
 * The protocol is the 9front dp9ik protocol.  The client and the file
 * server are both clients of the authentication server: each runs an
 * AuthPAK exchange with the AS using its own password-derived key.  The
 * client relays the server's ephemeral public point to the AS and the
 * AS's reply point back to the server.
 *
 * Client protocol:
 *	write challenge[challen]
 *	 read pakreq[ticketreqlen + pakylen]
 *	  write paky[pakylen]
 *	write ticket + authenticator
 *	read authenticator
 *
 * Server protocol:
 * 	read challenge[challen]
 *	 write pakreq[ticketreqlen + pakylen]
 *	  read paky[pakylen]
 *	read ticket + authenticator
 *	write authenticator
 */
#include "dat.h"
#include <dp9ik.h>

struct State
{
	Key	*key;
	Dp9ikTicket t;
	Ticketreq tr;
	Authkey k;
	PAKpriv p;
	char	cchal[CHALLEN];
	uchar	y[PAKYLEN];
	uchar	rand[2*NONCELEN];
	char	tbuf[MAXTICKETLEN + MAXAUTHENTLEN];
	int	tbuflen;
	uchar	*secret;
	int	speakfor;
	int	oracle;
};

enum
{
	/* client phases */
	CHaveChal,
	CNeedPAKreq,
	CHavePAKy,
	CHaveTicket,
	CNeedAuth,

	/* server phases */
	SNeedChal,
	SHavePAKreq,
	SNeedPAKy,
	SNeedTicket,
	SHaveAuth,

	Maxphase,
};

static char *phasenames[Maxphase] =
{
[CHaveChal]	= "CHaveChal",
[CNeedPAKreq]	= "CNeedPAKreq",
[CHavePAKy]	= "CHavePAKy",
[CHaveTicket]	= "CHaveTicket",
[CNeedAuth]	= "CNeedAuth",

[SNeedChal]	= "SNeedChal",
[SHavePAKreq]	= "SHavePAKreq",
[SNeedPAKy]	= "SNeedPAKy",
[SNeedTicket]	= "SNeedTicket",
[SHaveAuth]	= "SHaveAuth",
};

static int gettickets(State*, uchar*, char*, int);

static int
asrequest(int fd, Ticketreq *tr)
{
	char trbuf[TICKREQLEN];
	int n;

	n = convTR2M(tr, trbuf);
	if(write(fd, trbuf, n) != n)
		return -1;
	return 0;
}

static int
dp9ikinit(Proto *p, Fsstate *fss)
{
	State *s;
	int iscli, ret;
	Key *k;
	Keyinfo ki;
	Attr *attr;

	if((iscli = isclient(_strfindattr(fss->attr, "role"))) < 0)
		return failure(fss, nil);

	s = emalloc(sizeof *s);
	fss->phasename = phasenames;
	fss->maxphase = Maxphase;
	fss->proto = p;
	if(iscli){
		fss->phase = CHaveChal;
		genrandom((uchar*)s->cchal, CHALLEN);
	}else{
		fss->phase = SNeedChal;
		s->tr.type = AuthTreq;
		attr = setattr(_copyattr(fss->attr), "proto=%q", p->name);
		mkkeyinfo(&ki, fss, attr);
		ki.user = nil;
		ret = findkey(&k, &ki, "user? dom?");
		_freeattr(attr);
		if(ret != RpcOk){
			free(s);
			return ret;
		}
		safecpy(s->tr.authid, _strfindattr(k->attr, "user"),
			sizeof(s->tr.authid));
		safecpy(s->tr.authdom, _strfindattr(k->attr, "dom"),
			sizeof(s->tr.authdom));
		s->key = k;
		memmove(&s->k, k->priv, sizeof(Authkey));
		genrandom((uchar*)s->tr.chal, sizeof(s->tr.chal));
	}
	s->tbuflen = 0;
	s->secret = nil;
	fss->ps = s;
	return RpcOk;
}

static int
establish(Fsstate *fss)
{
	static char info[] = "Plan 9 session secret";
	AuthInfo *ai;
	State *s;

	s = fss->ps;
	ai = &fss->ai;

	ai->cuid = s->t.cuid;
	ai->suid = s->t.suid;
	ai->nsecret = 256;
	ai->secret = emalloc(ai->nsecret);
	hkdf_x(s->rand, 2*NONCELEN, (uchar*)info, sizeof(info)-1,
		s->t.key, NONCELEN, ai->secret, ai->nsecret,
		hmac_sha2_256, SHA2_256dlen);
	s->secret = ai->secret;
	fss->haveai = 1;
	fss->phase = Established;
	return RpcOk;
}


static int
oracle_available(void)
{
	int fd;

	fd = open("/mnt/keyroot/ctl", OREAD);
	if(fd < 0)
		return 0;
	close(fd);
	return 1;
}

static int
oracle_rw(char *op, void *in, int nin, void *out, int nout)
{
	char path[64];
	int fd, n;

	snprint(path, sizeof(path), "/mnt/keyroot/dp9ik/%s", op);
	fd = open(path, OWRITE);
	if(fd < 0)
		return -1;
	n = write(fd, in, nin);
	close(fd);
	if(n != nin)
		return -1;
	fd = open(path, OREAD);
	if(fd < 0)
		return -1;
	n = read(fd, out, nout);
	close(fd);
	return n;
}

static int
oracle_as(State *s, uchar *serverY, uchar *clientY, uchar *asreply,
	  char *tickets)
{
	int asfd;
	char *dom;

	if((dom = _strfindattr(s->key->attr, "dom")) == nil)
		return -1;
	asfd = _authdial(nil, dom);
	if(asfd < 0)
		return -1;
	s->tr.type = AuthPAK;
	if(asrequest(asfd, &s->tr) < 0 ||
	   write(asfd, serverY, PAKYLEN) != PAKYLEN ||
	   write(asfd, clientY, PAKYLEN) != PAKYLEN)
		goto fail;
	if(_asrdresp(asfd, (char*)asreply, 2*PAKYLEN) != 2*PAKYLEN)
		goto fail;
	s->tr.type = AuthTreq;
	if(asrequest(asfd, &s->tr) < 0)
		goto fail;
	if(_asrdresp(asfd, tickets, 2*MAXTICKETLEN) != 2*MAXTICKETLEN)
		goto fail;
	close(asfd);
	return 0;
fail:
	close(asfd);
	return -1;
}

/* Try the keyroot oracle.  Return 0 on success, -1 to fall back. */
static int
oracle_start(State *s, uchar *req, int nreq)
{
	uchar in[TICKREQLEN + PAKYLEN];
	uchar y[PAKYLEN], asreply[2*PAKYLEN], tickets[2*MAXTICKETLEN];
	uchar fin[2*PAKYLEN + 2*MAXTICKETLEN];
	uchar out[PAKYLEN + MAXTICKETLEN + MAXAUTHENTLEN];

	if(nreq != sizeof(in))
		return -1;
	memmove(in, req, sizeof(in));
	if(oracle_rw("init", in, sizeof(in), y, sizeof(y)) != sizeof(y))
		return -1;
	if(oracle_as(s, in + TICKREQLEN, y, asreply, (char*)tickets) < 0)
		return -1;
	memmove(fin, asreply, sizeof(asreply));
	memmove(fin + sizeof(asreply), tickets, sizeof(tickets));
	if(oracle_rw("finish", fin, sizeof(fin), out, sizeof(out)) !=
	   sizeof(out))
		return -1;
	memmove(s->y, out, PAKYLEN);
	memmove(s->tbuf, out + PAKYLEN, MAXTICKETLEN + MAXAUTHENTLEN);
	s->tbuflen = MAXTICKETLEN + MAXAUTHENTLEN;
	s->oracle = 1;
	return 0;
}

static int
dp9ikread(Fsstate *fss, void *a, u32 *n)
{
	int m;
	State *s;

	s = fss->ps;
	switch(fss->phase){
	default:
		return phaseerror(fss, "read");

	case CHaveChal:
		m = CHALLEN;
		if(*n < m)
			return toosmall(fss, m);
		*n = m;
		memmove(a, s->cchal, m);
		fss->phase = CNeedPAKreq;
		return RpcOk;

	case CHavePAKy:
		m = PAKYLEN;
		if(*n < m)
			return toosmall(fss, m);
		*n = m;
		memmove(a, s->y, m);
		fss->phase = CHaveTicket;
		return RpcOk;

	case CHaveTicket:
		m = s->tbuflen;
		if(*n < m)
			return toosmall(fss, m);
		*n = m;
		memmove(a, s->tbuf, m);
		fss->phase = CNeedAuth;
		return RpcOk;

	case SHavePAKreq:
		m = TICKREQLEN + PAKYLEN;
		if(*n < m)
			return toosmall(fss, m);
		s->tr.type = AuthPAK;
		*n = convTR2M(&s->tr, a);
		authpak_new(&s->p, &s->k, (uchar*)a + *n, 1);
		*n += PAKYLEN;
		fss->phase = SNeedPAKy;
		return RpcOk;

	case SHaveAuth:
		m = s->tbuflen;
		if(*n < m)
			return toosmall(fss, m);
		*n = m;
		memmove(a, s->tbuf, m);
		return establish(fss);
	}
}

static int
dp9ikwrite(Fsstate *fss, void *a, u32 n)
{
	int m, ret, sret;
	char tbuf[2*MAXTICKETLEN], *user;
	Attr *attr;
	Dp9ikAuthenticator auth;
	State *s;
	Key *srvkey;
	Keyinfo ki;

	s = fss->ps;
	switch(fss->phase){
	default:
		return phaseerror(fss, "write");

	case SNeedChal:
		m = CHALLEN;
		if(n < m)
			return toosmall(fss, m);
		memmove(s->cchal, a, m);
		fss->phase = SHavePAKreq;
		return RpcOk;

	case CNeedPAKreq:
		m = TICKREQLEN + PAKYLEN;
		if(n < m)
			return toosmall(fss, m);
		convM2TR(a, &s->tr);
		if(s->tr.type != AuthPAK)
			return failure(fss, Easproto);

		if(s->key != nil)
			closekey(s->key);

		attr = _delattr(_delattr(_copyattr(fss->attr), "role"),
				"user");
		attr = setattr(attr, "proto=%q", fss->proto->name);
		user = _strfindattr(fss->attr, "user");

		srvkey = nil;
		s->speakfor = 0;
		sret = RpcFailure;
		if(user == nil || strcmp(user, fss->sysuser) == 0){
			mkkeyinfo(&ki, fss, attr);
			ki.user = nil;
			sret = findkey(&srvkey, &ki,
				"role=speakfor dom=%q user?", s->tr.authdom);
		}
		if(user != nil)
			attr = setattr(attr, "user=%q", user);
		mkkeyinfo(&ki, fss, attr);
		ret = findkey(&s->key, &ki,
			"role=client dom=%q %s", s->tr.authdom,
			fss->proto->keyprompt);
		if(ret == RpcOk)
			closekey(srvkey);
		else if(sret == RpcOk){
			s->key = srvkey;
			s->speakfor = 1;
		}else if(ret == RpcConfirm || sret == RpcConfirm){
			_freeattr(attr);
			return RpcConfirm;
		}else{
			_freeattr(attr);
			return ret;
		}

		/* Fill in the rest of the request. */
		safecpy(s->tr.hostid, _strfindattr(s->key->attr, "user"),
			sizeof(s->tr.hostid));
		if(s->speakfor)
			safecpy(s->tr.uid, fss->sysuser, sizeof(s->tr.uid));
		else
			safecpy(s->tr.uid, s->tr.hostid, sizeof(s->tr.uid));

		s->oracle = 0;
		if(_strfindattr(s->key->privattr, "!local") == nil &&
		   oracle_available() &&
		   oracle_start(s, (uchar*)a, TICKREQLEN + PAKYLEN) == 0){
			s->key->successes++;
			_freeattr(attr);
			fss->phase = CHavePAKy;
			return RpcOk;
		}
		if(s->key->priv == nil){
			_freeattr(attr);
			return failure(fss, "keyroot unavailable");
		}
		memmove(&s->k, s->key->priv, sizeof(Authkey));
		ret = gettickets(s, (uchar*)a + TICKREQLEN, tbuf,
				 sizeof(tbuf));
		if(ret <= 0){
			_freeattr(attr);
			return failure(fss, nil);
		}
		m = dp9ik_convM2T(tbuf, ret, &s->t, &s->k);
		if(m <= 0 || s->t.num != AuthTc ||
		   memcmp(s->t.chal, s->tr.chal, CHALLEN) != 0){
			_freeattr(attr);
			return failure(fss, Easproto);
		}
		s->key->successes++;
		_freeattr(attr);
		ret -= m;
		memmove(s->tbuf, tbuf + m, ret);
		genrandom(s->rand, NONCELEN);
		memset(&auth, 0, sizeof(auth));
		auth.num = AuthAc;
		memmove(auth.chal, s->tr.chal, CHALLEN);
		memmove(auth.rand, s->rand, NONCELEN);
		ret += dp9ik_convA2M(&auth, s->tbuf + ret,
				     sizeof(s->tbuf) - ret, &s->t);
		s->tbuflen = ret;
		fss->phase = CHavePAKy;
		return RpcOk;

	case SNeedPAKy:
		m = PAKYLEN;
		if(n < m)
			return toosmall(fss, m);
		if(authpak_finish(&s->p, &s->k, a))
			return failure(fss, Easproto);
		fss->phase = SNeedTicket;
		return RpcOk;

	case SNeedTicket:
		m = dp9ik_convM2T(a, n, &s->t, &s->k);
		if(m <= 0 || s->t.num != AuthTs ||
		   memcmp(s->t.chal, s->tr.chal, CHALLEN) != 0)
			return failure(fss, Easproto);
		if(dp9ik_convM2A((char*)a + m, n - m, &auth, &s->t) <= 0)
			return toosmall(fss, MAXTICKETLEN + MAXAUTHENTLEN);
		if(auth.num != AuthAc ||
		   memcmp(auth.chal, s->tr.chal, CHALLEN) != 0)
			return failure(fss, Easproto);
		memmove(s->rand, auth.rand, NONCELEN);
		genrandom(s->rand + NONCELEN, NONCELEN);
		memset(&auth, 0, sizeof(auth));
		auth.num = AuthAs;
		memmove(auth.chal, s->cchal, CHALLEN);
		memmove(auth.rand, s->rand + NONCELEN, NONCELEN);
		s->tbuflen = dp9ik_convA2M(&auth, s->tbuf,
					   sizeof(s->tbuf), &s->t);
		fss->phase = SHaveAuth;
		return RpcOk;

	case CNeedAuth:
		if(s->oracle){
			uchar in[MAXAUTHENTLEN + CHALLEN];
			uchar secret[256];

			if(n < MAXAUTHENTLEN)
				return toosmall(fss, MAXAUTHENTLEN);
			memmove(in, a, MAXAUTHENTLEN);
			memmove(in + MAXAUTHENTLEN, s->cchal, CHALLEN);
			if(oracle_rw("reply", in, sizeof(in), secret,
				     sizeof(secret)) != sizeof(secret))
				return failure(fss, Easproto);
			fss->ai.cuid = s->tr.uid;
			fss->ai.suid = s->tr.uid;
			fss->ai.nsecret = sizeof(secret);
			fss->ai.secret = emalloc(sizeof(secret));
			memmove(fss->ai.secret, secret, sizeof(secret));
			s->secret = fss->ai.secret;
			fss->haveai = 1;
			fss->phase = Established;
			return RpcOk;
		}
		if(dp9ik_convM2A(a, n, &auth, &s->t) <= 0)
			return toosmall(fss, MAXAUTHENTLEN);
		if(auth.num != AuthAs ||
		   memcmp(auth.chal, s->cchal, CHALLEN) != 0)
			return failure(fss, Easproto);
		memmove(s->rand + NONCELEN, auth.rand, NONCELEN);
		return establish(fss);
	}
}

static void
dp9ikclose(Fsstate *fss)
{
	State *s;

	s = fss->ps;
	if(s->secret != nil){
		free(s->secret);
		s->secret = nil;
	}
	if(s->key != nil){
		closekey(s->key);
		s->key = nil;
	}
	memset(s, 0, sizeof(State));
	free(s);
}

static int
dp9ikaddkey(Key *k, int before)
{
	Authkey *akey;
	char *s, *u;

	u = _strfindattr(k->attr, "user");
	if(u == nil){
		werrstr("no user attribute");
		return -1;
	}
	akey = emalloc(sizeof(Authkey));
	if((s = _strfindattr(k->privattr, "!hex")) != nil){
		if(dec16(akey->aes, AESKEYLEN, s, strlen(s)) != AESKEYLEN){
			free(akey);
			werrstr("malformed key data");
			return -1;
		}
	}else if((s = _strfindattr(k->privattr, "!password")) != nil){
		dp9ik_passtokey(akey, s);
	}else if(_strfindattr(k->privattr, "!oracle") != nil){
		free(akey);
		k->priv = nil;
		return replacekey(k, before);
	}else{
		free(akey);
		werrstr("no key data");
		return -1;
	}
	authpak_hash(akey, u);
	k->priv = akey;
	return replacekey(k, before);
}

static void
dp9ikclosekey(Key *k)
{
	if(k->priv == nil)
		return;
	memset(k->priv, 0, sizeof(Authkey));
	free(k->priv);
}

static int
getastickets(State *s, uchar *y, char *tbuf, int tbuflen)
{
	int asfd, rv;
	char *dom;

	USED(tbuflen);
	if((dom = _strfindattr(s->key->attr, "dom")) == nil){
		werrstr("auth key has no domain");
		return -1;
	}
	asfd = _authdial(nil, dom);
	if(asfd < 0)
		return -1;
	rv = -1;
	if(y != nil){
		s->tr.type = AuthPAK;
		if(asrequest(asfd, &s->tr) < 0 ||
		   write(asfd, y, PAKYLEN) != PAKYLEN)
			goto out;
		authpak_new(&s->p, &s->k, (uchar*)tbuf, 1);
		if(write(asfd, tbuf, PAKYLEN) != PAKYLEN)
			goto out;
		if(_asrdresp(asfd, tbuf, 2*PAKYLEN) != 2*PAKYLEN)
			goto out;
		memmove(s->y, tbuf, PAKYLEN);
		if(authpak_finish(&s->p, &s->k,
				  (uchar*)tbuf + PAKYLEN) < 0)
			goto out;
	}
	s->tr.type = AuthTreq;
	if(asrequest(asfd, &s->tr) < 0)
		goto out;
	rv = _asrdresp(asfd, tbuf, 2*MAXTICKETLEN);
out:
	close(asfd);
	return rv;
}

static int
mkservertickets(State *s, uchar *y, char *tbuf, int tbuflen)
{
	uchar paky[PAKYLEN];
	Dp9ikTicket t;
	int ret;

	if(strcmp(s->tr.authid, s->tr.hostid) != 0)
		return -1;
	memset(&t, 0, sizeof(t));
	if(y != nil){
		memmove(paky, y, PAKYLEN);
		t.form = 1;
		authpak_new(&s->p, &s->k, s->y, 0);
		if(authpak_finish(&s->p, &s->k, paky) < 0)
			return -1;
	}
	t.num = AuthTc;
	memmove(t.chal, s->tr.chal, CHALLEN);
	strcpy(t.cuid, s->tr.uid);
	strcpy(t.suid, s->tr.uid);
	genrandom(t.key, NONCELEN);
	ret = dp9ik_convT2M(&t, tbuf, tbuflen, &s->k);
	if(ret <= 0)
		return -1;
	t.num = AuthTs;
	return ret + dp9ik_convT2M(&t, tbuf + ret, tbuflen - ret, &s->k);
}

static int
gettickets(State *s, uchar *y, char *tbuf, int tbuflen)
{
	int ret;

	if(s->tr.authdom[0] == 0 || s->tr.authid[0] == 0 ||
	   s->tr.hostid[0] == 0 || s->tr.uid[0] == 0){
		werrstr("bad ticket request");
		return -1;
	}
	ret = getastickets(s, y, tbuf, tbuflen);
	if(ret > 0)
		return ret;
	return mkservertickets(s, y, tbuf, tbuflen);
}

Proto dp9ik = {
	.name		= "dp9ik",
	.init		= dp9ikinit,
	.addkey		= dp9ikaddkey,
	.closekey	= dp9ikclosekey,
	.write		= dp9ikwrite,
	.read		= dp9ikread,
	.close		= dp9ikclose,
	.keyprompt	= "user? !password?",
};
