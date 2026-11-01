/*
 * SHA-256, HMAC-SHA256 and HKDF-SHA256 on top of the OP-TEE crypto API.
 * The AuthPAK code needs only these three primitives.
 */
#include <tee_internal_api.h>
#include <u.h>
#include <libc.h>
#include <libsec.h>

static TEE_Result
digest(TEE_OperationHandle *op)
{
	return TEE_AllocateOperation(op, TEE_ALG_SHA256, TEE_MODE_DIGEST, 0);
}

void
ta_sha256(const u8 *data, u32 len, u8 out[32])
{
	TEE_OperationHandle op;
	u32 n = 32;

	if(digest(&op) != TEE_SUCCESS)
		TEE_Panic(0x2001);
	TEE_DigestDoFinal(op, data, len, out, &n);
	TEE_FreeOperation(op);
}

void
ta_hmac_sha256(const u8 *key, u32 klen, const u8 *data, u32 dlen,
	       u8 out[32])
{
	TEE_OperationHandle op;
	u8 k[64], ipad[64], opad[64], inner[32];
	u32 n;

	memset(k, 0, sizeof(k));
	if(klen > sizeof(k)){
		ta_sha256(key, klen, k);
		klen = 32;
	}else
		memmove(k, key, klen);

	for(n = 0; n < 64; n++){
		ipad[n] = k[n] ^ 0x36;
		opad[n] = k[n] ^ 0x5c;
	}

	if(digest(&op) != TEE_SUCCESS)
		TEE_Panic(0x2002);
	TEE_DigestUpdate(op, ipad, sizeof(ipad));
	TEE_DigestUpdate(op, data, dlen);
	n = 32;
	TEE_DigestDoFinal(op, nil, 0, inner, &n);
	TEE_FreeOperation(op);

	if(digest(&op) != TEE_SUCCESS)
		TEE_Panic(0x2003);
	TEE_DigestUpdate(op, opad, sizeof(opad));
	TEE_DigestUpdate(op, inner, sizeof(inner));
	n = 32;
	TEE_DigestDoFinal(op, nil, 0, out, &n);
	TEE_FreeOperation(op);

	TEE_MemFill(k, 0, sizeof(k));
	TEE_MemFill(ipad, 0, sizeof(ipad));
	TEE_MemFill(opad, 0, sizeof(opad));
	TEE_MemFill(inner, 0, sizeof(inner));
}

void
ta_hkdf_sha256(const u8 *salt, u32 nsalt, const u8 *info, u32 ninfo,
	       const u8 *ikm, u32 nikm, u8 *out, u32 dlen)
{
	u8 zeros[32], prk[32], t[32], buf[32 + 64 + 1];
	u32 i, n, tlen;

	if(nsalt == 0){
		memset(zeros, 0, sizeof(zeros));
		salt = zeros;
		nsalt = sizeof(zeros);
	}
	ta_hmac_sha256(salt, nsalt, ikm, nikm, prk);

	tlen = 0;
	for(i = 1; dlen > 0; i++){
		n = 0;
		if(tlen > 0){
			memmove(buf + n, t, tlen);
			n += tlen;
		}
		if(ninfo > 0){
			memmove(buf + n, info, ninfo);
			n += ninfo;
		}
		buf[n++] = i;
		ta_hmac_sha256(prk, sizeof(prk), buf, n, t);
		tlen = sizeof(t);
		n = dlen < tlen ? dlen : tlen;
		memmove(out, t, n);
		out += n;
		dlen -= n;
		if(i == 255 && dlen > 0)
			TEE_Panic(0x2004);
	}
	TEE_MemFill(zeros, 0, sizeof(zeros));
	TEE_MemFill(prk, 0, sizeof(prk));
	TEE_MemFill(t, 0, sizeof(t));
	TEE_MemFill(buf, 0, sizeof(buf));
}
