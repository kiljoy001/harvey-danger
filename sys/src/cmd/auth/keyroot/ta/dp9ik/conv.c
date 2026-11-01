/*
 * dp9ik ticket and authenticator wire format.
 */
#include <u.h>
#include <libc.h>
#include <authsrv.h>
#include <libsec.h>
#include "dp9ik.h"

int
dp9ik_convT2M(Dp9ikTicket *f, char *ap, int n, Authkey *key)
{
	uchar *p;

	if(n < MAXTICKETLEN)
		return 0;

	p = (uchar*)ap;
	*p++ = f->num;
	memmove(p, f->chal, CHALLEN), p += CHALLEN;
	memmove(p, f->cuid, ANAMELEN), p += ANAMELEN;
	memmove(p, f->suid, ANAMELEN), p += ANAMELEN;
	memmove(p, f->key, NONCELEN), p += NONCELEN;
	return dp9ik_form1B2M(ap, p - (uchar*)ap, key->pakkey);
}

int
dp9ik_convM2T(char *ap, int n, Dp9ikTicket *f, Authkey *key)
{
	uchar buf[MAXTICKETLEN], *p;
	int m;

	if(n < MAXTICKETLEN)
		return 0;
	if(dp9ik_form1check(ap, 8) < 0)
		return 0;
	memmove(buf, ap, MAXTICKETLEN);
	m = dp9ik_form1M2B((char*)buf, MAXTICKETLEN, key->pakkey);
	if(m <= 0)
		return 0;

	memset(f, 0, sizeof(*f));
	p = buf;
	f->num = *p++;
	memmove(f->chal, p, CHALLEN), p += CHALLEN;
	memmove(f->cuid, p, ANAMELEN), p += ANAMELEN;
	f->cuid[ANAMELEN-1] = 0;
	memmove(f->suid, p, ANAMELEN), p += ANAMELEN;
	f->suid[ANAMELEN-1] = 0;
	memmove(f->key, p, NONCELEN);
	f->form = 1;
	return MAXTICKETLEN;
}

int
dp9ik_convA2M(Dp9ikAuthenticator *f, char *ap, int n, Dp9ikTicket *t)
{
	uchar *p;

	if(n < MAXAUTHENTLEN || t->form != 1)
		return 0;

	p = (uchar*)ap;
	*p++ = f->num;
	memmove(p, f->chal, CHALLEN), p += CHALLEN;
	memmove(p, f->rand, NONCELEN), p += NONCELEN;
	return dp9ik_form1B2M(ap, p - (uchar*)ap, t->key);
}

int
dp9ik_convM2A(char *ap, int n, Dp9ikAuthenticator *f, Dp9ikTicket *t)
{
	uchar buf[MAXAUTHENTLEN], *p;
	int m;

	if(n < MAXAUTHENTLEN || t->form != 1)
		return 0;
	if(dp9ik_form1check(ap, 8) < 0)
		return 0;
	memmove(buf, ap, MAXAUTHENTLEN);
	m = dp9ik_form1M2B((char*)buf, MAXAUTHENTLEN, t->key);
	if(m <= 0)
		return 0;

	memset(f, 0, sizeof(*f));
	p = buf;
	f->num = *p++;
	memmove(f->chal, p, CHALLEN), p += CHALLEN;
	memmove(f->rand, p, NONCELEN);
	return MAXAUTHENTLEN;
}
