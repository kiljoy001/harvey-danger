/*
 * fido - Hardware Security Fob (FIDO2 / WebAuthn / YubiKey / Nitrokey HSM)
 * authentication and user login protocol for Factotum.
 *
 * Bridges Factotum (/mnt/factotum) with the browser /n/host/fido service
 * (WebAuthn FIDO2 + WebUSB hardware security tokens) and switches the
 * hostowner identity (#c/hostowner) when role=login or role=client succeeds.
 *
 * Key format in /mnt/factotum/ctl:
 *   key proto=fido user=<user> [dom=<dom>] [fob=<type:vid:pid>] [id=<id>] !secret=<64hex>
 *
 * Client RPC protocol (/mnt/factotum/rpc):
 *   start proto=fido role=client [user=<user>] [mode=fido2|usb|auto]
 *   write [challenge]   (optional custom challenge; otherwise 32-byte random)
 *   read                -> returns "user=<user> fob=<fob> id=<id> sig=<sig>"
 *                          and transitions to Established with 32-byte secret in AuthInfo,
 *                          loads proto=fido and proto=dp9ik keys into ring,
 *                          and updates #c/hostowner + /env/user.
 */

#include "dat.h"

struct State
{
	Key	*key;
	char	user[Maxname];
	char	dom[Maxname];
	char	mode[32];
	char	fob[64];
	char	credid[128];
	char	sig[256];
	uchar	chal[32];
	uchar	secret[32];
	char	reply[512];
	int	setowner;
};

enum
{
	CHaveChal,
	CHaveAuth,
	Maxphase,
};

static char *phasenames[Maxphase] =
{
[CHaveChal]	= "CHaveChal",
[CHaveAuth]	= "CHaveAuth",
};

static int
openhostfido(char *file, int mode)
{
	char path[128];
	int fd;

	snprint(path, sizeof(path), "/n/host/fido/%s", file);
	fd = open(path, mode);
	if(fd >= 0)
		return fd;
	snprint(path, sizeof(path), "/vps/host/fido/%s", file);
	fd = open(path, mode);
	if(fd >= 0)
		return fd;
	snprint(path, sizeof(path), "/n/host/host/fido/%s", file);
	return open(path, mode);
}

