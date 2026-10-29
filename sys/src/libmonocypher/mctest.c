/*
 * Known-answer tests for libmonocypher under Harvey.
 *
 * Covers the primitives Harvey's libsec does not provide: BLAKE2b,
 * Poly1305, X25519, EdDSA (BLAKE2b) and XChaCha20-Poly1305 AEAD.
 * Vectors are from RFC 7693 (BLAKE2), RFC 8439 (Poly1305) and
 * RFC 7748 (X25519); the signature and AEAD cases are round trips.
 */
#include <u.h>
#include <libc.h>
#include <monocypher.h>

static int
hexval(int c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int
unhex(uchar *out, char *s, int n)
{
	int i, hi, lo;

	for(i = 0; i < n; i++){
		hi = hexval(s[2*i]);
		lo = hexval(s[2*i+1]);
		if(hi < 0 || lo < 0)
			return -1;
		out[i] = hi<<4 | lo;
	}
	return 0;
}

static int
hexeq(uchar *got, char *want, int n)
{
	uchar w[64];

	if(n > sizeof w || unhex(w, want, n) < 0)
		return 0;
	return memcmp(got, w, n) == 0;
}

static int
test_blake2b(void)
{
	uchar h[64];
	static char abc[] = "abc";
	static char want[] =
		"ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
		"7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923";

	crypto_blake2b(h, 64, (uchar*)abc, 3);
	if(!hexeq(h, want, 64)){
		print("MONOCYPHER FAIL blake2b-512\n");
		return -1;
	}
	print("MONOCYPHER blake2b-512 PASS\n");
	return 0;
}

static int
test_poly1305(void)
{
	uchar key[32], mac[16];
	static char msg[] = "Cryptographic Forum Research Group";
	static char k[] = "85d6be7857556d337f4452fe42d506a8"
		"0103808afb0db2fd4abff6af4149f51b";
	static char want[] = "a8061dc1305136c6c22b8baf0c0127a9";

	if(unhex(key, k, 32) < 0)
		return -1;
	crypto_poly1305(mac, (uchar*)msg, sizeof msg - 1, key);
	if(!hexeq(mac, want, 16)){
		print("MONOCYPHER FAIL poly1305\n");
		return -1;
	}
	print("MONOCYPHER poly1305 PASS\n");
	return 0;
}

static int
test_x25519(void)
{
	uchar scalar[32], point[32], out[32];
	static char s[] = "a546e36bf0527c9d3b16154b82465edd"
		"62144c0ac1fc5a18506a2244ba449ac4";
	static char u[] = "e6db6867583030db3594c1a424b15f7c"
		"726624ec26b3353b10a903a6d0ab1c4c";
	static char want[] = "c3da55379de9c6908e94ea4df28d084f"
		"32eccf03491c71f754b4075577a28552";

	if(unhex(scalar, s, 32) < 0 || unhex(point, u, 32) < 0)
		return -1;
	crypto_x25519(out, scalar, point);
	if(!hexeq(out, want, 32)){
		print("MONOCYPHER FAIL x25519\n");
		return -1;
	}
	print("MONOCYPHER x25519 PASS\n");
	return 0;
}

static int
test_eddsa(void)
{
	uchar seed[32], sk[64], pk[32], sig[64];
	static char msg[] = "harvey monocypher";

	memset(seed, 0x42, sizeof seed);
	crypto_eddsa_key_pair(sk, pk, seed);
	crypto_eddsa_sign(sig, sk, (uchar*)msg, sizeof msg - 1);
	if(crypto_eddsa_check(sig, pk, (uchar*)msg, sizeof msg - 1) != 0){
		print("MONOCYPHER FAIL eddsa verify\n");
		return -1;
	}
	msg[0] ^= 1;
	if(crypto_eddsa_check(sig, pk, (uchar*)msg, sizeof msg - 1) == 0){
		print("MONOCYPHER FAIL eddsa tamper\n");
		return -1;
	}
	print("MONOCYPHER eddsa PASS\n");
	return 0;
}

static int
test_aead(void)
{
	uchar key[32], nonce[24], ad[16], pt[47], ct[47], out[47], mac[16];
	int i;

	for(i = 0; i < sizeof key; i++)
		key[i] = i;
	for(i = 0; i < sizeof nonce; i++)
		nonce[i] = 0xa0 + i;
	for(i = 0; i < sizeof ad; i++)
		ad[i] = 0x10 + i;
	for(i = 0; i < sizeof pt; i++)
		pt[i] = 0x20 + i;
	crypto_aead_lock(ct, mac, key, nonce, ad, sizeof ad, pt, sizeof pt);
	if(crypto_aead_unlock(out, mac, key, nonce, ad, sizeof ad, ct, sizeof ct) != 0
	|| memcmp(out, pt, sizeof pt) != 0){
		print("MONOCYPHER FAIL aead roundtrip\n");
		return -1;
	}
	ct[0] ^= 1;
	if(crypto_aead_unlock(out, mac, key, nonce, ad, sizeof ad, ct, sizeof ct) == 0){
		print("MONOCYPHER FAIL aead tamper\n");
		return -1;
	}
	print("MONOCYPHER aead PASS\n");
	return 0;
}

void
main(void)
{
	if(test_blake2b() < 0 || test_poly1305() < 0 || test_x25519() < 0
	|| test_eddsa() < 0 || test_aead() < 0)
		exits("fail");
	print("MONOCYPHER PASS\n");
	exits(nil);
}
