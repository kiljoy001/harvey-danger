/*
 * dp9ik password key derivation: DES for legacy fallback, AES for
 * AuthPAK, and the derived Authkey container.
 */
#include <u.h>
#include <libc.h>
#include <authsrv.h>
#include <libsec.h>
#include "dp9ik.h"

static void
passtoaeskey(uchar key[AESKEYLEN], char *p)
{
	static char salt[] = "Plan 9 key derivation";

	pbkdf2_x((uchar*)p, strlen(p), (uchar*)salt, sizeof(salt)-1, 9001,
		key, AESKEYLEN, hmac_sha1, SHA1dlen);
}

void
dp9ik_passtokey(Authkey *key, char *pw)
{
	memset(key, 0, sizeof(*key));
	passtokey((char*)key->des, pw);
	passtoaeskey(key->aes, pw);
}