static char*
kvfind(char *line, char *key, char *out, int nout)
{
	char pat[64], *p, *e;
	int klen, n;

	out[0] = 0;
	snprint(pat, sizeof(pat), "%s=", key);
	klen = strlen(pat);
	p = line;
	while((p = strstr(p, pat)) != nil){
		if(p == line || p[-1] == ' ' || p[-1] == '\t'){
			p += klen;
			e = p;
			while(*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r')
				e++;
			n = e - p;
			if(n >= nout)
				n = nout - 1;
			memmove(out, p, n);
			out[n] = 0;
			return out;
		}
		p += klen;
	}
	return nil;
}

static void
syncuserenv(char *u)
{
	int fd;

	if(u == nil || u[0] == 0)
		return;
	writehostowner(u);
	owner = estrdup(u);
	fd = create("/env/user", OWRITE, 0666);
	if(fd < 0)
		fd = open("#ec/user", OWRITE|OTRUNC);
	if(fd >= 0){
		write(fd, u, strlen(u));
		close(fd);
	}
}

static void
injectkeys(State *s, char *secret_hex)
{
	char cmd[512];
	char *dom;

	dom = s->dom[0] ? s->dom : "aspera";
	snprint(cmd, sizeof(cmd),
		"key proto=fido dom=%s user=%s fob=%s id=%s !secret=%s",
		dom, s->user,
		s->fob[0] ? s->fob : "fob",
		s->credid[0] ? s->credid : "0",
		secret_hex);
	ctlwrite(cmd, 0);

	if(strlen(secret_hex) == 64){
		snprint(cmd, sizeof(cmd),
			"key proto=dp9ik dom=%s user=%s !hex=%s",
			dom, s->user, secret_hex);
		ctlwrite(cmd, 0);
	}
}

static int
runfidoauth(Fsstate *fss, State *s)
{
	char chalhex[68], cmd[256], buf[1024], secret_hex[128];
	int fd, i, n;

	enc16(chalhex, sizeof(chalhex), s->chal, sizeof(s->chal));
	for(i = 0; chalhex[i]; i++)
		if(chalhex[i] >= 'A' && chalhex[i] <= 'F')
			chalhex[i] += 'a' - 'A';

	fd = openhostfido("resp", OWRITE|OTRUNC);
	if(fd >= 0){
		write(fd, "pending\n", 8);
		close(fd);
	}

	fd = openhostfido("ctl", OWRITE|OTRUNC);
	if(fd < 0)
		return failure(fss, "fido bridge unavailable (/n/host/fido/ctl)");

	snprint(cmd, sizeof(cmd), "auth %s %s %s\n",
		s->user[0] ? s->user : "*",
		chalhex,
		s->mode[0] ? s->mode : "auto");
	if(write(fd, cmd, strlen(cmd)) < 0){
		close(fd);
		return failure(fss, "fido ctl write failed: %r");
	}
	close(fd);

	/* Poll /n/host/fido/resp up to 30 seconds for physical fob touch / USB check */
	for(i = 0; i < 120; i++){
		sleep(250);
		fd = openhostfido("resp", OREAD);
		if(fd < 0)
			continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if(n <= 0)
			continue;
		buf[n] = 0;
		while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
			buf[--n] = 0;
		if(strncmp(buf, "pending", 7) == 0 || strncmp(buf, "waiting", 7) == 0 || buf[0] == 0)
			continue;
		if(strncmp(buf, "err", 3) == 0){
			return failure(fss, "%s", buf);
		}
		if(strncmp(buf, "ok ", 3) == 0){
			kvfind(buf, "user", s->user, sizeof(s->user));
			kvfind(buf, "fob", s->fob, sizeof(s->fob));
			kvfind(buf, "id", s->credid, sizeof(s->credid));
			kvfind(buf, "sig", s->sig, sizeof(s->sig));
			secret_hex[0] = 0;
			kvfind(buf, "secret", secret_hex, sizeof(secret_hex));
			if(s->user[0] == 0)
				return failure(fss, "fido response missing user");
			if(strlen(secret_hex) == 64)
				dec16(s->secret, 32, secret_hex, 64);
			else
				sha2_256((uchar*)buf, strlen(buf), s->secret, nil);

			injectkeys(s, secret_hex);
			if(s->setowner)
				syncuserenv(s->user);

			snprint(s->reply, sizeof(s->reply),
				"user=%s fob=%s id=%s sig=%s",
				s->user,
				s->fob[0] ? s->fob : "fob",
				s->credid[0] ? s->credid : "0",
				s->sig[0] ? s->sig : "ok");
			fss->phase = CHaveAuth;
			return RpcOk;
		}
	}
	return failure(fss, "fido hardware verification timed out");
}

static int
fidoinit(Proto *p, Fsstate *fss)
{
	State *s;
	char *u, *d, *m, *role;

	s = emalloc(sizeof(*s));
	fss->phasename = phasenames;
	fss->maxphase = Maxphase;
	fss->proto = p;

	u = _strfindattr(fss->attr, "user");
	if(u != nil)
		safecpy(s->user, u, sizeof(s->user));
	d = _strfindattr(fss->attr, "dom");
	if(d != nil)
		safecpy(s->dom, d, sizeof(s->dom));
	else
		safecpy(s->dom, "aspera", sizeof(s->dom));
	m = _strfindattr(fss->attr, "mode");
	if(m != nil)
		safecpy(s->mode, m, sizeof(s->mode));
	else
		safecpy(s->mode, "auto", sizeof(s->mode));

	role = _strfindattr(fss->attr, "role");
	s->setowner = (role == nil || strcmp(role, "login") == 0 || strcmp(role, "client") == 0);

	genrandom(s->chal, sizeof(s->chal));
	fss->phase = CHaveChal;
	fss->ps = s;
	return RpcOk;
}

static int
fidowrite(Fsstate *fss, void *va, u32 n)
{
	State *s;

	s = fss->ps;
	if(fss->phase != CHaveChal)
		return phaseerror(fss, "write");
	if(n > 0){
		if(n == 32)
			memmove(s->chal, va, 32);
		else
			sha2_256((uchar*)va, n, s->chal, nil);
	}
	return runfidoauth(fss, s);
}

static int
fidoread(Fsstate *fss, void *va, u32 *n)
{
	State *s;
	int m, ret;

	s = fss->ps;
	if(fss->phase == CHaveChal){
		ret = runfidoauth(fss, s);
		if(ret != RpcOk)
			return ret;
	}
	if(fss->phase != CHaveAuth)
		return phaseerror(fss, "read");

	m = strlen(s->reply);
	if(*n < m)
		return toosmall(fss, m);
	*n = m;
	memmove(va, s->reply, m);

	fss->ai.cuid = s->user;
	fss->ai.suid = s->user;
	fss->ai.nsecret = sizeof(s->secret);
	fss->ai.secret = emalloc(sizeof(s->secret));
	memmove(fss->ai.secret, s->secret, sizeof(s->secret));
	fss->haveai = 1;
	fss->phase = Established;
	return RpcOk;
}

static void
fidoclose(Fsstate *fss)
{
	State *s;

	s = fss->ps;
	if(s == nil)
		return;
	if(s->key != nil)
		closekey(s->key);
	if(fss->ai.secret != nil){
		free(fss->ai.secret);
		fss->ai.secret = nil;
	}
	memset(s, 0, sizeof(*s));
	free(s);
}

static int
fidoaddkey(Key *k, int before)
{
	char *u, *sec;
	uchar *raw;

	u = _strfindattr(k->attr, "user");
	if(u == nil){
		werrstr("fido key requires user attribute");
		return -1;
	}
	raw = emalloc(32);
	if((sec = _strfindattr(k->privattr, "!secret")) != nil){
		if(dec16(raw, 32, sec, strlen(sec)) != 32)
			sha2_256((uchar*)sec, strlen(sec), raw, nil);
	}else if((sec = _strfindattr(k->privattr, "!hex")) != nil){
		if(dec16(raw, 32, sec, strlen(sec)) != 32)
			sha2_256((uchar*)sec, strlen(sec), raw, nil);
	}else{
		genrandom(raw, 32);
	}
	k->priv = raw;
	return replacekey(k, before);
}

static void
fidoclosekey(Key *k)
{
	if(k->priv == nil)
		return;
	memset(k->priv, 0, 32);
	free(k->priv);
}

Proto fido = {
	.name		= "fido",
	.init		= fidoinit,
	.addkey		= fidoaddkey,
	.closekey	= fidoclosekey,
	.write		= fidowrite,
	.read		= fidoread,
	.close		= fidoclose,
	.keyprompt	= "user? fob? id? !secret?",
};
