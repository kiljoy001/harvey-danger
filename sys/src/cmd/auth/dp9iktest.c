/*
 * dp9ik self-test: password derivation, HKDF, form1, ticket formats
 * and the AuthPAK exchange.
 */
#include <u.h>
#include <libc.h>
#include <authsrv.h>
#include <libsec.h>
#include <dp9ik.h>

enum
{
	CheckAESKEYLEN	= 1 / (AESKEYLEN == 16),
	CheckNONCELEN	= 1 / (NONCELEN == 32),
	CheckAuthPAK	= 1 / (AuthPAK == 19),
	CheckPAKKEYLEN	= 1 / (PAKKEYLEN == 32),
	CheckPAKSLEN	= 1 / (PAKSLEN == ((448+7)/8)),
};

static uchar
allzero(void *p, int n)
{
	uchar *b;

	for(b = p; n > 0; b++, n--)
		if(*b != 0)
			return 0;
	return 1;
}

void
main(void)
{
	static char pw[] = "testpassword";
	static uchar wantaes[AESKEYLEN] = {
		0x39, 0x63, 0x61, 0xfc, 0xcb, 0x1d, 0xe4, 0x23,
		0x29, 0x61, 0x9a, 0xf1, 0x00, 0xed, 0xdb, 0xa3,
	};
	static uchar wantsha256[SHA2_256dlen] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
		0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
	};
	static uchar ikm[22] = {
		0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
		0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
		0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
	};
	static uchar salt[] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
		0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
	};
	static uchar info[] = {
		0xf0, 0xf1, 0xf2, 0xf3, 0xf4,
		0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
	};
	static uchar wantokm[42] = {
		0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a,
		0x90, 0x43, 0x4f, 0x64, 0xd0, 0x36, 0x2f, 0x2a,
		0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a, 0x5a, 0x4c,
		0x5d, 0xb0, 0x2d, 0x56, 0xec, 0xc4, 0xc5, 0xbf,
		0x34, 0x00, 0x72, 0x08, 0xd5, 0xb8, 0x87, 0x18,
		0x58, 0x65,
	};
	static char wantpakhashhex[] =
		"04fdbd490bdc3e9e35aa052d339610af63e4263007cdb9a1895115ad4760faa8"
		"63cf3d704e0f9090c36aa91ef5c20e7056f20b038b36bf73ab69a724ce39f03b"
		"669c1e107bd8400587b24f87a90f17ae047b5ac42bb6baa5760bef777442f8f8"
		"1bc5683ed38ec5b3784a59180e09a5deb3bf2ebd167bb8311d5c4b4b863cf499"
		"8769603fbc80147d04fd8d60013682d6a96fe49353e6099c997be9778bdb9ba0"
		"a8cdb9951d2278b83ee973a330e3233d2a4fc8dd58205f89b0b938c445632600"
		"e288ad978fa9e46aed6cf6f07f4b1ba77eb1a7a190cae701a5a7aee5adfceb37"
		"8aa9f1ce06b15ae12e8244a66f3e82a84c20755700d6c5079bbd67f2440388f1"
		"2a89d88228f51f0f62656dff78f7fbd88a85949758bac77dbc3789e2cf00b7a0"
		"2a91065fed13780d2db359735c7e37553590cccc000bd54c20185f4a8f603cc8"
		"1641a3053e1abf02ad7508b806b2b4f2a19c50f1b28b8462edcea783642740f6"
		"88d5ea6677aa1bba77f3387c0f246e51fc116aa94ae4c557857df7ef27699fa6"
		"5a3d9556ae1bd81b80479b6e878c655d0fcee224f8553b907e96ee02ad1ab593"
		"cccc2235f4aafb4bdc5187710b1543148b7bbcd0da4588fadee77623c0e4c15b";
	Authkey k, ck, sk;
	PAKpriv cp, sp;
	Dp9ikAuthenticator a1, a2;
	Dp9ikTicket t1, t2;
	uchar digest[SHA2_256dlen];
	uchar okm[sizeof(wantokm)];
	uchar cy[PAKYLEN], sy[PAKYLEN];
	uchar wantpakhash[PAKHASHLEN];
	uchar fkey[PAKKEYLEN];
	uchar fmsg[64], fcopy[64];
	char abuf[MAXAUTHENTLEN];
	char tbuf[MAXTICKETLEN];
	int flen, felen;

	if(sizeof(k.des) != DESKEYLEN)
		sysfatal("bad Authkey.des size: %d", (int)sizeof(k.des));
	if(sizeof(k.aes) != AESKEYLEN)
		sysfatal("bad Authkey.aes size: %d", (int)sizeof(k.aes));
	if(sizeof(k.pakkey) != PAKKEYLEN)
		sysfatal("bad Authkey.pakkey size: %d", (int)sizeof(k.pakkey));
	if(sizeof(k.pakhash) != PAKHASHLEN)
		sysfatal("bad Authkey.pakhash size: %d", (int)sizeof(k.pakhash));
	if(sizeof(cp.x) != PAKXLEN)
		sysfatal("bad PAKpriv.x size: %d", (int)sizeof(cp.x));
	if(sizeof(cp.y) != PAKYLEN)
		sysfatal("bad PAKpriv.y size: %d", (int)sizeof(cp.y));

	memset(&k, 0x5a, sizeof(k));
	dp9ik_passtokey(&k, pw);
	if(allzero(k.des, sizeof(k.des)))
		sysfatal("DES key is all zeros");
	if(memcmp(k.aes, wantaes, AESKEYLEN) != 0)
		sysfatal("AES key mismatch");
	print("dp9ik: password derivation ok\n");

	sha2_256((uchar*)"abc", 3, digest, nil);
	if(memcmp(digest, wantsha256, sizeof(digest)) != 0)
		sysfatal("SHA-256 digest mismatch");
	print("dp9ik: sha256 ok\n");

	memset(okm, 0, sizeof(okm));
	hkdf_x(salt, sizeof(salt), info, sizeof(info), ikm, sizeof(ikm),
		okm, sizeof(okm), hmac_sha2_256, SHA2_256dlen);
	if(memcmp(okm, wantokm, sizeof(okm)) != 0)
		sysfatal("HKDF output mismatch");
	print("dp9ik: hkdf ok\n");

	memset(fkey, 0, sizeof(fkey));
	memset(fmsg, 0, sizeof(fmsg));
	fmsg[0] = AuthAs;
	memmove(fmsg+1, "form1 roundtrip payload", 23);
	memmove(fcopy, fmsg, 24);
	flen = dp9ik_form1B2M((char*)fmsg, 24, fkey);
	if(flen != 12+16+23)
		sysfatal("form1B2M length mismatch: %d", flen);
	if(dp9ik_form1check((char*)fmsg, flen) != AuthAs)
		sysfatal("form1check mismatch");
	felen = dp9ik_form1M2B((char*)fmsg, flen, fkey);
	if(felen != 24)
		sysfatal("form1M2B length mismatch: %d", felen);
	if(memcmp(fmsg, fcopy, 24) != 0)
		sysfatal("form1 roundtrip mismatch");
	fmsg[flen-1] ^= 1;
	if(dp9ik_form1M2B((char*)fmsg, flen, fkey) >= 0)
		sysfatal("form1 tamper check failed");
	print("dp9ik: form1 ok\n");

	memset(&t1, 0, sizeof(t1));
	t1.num = AuthTs;
	t1.form = 1;
	memset(t1.key, 0x33, NONCELEN);

	memset(&a1, 0, sizeof(a1));
	memset(&a2, 0, sizeof(a2));
	a1.num = AuthAc;
	memmove(a1.chal, "chalAUTH", CHALLEN);
	memset(a1.rand, 0x44, NONCELEN);
	flen = dp9ik_convA2M(&a1, abuf, sizeof(abuf), &t1);
	if(flen != MAXAUTHENTLEN)
		sysfatal("form1 auth length mismatch: %d", flen);
	if(dp9ik_convM2A(abuf, flen, &a2, &t1) != MAXAUTHENTLEN)
		sysfatal("form1 auth parse failed");
	if(a2.num != a1.num || memcmp(a2.chal, a1.chal, CHALLEN) != 0
	|| memcmp(a2.rand, a1.rand, NONCELEN) != 0)
		sysfatal("form1 auth roundtrip mismatch");
	abuf[flen-1] ^= 1;
	memset(&a2, 0, sizeof(a2));
	if(dp9ik_convM2A(abuf, flen, &a2, &t1) != 0)
		sysfatal("form1 auth tamper check failed");
	print("dp9ik: authenticator ok\n");

	memset(wantpakhash, 0, sizeof(wantpakhash));
	if(dec16(wantpakhash, sizeof(wantpakhash), wantpakhashhex,
		sizeof(wantpakhashhex)-1) != PAKHASHLEN)
		sysfatal("failed to decode AuthPAK hash vector");
	dp9ik_passtokey(&k, pw);
	authpak_hash(&k, "testuser");
	if(memcmp(k.pakhash, wantpakhash, sizeof(wantpakhash)) != 0)
		sysfatal("AuthPAK hash mismatch");
	print("dp9ik: authpak hash ok\n");

	memset(&ck, 0, sizeof(ck));
	memset(&sk, 0, sizeof(sk));
	dp9ik_passtokey(&ck, pw);
	dp9ik_passtokey(&sk, pw);
	authpak_hash(&ck, "testuser");
	authpak_hash(&sk, "testuser");

	authpak_new(&cp, &ck, cy, 1);
	authpak_new(&sp, &sk, sy, 0);
	if(authpak_finish(&cp, &ck, sy) < 0)
		sysfatal("client AuthPAK finish failed");
	if(authpak_finish(&sp, &sk, cy) < 0)
		sysfatal("server AuthPAK finish failed");
	if(allzero(ck.pakkey, sizeof(ck.pakkey)))
		sysfatal("client pakkey is all zeros");
	if(memcmp(ck.pakkey, sk.pakkey, sizeof(ck.pakkey)) != 0)
		sysfatal("AuthPAK shared key mismatch");
	print("dp9ik: authpak exchange ok\n");

	memset(&t1, 0, sizeof(t1));
	memset(&t2, 0, sizeof(t2));
	t1.num = AuthTs;
	t1.form = 1;
	memmove(t1.chal, "12345678", CHALLEN);
	strcpy(t1.cuid, "testuser");
	strcpy(t1.suid, "authdom");
	memmove(t1.key, ck.pakkey, NONCELEN);
	flen = dp9ik_convT2M(&t1, tbuf, sizeof(tbuf), &ck);
	if(flen != MAXTICKETLEN)
		sysfatal("form1 ticket length mismatch: %d", flen);
	if(dp9ik_convM2T(tbuf, flen, &t2, &ck) != MAXTICKETLEN)
		sysfatal("form1 ticket parse failed");
	if(t2.form != 1)
		sysfatal("form1 ticket form mismatch");
	if(t2.num != t1.num || memcmp(t2.chal, t1.chal, CHALLEN) != 0
	|| strcmp(t2.cuid, t1.cuid) != 0 || strcmp(t2.suid, t1.suid) != 0
	|| memcmp(t2.key, t1.key, NONCELEN) != 0)
		sysfatal("form1 ticket roundtrip mismatch");
	print("dp9ik: ticket ok\n");

	print("dp9ik authsrv api ok\n");
	exits(nil);
}
