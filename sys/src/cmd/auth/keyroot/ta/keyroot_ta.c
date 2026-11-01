/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * keyroot trusted application.
 *
 * The oracle model: private keys are generated and held inside secure
 * mode. The normal world sees only public keys and opaque sealed blobs.
 *
 * Commands:
 *   PING/INFO/DERIVE       bring-up and key derivation
 *   MLDSA_KEYGEN           generate an ML-DSA-44 identity
 *   MLDSA_PUBLIC           extract the public key from a sealed blob
 *   MLDSA_SIGN             sign a message with a sealed blob
 *   MLDSA_VERIFY           verify a signature with a public key
 *   MLDSA_SELFTEST         keygen + seal + sign + verify self-test
 */
#include <tee_internal_api.h>
#include <pta_system.h>
#include <stdio.h>

#include "api.h"
#include "des/des.h"
#include "dp9ik/dp9ik.h"
#include "dp9ik/libsec.h"
#include "pqkem/api.h"

typedef uint8_t u8;
typedef uint32_t u32;

#define TA_KEYROOT_CMD_PING		0
#define TA_KEYROOT_CMD_INFO		1
#define TA_KEYROOT_CMD_DERIVE		2
#define TA_KEYROOT_CMD_MLDSA_KEYGEN	3
#define TA_KEYROOT_CMD_MLDSA_PUBLIC	4
#define TA_KEYROOT_CMD_MLDSA_SIGN	5
#define TA_KEYROOT_CMD_MLDSA_VERIFY	6
#define TA_KEYROOT_CMD_MLDSA_SELFTEST	7
#define TA_KEYROOT_CMD_P9_IMPORT	8
#define TA_KEYROOT_CMD_P9_TICKET	9
#define TA_KEYROOT_CMD_P9_AUTH		10
#define TA_KEYROOT_CMD_P9_VERIFY	11
#define TA_KEYROOT_CMD_P9_SELFTEST	12
#define TA_KEYROOT_CMD_DP9IK_IMPORT	13
#define TA_KEYROOT_CMD_DP9IK_SELFTEST	14
#define TA_KEYROOT_CMD_DP9IK_INIT		15
#define TA_KEYROOT_CMD_DP9IK_FINISH	16
#define TA_KEYROOT_CMD_DP9IK_REPLY		17
#define TA_KEYROOT_CMD_MLKEM_KEYGEN	18
#define TA_KEYROOT_CMD_MLKEM_ENCAPS	19
#define TA_KEYROOT_CMD_MLKEM_DECAPS	20
#define TA_KEYROOT_CMD_MLKEM_SELFTEST	21
#define TA_KEYROOT_CMD_USERKEY_KEYGEN	22
#define TA_KEYROOT_CMD_USERKEY_PUBLIC	23
#define TA_KEYROOT_CMD_USERKEY_SELFTEST	24

#define DERIVE_DOMAIN		"keyroot/oracle/v1/"

#define MLDSA_PKLEN \
	PQCLEAN_MLDSA44_CLEAN_CRYPTO_PUBLICKEYBYTES
#define MLDSA_SKLEN \
	PQCLEAN_MLDSA44_CLEAN_CRYPTO_SECRETKEYBYTES
#define MLDSA_SIGLEN \
	PQCLEAN_MLDSA44_CLEAN_CRYPTO_BYTES

#define MLKEM_PKLEN \
	PQCLEAN_MLKEM768_CLEAN_CRYPTO_PUBLICKEYBYTES
#define MLKEM_SKLEN \
	PQCLEAN_MLKEM768_CLEAN_CRYPTO_SECRETKEYBYTES
#define MLKEM_CTLEN \
	PQCLEAN_MLKEM768_CLEAN_CRYPTO_CIPHERTEXTBYTES
#define MLKEM_SSLEN \
	PQCLEAN_MLKEM768_CLEAN_CRYPTO_BYTES

enum {
	BlobHdrlen	= 52,
	BlobVersion	= 1,
	BlobAlgMldsa44	= 1,
	BlobAlgP9des	= 2,
	BlobAlgDp9ik	= 3,
	BlobAlgMlkem768	= 4,
	BlobUsageIdentity = 1,
	BlobUsageP9Host	= 2,
	BlobUsageP9Auth	= 3,
	BlobUsageDp9ik	= 4,
	BlobUsageMlkem	= 5,

	UserKeyMldsa44	= 1,
	UserKeyMlkem768	= 2,
	UserKeyHdrlen	= 32,
	UserKeyMaxLabel	= 64,
	UserKeyMax	= 4096,
	BlobPolicyNone	= 0,
	BlobMaxLabel	= 64,
	BlobMaxPub	= MLDSA_PKLEN,
	BlobMaxPriv	= MLDSA_SKLEN,
	BlobMax		= BlobHdrlen + BlobMaxLabel + BlobMaxPub + BlobMaxPriv,
};

/* Plan 9 p9sk1 wire sizes. */
enum {
	P9Challen	= 8,
	P9Anamelen	= 28,
	P9Domlen	= 48,
	P9Deskeylen	= 7,
	P9Ticketlen	= 1 + P9Challen + 2*P9Anamelen + P9Deskeylen,
	P9Authentlen	= 1 + P9Challen + 4,
	P9Tickreqlen	= 1 + P9Anamelen + P9Domlen + P9Challen +
			  2*P9Anamelen,
};

static void
put32be(u8 *p, u32 v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static u32
get32be(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
	       ((u32)p[2] << 8) | p[3];
}

static TEE_Result
derive_label(const u8 *label, u32 nlabel, u8 key[32])
{
	static const char domain[] = DERIVE_DOMAIN;
	u8 extra[sizeof(domain) - 1 + BlobMaxLabel];
	TEE_TASessionHandle sess = TEE_HANDLE_NULL;
	TEE_UUID uuid = PTA_SYSTEM_UUID;
	TEE_Param pp[4];
	uint32_t ptypes;
	uint32_t origin;
	TEE_Result res;

	if(nlabel == 0 || nlabel > BlobMaxLabel)
		return TEE_ERROR_BAD_PARAMETERS;

	TEE_MemMove(extra, domain, sizeof(domain) - 1);
	TEE_MemMove(extra + sizeof(domain) - 1, label, nlabel);
	TEE_MemFill(pp, 0, sizeof(pp));
	ptypes = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE);
	pp[0].memref.buffer = extra;
	pp[0].memref.size = sizeof(domain) - 1 + nlabel;
	pp[1].memref.buffer = key;
	pp[1].memref.size = 32;

	res = TEE_OpenTASession(&uuid, 0,
				TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
						TEE_PARAM_TYPE_NONE,
						TEE_PARAM_TYPE_NONE,
						TEE_PARAM_TYPE_NONE),
				NULL, &sess, &origin);
	if(res != TEE_SUCCESS)
		goto out;
	res = TEE_InvokeTACommand(sess, 0, PTA_SYSTEM_DERIVE_TA_UNIQUE_KEY,
				  ptypes, pp, &origin);
out:
	if(sess != TEE_HANDLE_NULL)
		TEE_CloseTASession(sess);
	TEE_MemFill(extra, 0, sizeof(extra));
	return res;
}

static TEE_Result
aes_gcm(TEE_OperationMode mode, const u8 *key, const u8 *nonce,
	const u8 *aad, u32 aadlen, const u8 *in, u32 inlen,
	u8 *out, u8 *tag)
{
	TEE_OperationHandle op = TEE_HANDLE_NULL;
	TEE_ObjectHandle obj = TEE_HANDLE_NULL;
	TEE_Attribute attr;
	TEE_Result res;
	u32 outlen = inlen;
	u32 finlen = 0;
	u32 taglen = 16;
	u8 tagbuf[16];

	res = TEE_AllocateTransientObject(TEE_TYPE_AES, 256, &obj);
	if(res != TEE_SUCCESS)
		goto out;
	TEE_InitRefAttribute(&attr, TEE_ATTR_SECRET_VALUE, (void *)key, 32);
	res = TEE_PopulateTransientObject(obj, &attr, 1);
	if(res != TEE_SUCCESS)
		goto out;
	res = TEE_AllocateOperation(&op, TEE_ALG_AES_GCM, mode, 256);
	if(res != TEE_SUCCESS)
		goto out;
	res = TEE_SetOperationKey(op, obj);
	if(res != TEE_SUCCESS)
		goto out;
	res = TEE_AEInit(op, (void *)nonce, 12, 128, aadlen, inlen);
	if(res != TEE_SUCCESS)
		goto out;
	TEE_AEUpdateAAD(op, (void *)aad, aadlen);

	if(mode == TEE_MODE_ENCRYPT){
		outlen = inlen;
		res = TEE_AEEncryptFinal(op, (void *)in, inlen, out,
					 &outlen, tag, &taglen);
	} else {
		outlen = inlen;
		TEE_MemMove(tagbuf, tag, sizeof(tagbuf));
		res = TEE_AEDecryptFinal(op, (void *)in, inlen, out,
					 &outlen, tagbuf, sizeof(tagbuf));
	}
	finlen = outlen;
out:
	TEE_MemFill(tagbuf, 0, sizeof(tagbuf));
	if(op != TEE_HANDLE_NULL)
		TEE_FreeOperation(op);
	if(obj != TEE_HANDLE_NULL)
		TEE_FreeTransientObject(obj);
	return res;
}

static TEE_Result
blob_seal(u8 alg, u8 usage, const u8 *label, u32 nlabel, const u8 *pub,
	  u32 publen, const u8 *priv, u32 privlen, u8 *blob, u32 *nblob)
{
	u8 key[32];
	u8 aad[24 + BlobMaxLabel + BlobMaxPub];
	u32 total;
	TEE_Result res;

	if(nlabel == 0 || nlabel > BlobMaxLabel || publen > BlobMaxPub ||
	   privlen == 0 || privlen > BlobMaxPriv)
		return TEE_ERROR_BAD_PARAMETERS;
	total = BlobHdrlen + nlabel + publen + privlen;
	if(nblob == NULL || *nblob < total)
		return TEE_ERROR_SHORT_BUFFER;

	TEE_MemMove(blob, "KROOT2", 6);
	blob[6] = BlobVersion;
	blob[7] = alg;
	blob[8] = usage;
	blob[9] = BlobPolicyNone;
	blob[10] = 0;
	blob[11] = 0;
	put32be(blob + 12, nlabel);
	put32be(blob + 16, publen);
	put32be(blob + 20, privlen);
	TEE_GenerateRandom(blob + 24, 12);
	TEE_MemMove(blob + BlobHdrlen, label, nlabel);
	TEE_MemMove(blob + BlobHdrlen + nlabel, pub, publen);

	res = derive_label(label, nlabel, key);
	if(res != TEE_SUCCESS)
		goto out;

	TEE_MemMove(aad, blob, 24);
	TEE_MemMove(aad + 24, label, nlabel);
	TEE_MemMove(aad + 24 + nlabel, pub, publen);
	res = aes_gcm(TEE_MODE_ENCRYPT, key, blob + 24, aad,
		      24 + nlabel + publen, priv, privlen,
		      blob + BlobHdrlen + nlabel + publen, blob + 36);
	if(res == TEE_SUCCESS)
		*nblob = total;
out:
	TEE_MemFill(key, 0, sizeof(key));
	TEE_MemFill(aad, 0, sizeof(aad));
	return res;
}

static TEE_Result
blob_parse(const u8 *blob, u32 nblob, u8 *alg, u8 *usage,
	   const u8 **label, u32 *nlabel, const u8 **pub, u32 *npub,
	   const u8 **cipher, u32 *ncipher)
{
	u32 ll, pl, cl;

	if(nblob < BlobHdrlen)
		return TEE_ERROR_BAD_FORMAT;
	if(TEE_MemCompare(blob, "KROOT2", 6) != 0)
		return TEE_ERROR_BAD_FORMAT;
	if(blob[6] != BlobVersion)
		return TEE_ERROR_BAD_FORMAT;
	ll = get32be(blob + 12);
	pl = get32be(blob + 16);
	cl = get32be(blob + 20);
	if(ll == 0 || ll > BlobMaxLabel || pl > BlobMaxPub ||
	   cl == 0 || cl > BlobMaxPriv)
		return TEE_ERROR_BAD_FORMAT;
	if(nblob != BlobHdrlen + ll + pl + cl)
		return TEE_ERROR_BAD_FORMAT;
	if(alg != NULL)
		*alg = blob[7];
	if(usage != NULL)
		*usage = blob[8];
	*label = blob + BlobHdrlen;
	*nlabel = ll;
	*pub = blob + BlobHdrlen + ll;
	*npub = pl;
	*cipher = blob + BlobHdrlen + ll + pl;
	*ncipher = cl;
	return TEE_SUCCESS;
}

static TEE_Result
blob_open(const u8 *blob, u32 nblob, u8 *alg, u8 *usage,
	  const u8 **label, u32 *nlabel, const u8 **pub, u32 *npub,
	  u8 *priv, u32 *npriv)
{
	const u8 *cipher;
	u8 key[32];
	u8 aad[24 + BlobMaxLabel + BlobMaxPub];
	u32 ncipher;
	TEE_Result res;

	res = blob_parse(blob, nblob, alg, usage, label, nlabel, pub, npub,
			 &cipher, &ncipher);
	if(res != TEE_SUCCESS)
		return res;
	if(*npriv < ncipher)
		return TEE_ERROR_SHORT_BUFFER;

	res = derive_label(*label, *nlabel, key);
	if(res != TEE_SUCCESS)
		goto out;

	TEE_MemMove(aad, blob, 24);
	TEE_MemMove(aad + 24, *label, *nlabel);
	TEE_MemMove(aad + 24 + *nlabel, *pub, *npub);
	res = aes_gcm(TEE_MODE_DECRYPT, key, blob + 24, aad,
		      24 + *nlabel + *npub, cipher, ncipher,
		      priv, (u8 *)(blob + 36));
	if(res == TEE_SUCCESS)
		*npriv = ncipher;
out:
	TEE_MemFill(key, 0, sizeof(key));
	TEE_MemFill(aad, 0, sizeof(aad));
	return res;
}

static TEE_Result
cmd_ping(u32 pt, TEE_Param p[4])
{
	static const char msg[] = "keyroot-ta-ping";
	u32 n = sizeof(msg) - 1;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	if(p[0].memref.size < n)
		return TEE_ERROR_SHORT_BUFFER;
	TEE_MemMove(p[0].memref.buffer, msg, n);
	p[0].memref.size = n;
	return TEE_SUCCESS;
}

static TEE_Result
cmd_info(u32 pt, TEE_Param p[4])
{
	char buf[160];
	int n;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	n = snprintf(buf, sizeof(buf),
		     "keyroot-ta v3 mldsa=%s pk=%u sk=%u sig=%u p9ticket=%u p9auth=%u\n",
		     PQCLEAN_MLDSA44_CLEAN_CRYPTO_ALGNAME,
		     (unsigned)MLDSA_PKLEN, (unsigned)MLDSA_SKLEN,
		     (unsigned)MLDSA_SIGLEN, (unsigned)P9Ticketlen,
		     (unsigned)P9Authentlen);
	if(n < 0)
		return TEE_ERROR_GENERIC;
	if(p[0].memref.size < (u32)n)
		return TEE_ERROR_SHORT_BUFFER;
	TEE_MemMove(p[0].memref.buffer, buf, n);
	p[0].memref.size = n;
	return TEE_SUCCESS;
}

static TEE_Result
cmd_derive(u32 pt, TEE_Param p[4])
{
	u8 key[32];
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	if(p[0].memref.size == 0 || p[0].memref.size > BlobMaxLabel)
		return TEE_ERROR_BAD_PARAMETERS;
	if(p[1].memref.size < sizeof(key))
		return TEE_ERROR_SHORT_BUFFER;

	res = derive_label(p[0].memref.buffer, p[0].memref.size, key);
	if(res == TEE_SUCCESS){
		TEE_MemMove(p[1].memref.buffer, key, sizeof(key));
		p[1].memref.size = sizeof(key);
	}
	TEE_MemFill(key, 0, sizeof(key));
	return res;
}

static TEE_Result
cmd_mldsa_keygen(u32 pt, TEE_Param p[4])
{
	u8 *pk = NULL;
	u8 *sk = NULL;
	u8 *blob = NULL;
	u32 nblob = BlobMax;
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;

	pk = TEE_Malloc(MLDSA_PKLEN, 0);
	sk = TEE_Malloc(MLDSA_SKLEN, 0);
	blob = TEE_Malloc(BlobMax, 0);
	if(pk == NULL || sk == NULL || blob == NULL){
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto out;
	}
	if(PQCLEAN_MLDSA44_CLEAN_crypto_sign_keypair(pk, sk) != 0){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	res = blob_seal(BlobAlgMldsa44, BlobUsageIdentity,
			p[0].memref.buffer, p[0].memref.size,
			pk, MLDSA_PKLEN, sk, MLDSA_SKLEN, blob, &nblob);
	if(res != TEE_SUCCESS)
		goto out;
	if(p[1].memref.size < MLDSA_PKLEN ||
	   p[2].memref.size < nblob){
		res = TEE_ERROR_SHORT_BUFFER;
		goto out;
	}
	TEE_MemMove(p[1].memref.buffer, pk, MLDSA_PKLEN);
	p[1].memref.size = MLDSA_PKLEN;
	TEE_MemMove(p[2].memref.buffer, blob, nblob);
	p[2].memref.size = nblob;
	res = TEE_SUCCESS;
out:
	if(pk != NULL){
		TEE_MemFill(pk, 0, MLDSA_PKLEN);
		TEE_Free(pk);
	}
	if(sk != NULL){
		TEE_MemFill(sk, 0, MLDSA_SKLEN);
		TEE_Free(sk);
	}
	if(blob != NULL){
		TEE_MemFill(blob, 0, BlobMax);
		TEE_Free(blob);
	}
	return res;
}

static TEE_Result
cmd_mldsa_public(u32 pt, TEE_Param p[4])
{
	const u8 *label, *pub, *cipher;
	u32 nlabel, npub, ncipher;
	u8 alg, usage;
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	res = blob_parse(p[0].memref.buffer, p[0].memref.size,
			 &alg, &usage, &label, &nlabel, &pub, &npub,
			 &cipher, &ncipher);
	if(res != TEE_SUCCESS)
		return res;
	if(alg != BlobAlgMldsa44 || usage != BlobUsageIdentity)
		return TEE_ERROR_BAD_FORMAT;
	if(p[1].memref.size < npub)
		return TEE_ERROR_SHORT_BUFFER;
	TEE_MemMove(p[1].memref.buffer, pub, npub);
	p[1].memref.size = npub;
	return TEE_SUCCESS;
}

static TEE_Result
cmd_mldsa_sign(u32 pt, TEE_Param p[4])
{
	const u8 *label, *pub, *cipher;
	u32 nlabel, npub, ncipher;
	u8 alg, usage;
	u8 *sk = NULL;
	u32 sklen = MLDSA_SKLEN;
	size_t siglen = MLDSA_SIGLEN;
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	sk = TEE_Malloc(MLDSA_SKLEN, 0);
	if(sk == NULL)
		return TEE_ERROR_OUT_OF_MEMORY;
	res = blob_open(p[0].memref.buffer, p[0].memref.size,
			&alg, &usage, &label, &nlabel, &pub, &npub,
			sk, &sklen);
	if(res != TEE_SUCCESS)
		goto out;
	if(alg != BlobAlgMldsa44 || usage != BlobUsageIdentity){
		res = TEE_ERROR_BAD_FORMAT;
		goto out;
	}
	if(p[2].memref.size < MLDSA_SIGLEN){
		res = TEE_ERROR_SHORT_BUFFER;
		goto out;
	}
	if(PQCLEAN_MLDSA44_CLEAN_crypto_sign_signature(
		   p[2].memref.buffer, &siglen, p[1].memref.buffer,
		   p[1].memref.size, sk) != 0){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	p[2].memref.size = (u32)siglen;
	res = TEE_SUCCESS;
out:
	if(sk != NULL){
		TEE_MemFill(sk, 0, MLDSA_SKLEN);
		TEE_Free(sk);
	}
	return res;
}

static TEE_Result
cmd_mldsa_verify(u32 pt, TEE_Param p[4])
{
	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	if(p[0].memref.size != MLDSA_PKLEN ||
	   p[2].memref.size != MLDSA_SIGLEN)
		return TEE_ERROR_BAD_PARAMETERS;
	if(PQCLEAN_MLDSA44_CLEAN_crypto_sign_verify(
		   p[2].memref.buffer, p[2].memref.size,
		   p[1].memref.buffer, p[1].memref.size,
		   p[0].memref.buffer) != 0)
		return TEE_ERROR_SIGNATURE_INVALID;
	return TEE_SUCCESS;
}

static TEE_Result
cmd_mldsa_selftest(u32 pt, TEE_Param p[4])
{
	static const char label[] = "selftest/identity";
	static const char msg[] = "keyroot mldsa selftest";
	const u8 *blabel, *bpub, *cipher;
	u32 nblabel, nbpub, ncipher;
	u8 alg, usage;
	u8 *pk = NULL;
	u8 *sk = NULL;
	u8 *sk2 = NULL;
	u8 *blob = NULL;
	u8 *sig = NULL;
	u32 nblob = BlobMax;
	u32 sklen = MLDSA_SKLEN;
	size_t siglen = MLDSA_SIGLEN;
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;

	pk = TEE_Malloc(MLDSA_PKLEN, 0);
	sk = TEE_Malloc(MLDSA_SKLEN, 0);
	sk2 = TEE_Malloc(MLDSA_SKLEN, 0);
	blob = TEE_Malloc(BlobMax, 0);
	sig = TEE_Malloc(MLDSA_SIGLEN, 0);
	if(pk == NULL || sk == NULL || sk2 == NULL || blob == NULL ||
	   sig == NULL){
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto out;
	}
	if(PQCLEAN_MLDSA44_CLEAN_crypto_sign_keypair(pk, sk) != 0){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	res = blob_seal(BlobAlgMldsa44, BlobUsageIdentity,
			(const u8 *)label, sizeof(label) - 1,
			pk, MLDSA_PKLEN, sk, MLDSA_SKLEN, blob, &nblob);
	if(res != TEE_SUCCESS)
		goto out;
	res = blob_open(blob, nblob, &alg, &usage,
			&blabel, &nblabel, &bpub, &nbpub, sk2, &sklen);
	if(res != TEE_SUCCESS)
		goto out;
	if(alg != BlobAlgMldsa44 || usage != BlobUsageIdentity){
		res = TEE_ERROR_BAD_FORMAT;
		goto out;
	}
	if(nbpub != MLDSA_PKLEN || sklen != MLDSA_SKLEN){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	if(PQCLEAN_MLDSA44_CLEAN_crypto_sign_signature(
		   sig, &siglen, (const u8 *)msg, sizeof(msg) - 1,
		   sk2) != 0){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	if(PQCLEAN_MLDSA44_CLEAN_crypto_sign_verify(
		   sig, siglen, (const u8 *)msg, sizeof(msg) - 1,
		   bpub) != 0){
		res = TEE_ERROR_SIGNATURE_INVALID;
		goto out;
	}
	res = TEE_SUCCESS;
out:
	if(pk != NULL){
		TEE_MemFill(pk, 0, MLDSA_PKLEN);
		TEE_Free(pk);
	}
	if(sk != NULL){
		TEE_MemFill(sk, 0, MLDSA_SKLEN);
		TEE_Free(sk);
	}
	if(sk2 != NULL){
		TEE_MemFill(sk2, 0, MLDSA_SKLEN);
		TEE_Free(sk2);
	}
	if(blob != NULL){
		TEE_MemFill(blob, 0, BlobMax);
		TEE_Free(blob);
	}
	if(sig != NULL){
		TEE_MemFill(sig, 0, MLDSA_SIGLEN);
		TEE_Free(sig);
	}
	return res;
}

/*
 * p9sk1 oracle.
 *
 * The trusted application holds the DES keys.  The normal world gets
 * encrypted tickets and authenticators, never the session key.
 *
 * Wire offsets, from sys/include/authsrv.h:
 *   Ticketreq: type 0, authid 1, authdom 29, chal 77, hostid 85, uid 113
 *   Ticket:    num 0, chal 1, cuid 9, suid 37, key 65
 *   Auth:      num 0, chal 1, id 9 (little endian)
 */
enum {
	P9TrType	= 0,
	P9TrAuthdom	= 29,
	P9TrChal	= 77,
	P9TrHostid	= 85,
	P9TrUid		= 113,

	P9TNum		= 0,
	P9TChal		= 1,
	P9TCuid		= 9,
	P9TSuid		= 37,
	P9TKey		= 65,

	P9ANum		= 0,
	P9AChal		= 1,
	P9AId		= 9,

	P9AuthTreq	= 1,
	P9AuthTs	= 64,
	P9AuthTc	= 65,
	P9AuthAs	= 66,
	P9AuthAc	= 67,
};

static TEE_Result
p9_open_key(const u8 *blob, u32 nblob, u8 wantusage, u8 key[P9Deskeylen])
{
	const u8 *label, *pub, *cipher;
	u32 nlabel, npub, ncipher;
	u8 alg, usage;
	u8 priv[P9Deskeylen];
	u32 npriv = sizeof(priv);
	TEE_Result res;

	res = blob_open(blob, nblob, &alg, &usage, &label, &nlabel,
			&pub, &npub, priv, &npriv);
	if(res != TEE_SUCCESS)
		return res;
	if(alg != BlobAlgP9des || usage != wantusage ||
	   npriv != P9Deskeylen)
		res = TEE_ERROR_BAD_FORMAT;
	else
		TEE_MemMove(key, priv, P9Deskeylen);
	TEE_MemFill(priv, 0, sizeof(priv));
	return res;
}

static TEE_Result
p9_make_tickets(const u8 *ablob, u32 anblob, const u8 *hblob,
		u32 hnblob, const u8 *tr, u8 *out, u32 outlen)
{
	u8 akey[P9Deskeylen], hkey[P9Deskeylen];
	u8 t[P9Ticketlen];
	TEE_Result res;

	if(outlen < 2*P9Ticketlen)
		return TEE_ERROR_SHORT_BUFFER;
	if(tr[P9TrType] != P9AuthTreq)
		return TEE_ERROR_BAD_FORMAT;
	res = p9_open_key(ablob, anblob, BlobUsageP9Auth, akey);
	if(res != TEE_SUCCESS)
		return res;
	res = p9_open_key(hblob, hnblob, BlobUsageP9Host, hkey);
	if(res != TEE_SUCCESS)
		goto out;

	TEE_MemFill(t, 0, sizeof(t));
	t[P9TNum] = P9AuthTc;
	TEE_MemMove(t + P9TChal, tr + P9TrChal, P9Challen);
	TEE_MemMove(t + P9TCuid, tr + P9TrUid, P9Anamelen);
	TEE_MemMove(t + P9TSuid, tr + P9TrUid, P9Anamelen);
	TEE_GenerateRandom(t + P9TKey, P9Deskeylen);

	/* Client ticket first, then server ticket, as _asgetticket returns. */
	TEE_MemMove(out, t, P9Ticketlen);
	encrypt(hkey, out, P9Ticketlen);
	t[P9TNum] = P9AuthTs;
	TEE_MemMove(out + P9Ticketlen, t, P9Ticketlen);
	encrypt(akey, out + P9Ticketlen, P9Ticketlen);
	res = TEE_SUCCESS;
out:
	TEE_MemFill(t, 0, sizeof(t));
	TEE_MemFill(akey, 0, sizeof(akey));
	TEE_MemFill(hkey, 0, sizeof(hkey));
	return res;
}

/* Decrypt a host ticket, checking only that it is a client ticket. */
static TEE_Result
p9_open_ticket_key(const u8 *hblob, u32 hnblob, const u8 *ticket,
		   u8 t[P9Ticketlen])
{
	u8 hkey[P9Deskeylen];
	TEE_Result res;

	TEE_MemMove(t, ticket, P9Ticketlen);
	res = p9_open_key(hblob, hnblob, BlobUsageP9Host, hkey);
	if(res == TEE_SUCCESS){
		decrypt(hkey, t, P9Ticketlen);
		if(t[P9TNum] != P9AuthTc)
			res = TEE_ERROR_BAD_FORMAT;
	}
	TEE_MemFill(hkey, 0, sizeof(hkey));
	return res;
}

/* Decrypt a host ticket and check that it belongs to this challenge. */
static TEE_Result
p9_open_ticket(const u8 *hblob, u32 hnblob, const u8 *ticket,
	       const u8 *chal, u8 t[P9Ticketlen])
{
	TEE_Result res;

	res = p9_open_ticket_key(hblob, hnblob, ticket, t);
	if(res == TEE_SUCCESS &&
	   TEE_MemCompare(t + P9TChal, chal, P9Challen) != 0)
		res = TEE_ERROR_BAD_FORMAT;
	return res;
}

static TEE_Result
p9_make_auth(const u8 *hblob, u32 hnblob, const u8 *ticket,
	     const u8 *chal, u8 *out, u32 outlen)
{
	u8 t[P9Ticketlen];
	u8 a[P9Authentlen];
	TEE_Result res;

	if(outlen < P9Authentlen)
		return TEE_ERROR_SHORT_BUFFER;
	res = p9_open_ticket(hblob, hnblob, ticket, chal, t);
	if(res != TEE_SUCCESS)
		goto out;
	TEE_MemFill(a, 0, sizeof(a));
	a[P9ANum] = P9AuthAc;
	TEE_MemMove(a + P9AChal, chal, P9Challen);
	encrypt(t + P9TKey, a, P9Authentlen);
	TEE_MemMove(out, a, P9Authentlen);
	res = TEE_SUCCESS;
out:
	TEE_MemFill(a, 0, sizeof(a));
	TEE_MemFill(t, 0, sizeof(t));
	return res;
}

/* Server side, for the self-test: decrypt AuthTs, reply with AuthAs. */
static TEE_Result
p9_server_reply(const u8 *ablob, u32 anblob, const u8 *ticket,
		const u8 *cchal, u8 *out, u32 outlen)
{
	u8 akey[P9Deskeylen];
	u8 t[P9Ticketlen];
	u8 a[P9Authentlen];
	TEE_Result res;

	if(outlen < P9Authentlen)
		return TEE_ERROR_SHORT_BUFFER;
	TEE_MemMove(t, ticket, P9Ticketlen);
	res = p9_open_key(ablob, anblob, BlobUsageP9Auth, akey);
	if(res != TEE_SUCCESS)
		goto out;
	decrypt(akey, t, P9Ticketlen);
	if(t[P9TNum] != P9AuthTs){
		res = TEE_ERROR_BAD_FORMAT;
		goto out;
	}
	TEE_MemFill(a, 0, sizeof(a));
	a[P9ANum] = P9AuthAs;
	TEE_MemMove(a + P9AChal, cchal, P9Challen);
	encrypt(t + P9TKey, a, P9Authentlen);
	TEE_MemMove(out, a, P9Authentlen);
	res = TEE_SUCCESS;
out:
	TEE_MemFill(a, 0, sizeof(a));
	TEE_MemFill(t, 0, sizeof(t));
	TEE_MemFill(akey, 0, sizeof(akey));
	return res;
}

static TEE_Result
p9_check_reply(const u8 *hblob, u32 hnblob, const u8 *ticket,
	       const u8 *cchal, const u8 *auth)
{
	u8 t[P9Ticketlen];
	u8 a[P9Authentlen];
	u32 id;
	TEE_Result res;

	res = p9_open_ticket_key(hblob, hnblob, ticket, t);
	if(res != TEE_SUCCESS)
		goto out;
	TEE_MemMove(a, auth, P9Authentlen);
	decrypt(t + P9TKey, a, P9Authentlen);
	id = (u32)a[P9AId] | ((u32)a[P9AId+1] << 8) |
	     ((u32)a[P9AId+2] << 16) | ((u32)a[P9AId+3] << 24);
	if(a[P9ANum] != P9AuthAs ||
	   TEE_MemCompare(a + P9AChal, cchal, P9Challen) != 0 || id != 0)
		res = TEE_ERROR_SIGNATURE_INVALID;
out:
	TEE_MemFill(a, 0, sizeof(a));
	TEE_MemFill(t, 0, sizeof(t));
	return res;
}

static TEE_Result
cmd_p9_import(u32 pt, TEE_Param p[4])
{
	u8 *blob;
	u8 usage;
	u32 nblob = BlobMax;
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT))
		return TEE_ERROR_BAD_PARAMETERS;
	if(p[0].memref.size == 0 || p[0].memref.size > BlobMaxLabel ||
	   p[1].memref.size != P9Deskeylen || p[2].memref.size != 1)
		return TEE_ERROR_BAD_PARAMETERS;
	usage = *(u8 *)p[2].memref.buffer;
	if(usage != BlobUsageP9Host && usage != BlobUsageP9Auth)
		return TEE_ERROR_BAD_PARAMETERS;
	blob = TEE_Malloc(BlobMax, 0);
	if(blob == NULL)
		return TEE_ERROR_OUT_OF_MEMORY;
	res = blob_seal(BlobAlgP9des, usage, p[0].memref.buffer,
			p[0].memref.size, NULL, 0, p[1].memref.buffer,
			P9Deskeylen, blob, &nblob);
	if(res == TEE_SUCCESS){
		if(p[3].memref.size < nblob)
			res = TEE_ERROR_SHORT_BUFFER;
		else{
			TEE_MemMove(p[3].memref.buffer, blob, nblob);
			p[3].memref.size = nblob;
		}
	}
	TEE_MemFill(blob, 0, BlobMax);
	TEE_Free(blob);
	return res;
}

static TEE_Result
cmd_p9_ticket(u32 pt, TEE_Param p[4])
{
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT))
		return TEE_ERROR_BAD_PARAMETERS;
	if(p[2].memref.size != P9Tickreqlen)
		return TEE_ERROR_BAD_PARAMETERS;
	res = p9_make_tickets(p[0].memref.buffer, p[0].memref.size,
			      p[1].memref.buffer, p[1].memref.size,
			      p[2].memref.buffer, p[3].memref.buffer,
			      p[3].memref.size);
	if(res == TEE_SUCCESS)
		p[3].memref.size = 2*P9Ticketlen;
	return res;
}

static TEE_Result
cmd_p9_auth(u32 pt, TEE_Param p[4])
{
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT))
		return TEE_ERROR_BAD_PARAMETERS;
	if(p[1].memref.size != P9Ticketlen ||
	   p[2].memref.size != P9Challen)
		return TEE_ERROR_BAD_PARAMETERS;
	res = p9_make_auth(p[0].memref.buffer, p[0].memref.size,
			   p[1].memref.buffer, p[2].memref.buffer,
			   p[3].memref.buffer, p[3].memref.size);
	if(res == TEE_SUCCESS)
		p[3].memref.size = P9Authentlen;
	return res;
}

static TEE_Result
cmd_p9_verify(u32 pt, TEE_Param p[4])
{
	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT))
		return TEE_ERROR_BAD_PARAMETERS;
	if(p[1].memref.size != P9Ticketlen ||
	   p[2].memref.size != P9Challen ||
	   p[3].memref.size != P9Authentlen)
		return TEE_ERROR_BAD_PARAMETERS;
	return p9_check_reply(p[0].memref.buffer, p[0].memref.size,
			      p[1].memref.buffer, p[2].memref.buffer,
			      p[3].memref.buffer);
}

static TEE_Result
cmd_p9_selftest(u32 pt, TEE_Param p[4])
{
	static const char hlabel[] = "selftest/p9host";
	static const char alabel[] = "selftest/p9auth";
	u8 hkey[P9Deskeylen], akey[P9Deskeylen];
	u8 hblob[BlobMax], ablob[BlobMax];
	u8 tr[P9Tickreqlen], tickets[2*P9Ticketlen];
	u8 auth[P9Authentlen], reply[P9Authentlen];
	u8 cchal[P9Challen];
	u32 nhblob = BlobMax, nablob = BlobMax;
	TEE_Result res;

	(void)p;
	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;

	TEE_GenerateRandom(hkey, sizeof(hkey));
	TEE_GenerateRandom(akey, sizeof(akey));
	TEE_GenerateRandom(cchal, sizeof(cchal));
	res = blob_seal(BlobAlgP9des, BlobUsageP9Host,
			(const u8 *)hlabel, sizeof(hlabel) - 1, NULL, 0,
			hkey, sizeof(hkey), hblob, &nhblob);
	if(res != TEE_SUCCESS)
		goto out;
	res = blob_seal(BlobAlgP9des, BlobUsageP9Auth,
			(const u8 *)alabel, sizeof(alabel) - 1, NULL, 0,
			akey, sizeof(akey), ablob, &nablob);
	if(res != TEE_SUCCESS)
		goto out;

	TEE_MemFill(tr, 0, sizeof(tr));
	tr[P9TrType] = P9AuthTreq;
	TEE_MemMove(tr + P9TrAuthdom, "selftest", 8);
	TEE_GenerateRandom(tr + P9TrChal, P9Challen);
	TEE_MemMove(tr + P9TrHostid, "host", 4);
	TEE_MemMove(tr + P9TrUid, "user", 4);

	res = p9_make_tickets(ablob, nablob, hblob, nhblob, tr,
			      tickets, sizeof(tickets));
	if(res != TEE_SUCCESS)
		goto out;
	res = p9_make_auth(hblob, nhblob, tickets, tr + P9TrChal,
			   auth, sizeof(auth));
	if(res != TEE_SUCCESS)
		goto out;
	res = p9_server_reply(ablob, nablob, tickets + P9Ticketlen,
			      cchal, reply, sizeof(reply));
	if(res != TEE_SUCCESS)
		goto out;
	res = p9_check_reply(hblob, nhblob, tickets, cchal, reply);
out:
	TEE_MemFill(hkey, 0, sizeof(hkey));
	TEE_MemFill(akey, 0, sizeof(akey));
	TEE_MemFill(hblob, 0, sizeof(hblob));
	TEE_MemFill(ablob, 0, sizeof(ablob));
	TEE_MemFill(tr, 0, sizeof(tr));
	TEE_MemFill(tickets, 0, sizeof(tickets));
	TEE_MemFill(auth, 0, sizeof(auth));
	TEE_MemFill(reply, 0, sizeof(reply));
	return res;
}


typedef struct Dp9ikState Dp9ikState;
struct Dp9ikState
{
	Authkey	k;
	PAKpriv	p;
	u8	kn[NONCELEN];
	u8	rnc[NONCELEN];
	int	active;
};

static TEE_Result
dp9ik_open_blob(const u8 *blob, u32 nblob, Authkey *k)
{
	const u8 *label, *pub;
	u32 nlabel, npub, npriv = sizeof(*k);
	u8 alg, usage;
	TEE_Result res;

	res = blob_open(blob, nblob, &alg, &usage, &label, &nlabel,
			&pub, &npub, (u8 *)k, &npriv);
	if(res != TEE_SUCCESS)
		return res;
	if(alg != BlobAlgDp9ik || usage != BlobUsageDp9ik ||
	   npriv != sizeof(*k))
		return TEE_ERROR_BAD_FORMAT;
	return TEE_SUCCESS;
}

static TEE_Result
cmd_dp9ik_import(u32 pt, TEE_Param p[4])
{
	Authkey k;
	char user[ANAMELEN+1];
	u8 *blob;
	u32 nblob = BlobMax;
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT))
		return TEE_ERROR_BAD_PARAMETERS;
	if(p[0].memref.size == 0 || p[0].memref.size > BlobMaxLabel ||
	   p[1].memref.size != AESKEYLEN ||
	   p[2].memref.size == 0 || p[2].memref.size > ANAMELEN)
		return TEE_ERROR_BAD_PARAMETERS;

	TEE_MemFill(&k, 0, sizeof(k));
	TEE_MemMove(k.aes, p[1].memref.buffer, AESKEYLEN);
	TEE_MemMove(user, p[2].memref.buffer, p[2].memref.size);
	user[p[2].memref.size] = 0;
	authpak_hash(&k, user);

	blob = TEE_Malloc(BlobMax, 0);
	if(blob == NULL){
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto out;
	}
	res = blob_seal(BlobAlgDp9ik, BlobUsageDp9ik,
			p[0].memref.buffer, p[0].memref.size, NULL, 0,
			(u8 *)&k, sizeof(k), blob, &nblob);
	if(res == TEE_SUCCESS){
		if(p[3].memref.size < nblob)
			res = TEE_ERROR_SHORT_BUFFER;
		else{
			TEE_MemMove(p[3].memref.buffer, blob, nblob);
			p[3].memref.size = nblob;
		}
	}
	TEE_MemFill(blob, 0, BlobMax);
	TEE_Free(blob);
out:
	TEE_MemFill(&k, 0, sizeof(k));
	TEE_MemFill(user, 0, sizeof(user));
	return res;
}

static TEE_Result
cmd_dp9ik_selftest(u32 pt, TEE_Param p[4])
{
	Authkey ck, sk;
	PAKpriv cp, sp;
	Dp9ikTicket t1, t2;
	Dp9ikAuthenticator a1, a2;
	u8 aes[AESKEYLEN];
	u8 cy[PAKYLEN], sy[PAKYLEN];
	char tbuf[MAXTICKETLEN], abuf[MAXAUTHENTLEN];
	int n;
	TEE_Result res;

	(void)p;
	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;

	TEE_GenerateRandom(aes, sizeof(aes));
	TEE_MemFill(&ck, 0, sizeof(ck));
	TEE_MemFill(&sk, 0, sizeof(sk));
	TEE_MemMove(ck.aes, aes, sizeof(aes));
	TEE_MemMove(sk.aes, aes, sizeof(aes));
	authpak_hash(&ck, "testuser");
	authpak_hash(&sk, "testuser");

	authpak_new(&cp, &ck, cy, 1);
	authpak_new(&sp, &sk, sy, 0);
	if(authpak_finish(&cp, &ck, sy) < 0 ||
	   authpak_finish(&sp, &sk, cy) < 0){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	if(TEE_MemCompare(ck.pakkey, sk.pakkey, PAKKEYLEN) != 0){
		res = TEE_ERROR_GENERIC;
		goto out;
	}

	TEE_MemFill(&t1, 0, sizeof(t1));
	t1.num = AuthTs;
	t1.form = 1;
	TEE_GenerateRandom(t1.chal, CHALLEN);
	TEE_GenerateRandom(t1.key, NONCELEN);
	TEE_MemMove(t1.cuid, "testuser", 8);
	TEE_MemMove(t1.suid, "authdom", 7);
	n = dp9ik_convT2M(&t1, tbuf, sizeof(tbuf), &ck);
	if(n != MAXTICKETLEN ||
	   dp9ik_convM2T(tbuf, n, &t2, &ck) != MAXTICKETLEN){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	if(t2.num != AuthTs || t2.form != 1 ||
	   TEE_MemCompare(t2.chal, t1.chal, CHALLEN) != 0 ||
	   TEE_MemCompare(t2.key, t1.key, NONCELEN) != 0){
		res = TEE_ERROR_GENERIC;
		goto out;
	}

	TEE_MemFill(&a1, 0, sizeof(a1));
	a1.num = AuthAc;
	TEE_GenerateRandom(a1.chal, CHALLEN);
	TEE_GenerateRandom(a1.rand, NONCELEN);
	n = dp9ik_convA2M(&a1, abuf, sizeof(abuf), &t1);
	if(n != MAXAUTHENTLEN ||
	   dp9ik_convM2A(abuf, n, &a2, &t1) != MAXAUTHENTLEN){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	if(a2.num != AuthAc ||
	   TEE_MemCompare(a2.chal, a1.chal, CHALLEN) != 0 ||
	   TEE_MemCompare(a2.rand, a1.rand, NONCELEN) != 0){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	res = TEE_SUCCESS;
out:
	TEE_MemFill(&ck, 0, sizeof(ck));
	TEE_MemFill(&sk, 0, sizeof(sk));
	TEE_MemFill(&cp, 0, sizeof(cp));
	TEE_MemFill(&sp, 0, sizeof(sp));
	TEE_MemFill(tbuf, 0, sizeof(tbuf));
	TEE_MemFill(abuf, 0, sizeof(abuf));
	return res;
}


static TEE_Result
cmd_dp9ik_init(Dp9ikState *ds, u32 pt, TEE_Param p[4])
{
	Ticketreq tr;
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT))
		return TEE_ERROR_BAD_PARAMETERS;
	if(p[1].memref.size != TICKREQLEN ||
	   p[2].memref.size != PAKYLEN ||
	   p[3].memref.size < PAKYLEN)
		return TEE_ERROR_BAD_PARAMETERS;
	res = dp9ik_open_blob(p[0].memref.buffer, p[0].memref.size, &ds->k);
	if(res != TEE_SUCCESS)
		return res;
	TEE_MemMove(&tr, p[1].memref.buffer, TICKREQLEN);
	if(tr.type != AuthPAK){
		res = TEE_ERROR_BAD_FORMAT;
		goto out;
	}
	TEE_MemMove(ds->p.y, p[2].memref.buffer, PAKYLEN);
	authpak_new(&ds->p, &ds->k, p[3].memref.buffer, 1);
	p[3].memref.size = PAKYLEN;
	ds->active = 1;
	res = TEE_SUCCESS;
out:
	if(res != TEE_SUCCESS)
		TEE_MemFill(&ds->k, 0, sizeof(ds->k));
	return res;
}

static TEE_Result
cmd_dp9ik_finish(Dp9ikState *ds, u32 pt, TEE_Param p[4])
{
	Dp9ikTicket t;
	Dp9ikAuthenticator a;
	u8 *out;
	u32 n;
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT))
		return TEE_ERROR_BAD_PARAMETERS;
	if(!ds->active ||
	   p[0].memref.size != 2*PAKYLEN ||
	   p[1].memref.size != 2*MAXTICKETLEN ||
	   p[2].memref.size < PAKYLEN ||
	   p[3].memref.size < MAXTICKETLEN + MAXAUTHENTLEN)
		return TEE_ERROR_BAD_PARAMETERS;

	res = authpak_finish(&ds->p, &ds->k,
			     (u8 *)p[0].memref.buffer + PAKYLEN);
	if(res < 0)
		return TEE_ERROR_BAD_FORMAT;

	if(dp9ik_convM2T(p[1].memref.buffer, MAXTICKETLEN, &t,
			 &ds->k) != MAXTICKETLEN ||
	   t.num != AuthTc)
		return TEE_ERROR_BAD_FORMAT;
	TEE_MemMove(ds->kn, t.key, NONCELEN);
	TEE_GenerateRandom(ds->rnc, NONCELEN);

	TEE_MemFill(&a, 0, sizeof(a));
	a.num = AuthAc;
	TEE_MemMove(a.chal, t.chal, CHALLEN);
	TEE_MemMove(a.rand, ds->rnc, NONCELEN);

	n = dp9ik_convA2M(&a, (char *)p[3].memref.buffer + MAXTICKETLEN,
			  p[3].memref.size - MAXTICKETLEN, &t);
	if(n != MAXAUTHENTLEN)
		return TEE_ERROR_GENERIC;
	TEE_MemMove(p[3].memref.buffer, (u8 *)p[1].memref.buffer + MAXTICKETLEN,
		    MAXTICKETLEN);
	p[3].memref.size = MAXTICKETLEN + MAXAUTHENTLEN;
	TEE_MemMove(p[2].memref.buffer, p[0].memref.buffer, PAKYLEN);
	p[2].memref.size = PAKYLEN;
	TEE_MemFill(&a, 0, sizeof(a));
	TEE_MemFill(&t, 0, sizeof(t));
	return TEE_SUCCESS;
}

static TEE_Result
cmd_dp9ik_reply(Dp9ikState *ds, u32 pt, TEE_Param p[4])
{
	static char info[] = "Plan 9 session secret";
	Dp9ikTicket t;
	Dp9ikAuthenticator a;
	u8 rn[2*NONCELEN];
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	if(!ds->active ||
	   p[0].memref.size != MAXAUTHENTLEN ||
	   p[1].memref.size != CHALLEN ||
	   p[2].memref.size < 256)
		return TEE_ERROR_BAD_PARAMETERS;

	TEE_MemFill(&t, 0, sizeof(t));
	t.form = 1;
	TEE_MemMove(t.key, ds->kn, NONCELEN);
	if(dp9ik_convM2A(p[0].memref.buffer, MAXAUTHENTLEN, &a, &t) !=
	   MAXAUTHENTLEN){
		res = TEE_ERROR_BAD_FORMAT;
		goto out;
	}
	if(a.num != AuthAs ||
	   TEE_MemCompare(a.chal, p[1].memref.buffer, CHALLEN) != 0){
		res = TEE_ERROR_SIGNATURE_INVALID;
		goto out;
	}
	TEE_MemMove(rn, ds->rnc, NONCELEN);
	TEE_MemMove(rn + NONCELEN, a.rand, NONCELEN);
	ta_hkdf_sha256(rn, sizeof(rn), (u8 *)info, sizeof(info)-1,
		       ds->kn, NONCELEN, p[2].memref.buffer, 256);
	p[2].memref.size = 256;
	ds->active = 0;
	res = TEE_SUCCESS;
out:
	TEE_MemFill(rn, 0, sizeof(rn));
	TEE_MemFill(&a, 0, sizeof(a));
	TEE_MemFill(&t, 0, sizeof(t));
	return res;
}


static TEE_Result
mlkem_open_blob(const u8 *blob, u32 nblob, u8 *sk, u32 *nsk)
{
	const u8 *label, *pub;
	u32 nlabel, npub;
	u8 alg, usage;
	TEE_Result res;

	res = blob_open(blob, nblob, &alg, &usage, &label, &nlabel,
			&pub, &npub, sk, nsk);
	if(res != TEE_SUCCESS)
		return res;
	if(alg != BlobAlgMlkem768 || usage != BlobUsageMlkem ||
	   *nsk != MLKEM_SKLEN)
		return TEE_ERROR_BAD_FORMAT;
	return TEE_SUCCESS;
}

static TEE_Result
cmd_mlkem_keygen(u32 pt, TEE_Param p[4])
{
	u8 *pk = NULL, *sk = NULL, *blob = NULL;
	u32 nblob = BlobMax;
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	pk = TEE_Malloc(MLKEM_PKLEN, 0);
	sk = TEE_Malloc(MLKEM_SKLEN, 0);
	blob = TEE_Malloc(BlobMax, 0);
	if(pk == NULL || sk == NULL || blob == NULL){
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto out;
	}
	if(PQCLEAN_MLKEM768_CLEAN_crypto_kem_keypair(pk, sk) != 0){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	res = blob_seal(BlobAlgMlkem768, BlobUsageMlkem,
			p[0].memref.buffer, p[0].memref.size,
			pk, MLKEM_PKLEN, sk, MLKEM_SKLEN, blob, &nblob);
	if(res != TEE_SUCCESS)
		goto out;
	if(p[1].memref.size < MLKEM_PKLEN ||
	   p[2].memref.size < nblob){
		res = TEE_ERROR_SHORT_BUFFER;
		goto out;
	}
	TEE_MemMove(p[1].memref.buffer, pk, MLKEM_PKLEN);
	p[1].memref.size = MLKEM_PKLEN;
	TEE_MemMove(p[2].memref.buffer, blob, nblob);
	p[2].memref.size = nblob;
	res = TEE_SUCCESS;
out:
	if(pk != NULL){
		TEE_MemFill(pk, 0, MLKEM_PKLEN);
		TEE_Free(pk);
	}
	if(sk != NULL){
		TEE_MemFill(sk, 0, MLKEM_SKLEN);
		TEE_Free(sk);
	}
	if(blob != NULL){
		TEE_MemFill(blob, 0, BlobMax);
		TEE_Free(blob);
	}
	return res;
}

static TEE_Result
cmd_mlkem_encaps(u32 pt, TEE_Param p[4])
{
	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	if(p[0].memref.size != MLKEM_PKLEN ||
	   p[1].memref.size < MLKEM_CTLEN ||
	   p[2].memref.size < MLKEM_SSLEN)
		return TEE_ERROR_BAD_PARAMETERS;
	if(PQCLEAN_MLKEM768_CLEAN_crypto_kem_enc(p[1].memref.buffer,
			p[2].memref.buffer, p[0].memref.buffer) != 0)
		return TEE_ERROR_GENERIC;
	p[1].memref.size = MLKEM_CTLEN;
	p[2].memref.size = MLKEM_SSLEN;
	return TEE_SUCCESS;
}

static TEE_Result
cmd_mlkem_decaps(u32 pt, TEE_Param p[4])
{
	u8 *sk = NULL;
	u32 nsk = MLKEM_SKLEN;
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	if(p[1].memref.size != MLKEM_CTLEN ||
	   p[2].memref.size < MLKEM_SSLEN)
		return TEE_ERROR_BAD_PARAMETERS;
	sk = TEE_Malloc(MLKEM_SKLEN, 0);
	if(sk == NULL)
		return TEE_ERROR_OUT_OF_MEMORY;
	res = mlkem_open_blob(p[0].memref.buffer, p[0].memref.size,
			      sk, &nsk);
	if(res != TEE_SUCCESS)
		goto out;
	if(PQCLEAN_MLKEM768_CLEAN_crypto_kem_dec(p[2].memref.buffer,
			p[1].memref.buffer, sk) != 0){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	p[2].memref.size = MLKEM_SSLEN;
	res = TEE_SUCCESS;
out:
	if(sk != NULL){
		TEE_MemFill(sk, 0, MLKEM_SKLEN);
		TEE_Free(sk);
	}
	return res;
}

static TEE_Result
cmd_mlkem_selftest(u32 pt, TEE_Param p[4])
{
	u8 *pk = NULL, *sk = NULL, *sk2 = NULL, *blob = NULL;
	u8 ct[MLKEM_CTLEN], ss1[MLKEM_SSLEN], ss2[MLKEM_SSLEN];
	u32 nblob = BlobMax, nsk = MLKEM_SKLEN;
	TEE_Result res;

	(void)p;
	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	pk = TEE_Malloc(MLKEM_PKLEN, 0);
	sk = TEE_Malloc(MLKEM_SKLEN, 0);
	sk2 = TEE_Malloc(MLKEM_SKLEN, 0);
	blob = TEE_Malloc(BlobMax, 0);
	if(pk == NULL || sk == NULL || sk2 == NULL || blob == NULL){
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto out;
	}
	if(PQCLEAN_MLKEM768_CLEAN_crypto_kem_keypair(pk, sk) != 0){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	res = blob_seal(BlobAlgMlkem768, BlobUsageMlkem,
			(const u8*)"selftest/mlkem", 15,
			pk, MLKEM_PKLEN, sk, MLKEM_SKLEN, blob, &nblob);
	if(res != TEE_SUCCESS)
		goto out;
	res = mlkem_open_blob(blob, nblob, sk2, &nsk);
	if(res != TEE_SUCCESS)
		goto out;
	if(PQCLEAN_MLKEM768_CLEAN_crypto_kem_enc(ct, ss1, pk) != 0 ||
	   PQCLEAN_MLKEM768_CLEAN_crypto_kem_dec(ss2, ct, sk2) != 0){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	if(TEE_MemCompare(ss1, ss2, MLKEM_SSLEN) != 0){
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	res = TEE_SUCCESS;
out:
	if(pk != NULL){
		TEE_MemFill(pk, 0, MLKEM_PKLEN);
		TEE_Free(pk);
	}
	if(sk != NULL){
		TEE_MemFill(sk, 0, MLKEM_SKLEN);
		TEE_Free(sk);
	}
	if(sk2 != NULL){
		TEE_MemFill(sk2, 0, MLKEM_SKLEN);
		TEE_Free(sk2);
	}
	if(blob != NULL){
		TEE_MemFill(blob, 0, BlobMax);
		TEE_Free(blob);
	}
	TEE_MemFill(ct, 0, sizeof(ct));
	TEE_MemFill(ss1, 0, sizeof(ss1));
	TEE_MemFill(ss2, 0, sizeof(ss2));
	return res;
}


static TEE_Result
userkey_derive(const u8 *idblob, u32 nid, const u8 *label, u32 nlabel,
	       u8 vk[32])
{
	static const char salt[] = "secstore vault v1";
	const u8 *l, *pub;
	u32 nl, np, nsk = MLDSA_SKLEN;
	u8 alg, usage;
	u8 sk[MLDSA_SKLEN];
	TEE_Result res;

	res = blob_open(idblob, nid, &alg, &usage, &l, &nl, &pub, &np,
			sk, &nsk);
	if(res != TEE_SUCCESS)
		return res;
	if(alg != BlobAlgMldsa44 || usage != BlobUsageIdentity ||
	   nsk != MLDSA_SKLEN)
		res = TEE_ERROR_BAD_FORMAT;
	else{
		ta_hkdf_sha256((u8 *)salt, sizeof(salt)-1, label, nlabel,
			       sk, nsk, vk, 32);
		res = TEE_SUCCESS;
	}
	TEE_MemFill(sk, 0, sizeof(sk));
	return res;
}

static TEE_Result
userkey_seal(u8 type, const u8 *label, u32 nlabel, const u8 *pub,
	     u32 npub, const u8 *priv, u32 npriv, const u8 vk[32],
	     u8 *blob, u32 *nblob)
{
	u8 tmp[UserKeyMax];
	u8 *out, *tag;
	u32 total;
	TEE_Result res;

	total = UserKeyHdrlen + nlabel + npub + npriv + 16;
	if(nblob == NULL || *nblob < total || total > sizeof(tmp))
		return TEE_ERROR_SHORT_BUFFER;
	TEE_MemMove(tmp, "KUSER1", 6);
	tmp[6] = 1;
	tmp[7] = type;
	tmp[8] = nlabel;
	tmp[9] = 0;
	tmp[10] = npub >> 8;
	tmp[11] = npub;
	tmp[12] = npriv >> 8;
	tmp[13] = npriv;
	tmp[14] = 0;
	tmp[15] = 0;
	TEE_GenerateRandom(tmp + 16, 12);
	TEE_MemMove(tmp + UserKeyHdrlen, label, nlabel);
	TEE_MemMove(tmp + UserKeyHdrlen + nlabel, pub, npub);

	out = tmp + UserKeyHdrlen + nlabel + npub;
	tag = out + npriv;
	res = aes_gcm(TEE_MODE_ENCRYPT, vk, tmp + 16, tmp,
		      UserKeyHdrlen + nlabel + npub, priv, npriv,
		      out, tag);
	if(res == TEE_SUCCESS){
		TEE_MemMove(blob, tmp, total);
		*nblob = total;
	}
	TEE_MemFill(tmp, 0, sizeof(tmp));
	return res;
}

static TEE_Result
userkey_parse(const u8 *blob, u32 nblob, u8 *type, const u8 **label,
	      u32 *nlabel, const u8 **pub, u32 *npub, const u8 **cipher,
	      u32 *ncipher, const u8 **tag)
{
	u32 ll, pl, cl;

	if(nblob < UserKeyHdrlen ||
	   TEE_MemCompare(blob, "KUSER1", 6) != 0 || blob[6] != 1)
		return TEE_ERROR_BAD_FORMAT;
	*type = blob[7];
	ll = blob[8];
	pl = ((u32)blob[10] << 8) | blob[11];
	cl = ((u32)blob[12] << 8) | blob[13];
	if(ll == 0 || ll > UserKeyMaxLabel || pl > MLDSA_PKLEN ||
	   cl == 0 || cl > MLDSA_SKLEN ||
	   nblob != UserKeyHdrlen + ll + pl + cl + 16)
		return TEE_ERROR_BAD_FORMAT;
	*label = blob + UserKeyHdrlen;
	*nlabel = ll;
	*pub = blob + UserKeyHdrlen + ll;
	*npub = pl;
	*cipher = *pub + pl;
	*ncipher = cl;
	*tag = *cipher + cl;
	return TEE_SUCCESS;
}

static TEE_Result
userkey_open(const u8 *idblob, u32 nid, const u8 *blob, u32 nblob,
	     u8 *priv, u32 *npriv)
{
	u8 aad[UserKeyHdrlen + UserKeyMaxLabel + MLDSA_PKLEN];
	u8 tagbuf[16], plain[MLDSA_SKLEN];
	const u8 *label, *pub, *cipher, *tag;
	u32 nlabel, npub, ncipher;
	u8 type, vk[32];
	TEE_Result res;

	res = userkey_parse(blob, nblob, &type, &label, &nlabel,
			    &pub, &npub, &cipher, &ncipher, &tag);
	if(res != TEE_SUCCESS)
		return res;
	if(*npriv < ncipher || ncipher > sizeof(plain))
		return TEE_ERROR_SHORT_BUFFER;
	res = userkey_derive(idblob, nid, label, nlabel, vk);
	if(res != TEE_SUCCESS)
		return res;
	TEE_MemMove(aad, blob, UserKeyHdrlen + nlabel + npub);
	TEE_MemMove(tagbuf, tag, sizeof(tagbuf));
	res = aes_gcm(TEE_MODE_DECRYPT, vk, (u8 *)blob + 16, aad,
		      UserKeyHdrlen + nlabel + npub, cipher, ncipher,
		      plain, tagbuf);
	if(res == TEE_SUCCESS){
		TEE_MemMove(priv, plain, ncipher);
		*npriv = ncipher;
	}
	TEE_MemFill(vk, 0, sizeof(vk));
	TEE_MemFill(aad, 0, sizeof(aad));
	TEE_MemFill(tagbuf, 0, sizeof(tagbuf));
	TEE_MemFill(plain, 0, sizeof(plain));
	return res;
}

static TEE_Result
userkey_generate(const u8 *idblob, u32 nid, u8 type, const u8 *label,
		 u32 nlabel, u8 *blob, u32 *nblob, u8 *outpriv,
		 u32 *noutpriv)
{
	u8 vk[32];
	u8 *pk = NULL, *sk = NULL;
	u32 npub, npriv;
	TEE_Result res;

	if(type == UserKeyMldsa44){
		npub = MLDSA_PKLEN;
		npriv = MLDSA_SKLEN;
	}else if(type == UserKeyMlkem768){
		npub = MLKEM_PKLEN;
		npriv = MLKEM_SKLEN;
	}else
		return TEE_ERROR_BAD_PARAMETERS;

	res = userkey_derive(idblob, nid, label, nlabel, vk);
	if(res != TEE_SUCCESS)
		return res;
	pk = TEE_Malloc(npub, 0);
	sk = TEE_Malloc(npriv, 0);
	if(pk == NULL || sk == NULL){
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto out;
	}
	if(type == UserKeyMldsa44)
		res = PQCLEAN_MLDSA44_CLEAN_crypto_sign_keypair(pk, sk) == 0 ?
			TEE_SUCCESS : TEE_ERROR_GENERIC;
	else
		res = PQCLEAN_MLKEM768_CLEAN_crypto_kem_keypair(pk, sk) == 0 ?
			TEE_SUCCESS : TEE_ERROR_GENERIC;
	if(res != TEE_SUCCESS)
		goto out;
	if(outpriv != NULL){
		if(*noutpriv < npriv){
			res = TEE_ERROR_SHORT_BUFFER;
			goto out;
		}
		TEE_MemMove(outpriv, sk, npriv);
		*noutpriv = npriv;
	}
	res = userkey_seal(type, label, nlabel, pk, npub, sk, npriv,
			   vk, blob, nblob);
out:
	if(pk != NULL){
		TEE_MemFill(pk, 0, npub);
		TEE_Free(pk);
	}
	if(sk != NULL){
		TEE_MemFill(sk, 0, npriv);
		TEE_Free(sk);
	}
	TEE_MemFill(vk, 0, sizeof(vk));
	return res;
}

static TEE_Result
cmd_userkey_keygen(u32 pt, TEE_Param p[4])
{
	u8 type;
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT))
		return TEE_ERROR_BAD_PARAMETERS;
	if(p[1].memref.size != 1 || p[2].memref.size == 0 ||
	   p[2].memref.size > UserKeyMaxLabel)
		return TEE_ERROR_BAD_PARAMETERS;
	type = *(u8 *)p[1].memref.buffer;
	res = userkey_generate(p[0].memref.buffer, p[0].memref.size, type,
			       p[2].memref.buffer, p[2].memref.size,
			       p[3].memref.buffer, &p[3].memref.size,
			       NULL, NULL);
	return res;
}

static TEE_Result
cmd_userkey_public(u32 pt, TEE_Param p[4])
{
	const u8 *label, *pub, *cipher, *tag;
	u32 nlabel, npub, ncipher;
	u8 type;
	TEE_Result res;

	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				 TEE_PARAM_TYPE_MEMREF_OUTPUT,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;
	res = userkey_parse(p[0].memref.buffer, p[0].memref.size, &type,
			    &label, &nlabel, &pub, &npub, &cipher, &ncipher,
			    &tag);
	if(res != TEE_SUCCESS)
		return res;
	if(p[1].memref.size < npub)
		return TEE_ERROR_SHORT_BUFFER;
	TEE_MemMove(p[1].memref.buffer, pub, npub);
	p[1].memref.size = npub;
	return TEE_SUCCESS;
}

static TEE_Result
cmd_userkey_selftest(u32 pt, TEE_Param p[4])
{
	static const char idlabel[] = "selftest/identity";
	static const char ulabel[] = "selftest/userkey";
	u8 idpk[MLDSA_PKLEN], idsk[MLDSA_SKLEN];
	u8 idblob[BlobMax], ublob[UserKeyMax];
	u8 want[MLDSA_SKLEN], got[MLDSA_SKLEN];
	u32 nidblob = BlobMax, nublob, nwant, ngot;
	int type;
	TEE_Result res;

	(void)p;
	if(pt != TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE,
				 TEE_PARAM_TYPE_NONE))
		return TEE_ERROR_BAD_PARAMETERS;

	if(PQCLEAN_MLDSA44_CLEAN_crypto_sign_keypair(idpk, idsk) != 0)
		return TEE_ERROR_GENERIC;
	res = blob_seal(BlobAlgMldsa44, BlobUsageIdentity,
			(const u8 *)idlabel, sizeof(idlabel)-1,
			idpk, MLDSA_PKLEN, idsk, MLDSA_SKLEN,
			idblob, &nidblob);
	if(res != TEE_SUCCESS)
		goto out;

	for(type = UserKeyMldsa44; type <= UserKeyMlkem768; type++){
		nublob = sizeof(ublob);
		nwant = sizeof(want);
		ngot = sizeof(got);
		res = userkey_generate(idblob, nidblob, type,
				       (const u8 *)ulabel, sizeof(ulabel)-1,
				       ublob, &nublob, want, &nwant);
		if(res != TEE_SUCCESS)
			goto out;
		res = userkey_open(idblob, nidblob, ublob, nublob,
				   got, &ngot);
		if(res != TEE_SUCCESS)
			goto out;
		if(nwant != ngot ||
		   TEE_MemCompare(want, got, nwant) != 0){
			res = TEE_ERROR_GENERIC;
			goto out;
		}
	}
	res = TEE_SUCCESS;
out:
	TEE_MemFill(idpk, 0, sizeof(idpk));
	TEE_MemFill(idsk, 0, sizeof(idsk));
	TEE_MemFill(idblob, 0, sizeof(idblob));
	TEE_MemFill(ublob, 0, sizeof(ublob));
	TEE_MemFill(want, 0, sizeof(want));
	TEE_MemFill(got, 0, sizeof(got));
	return res;
}

TEE_Result
TA_CreateEntryPoint(void)
{
	return TEE_SUCCESS;
}

void
TA_DestroyEntryPoint(void)
{
}

TEE_Result
TA_OpenSessionEntryPoint(uint32_t pt, TEE_Param p[4], void **sess)
{
	Dp9ikState *ds;

	(void)pt;
	(void)p;
	ds = TEE_Malloc(sizeof(*ds), 0);
	if(ds == NULL)
		return TEE_ERROR_OUT_OF_MEMORY;
	TEE_MemFill(ds, 0, sizeof(*ds));
	*sess = ds;
	return TEE_SUCCESS;
}

void
TA_CloseSessionEntryPoint(void *sess)
{
	Dp9ikState *ds = sess;

	if(ds != NULL){
		TEE_MemFill(ds, 0, sizeof(*ds));
		TEE_Free(ds);
	}
}

TEE_Result
TA_InvokeCommandEntryPoint(void *sess, uint32_t cmd, uint32_t pt,
			   TEE_Param p[4])
{
	Dp9ikState *ds = sess;

	switch(cmd){
	case TA_KEYROOT_CMD_PING:
		return cmd_ping(pt, p);
	case TA_KEYROOT_CMD_INFO:
		return cmd_info(pt, p);
	case TA_KEYROOT_CMD_DERIVE:
		return cmd_derive(pt, p);
	case TA_KEYROOT_CMD_MLDSA_KEYGEN:
		return cmd_mldsa_keygen(pt, p);
	case TA_KEYROOT_CMD_MLDSA_PUBLIC:
		return cmd_mldsa_public(pt, p);
	case TA_KEYROOT_CMD_MLDSA_SIGN:
		return cmd_mldsa_sign(pt, p);
	case TA_KEYROOT_CMD_MLDSA_VERIFY:
		return cmd_mldsa_verify(pt, p);
	case TA_KEYROOT_CMD_MLDSA_SELFTEST:
		return cmd_mldsa_selftest(pt, p);
	case TA_KEYROOT_CMD_P9_IMPORT:
		return cmd_p9_import(pt, p);
	case TA_KEYROOT_CMD_P9_TICKET:
		return cmd_p9_ticket(pt, p);
	case TA_KEYROOT_CMD_P9_AUTH:
		return cmd_p9_auth(pt, p);
	case TA_KEYROOT_CMD_P9_VERIFY:
		return cmd_p9_verify(pt, p);
	case TA_KEYROOT_CMD_P9_SELFTEST:
		return cmd_p9_selftest(pt, p);
	case TA_KEYROOT_CMD_DP9IK_IMPORT:
		return cmd_dp9ik_import(pt, p);
	case TA_KEYROOT_CMD_DP9IK_SELFTEST:
		return cmd_dp9ik_selftest(pt, p);
	case TA_KEYROOT_CMD_DP9IK_INIT:
		return cmd_dp9ik_init(ds, pt, p);
	case TA_KEYROOT_CMD_DP9IK_FINISH:
		return cmd_dp9ik_finish(ds, pt, p);
	case TA_KEYROOT_CMD_DP9IK_REPLY:
		return cmd_dp9ik_reply(ds, pt, p);
	case TA_KEYROOT_CMD_MLKEM_KEYGEN:
		return cmd_mlkem_keygen(pt, p);
	case TA_KEYROOT_CMD_MLKEM_ENCAPS:
		return cmd_mlkem_encaps(pt, p);
	case TA_KEYROOT_CMD_MLKEM_DECAPS:
		return cmd_mlkem_decaps(pt, p);
	case TA_KEYROOT_CMD_MLKEM_SELFTEST:
		return cmd_mlkem_selftest(pt, p);
	case TA_KEYROOT_CMD_USERKEY_KEYGEN:
		return cmd_userkey_keygen(pt, p);
	case TA_KEYROOT_CMD_USERKEY_PUBLIC:
		return cmd_userkey_public(pt, p);
	case TA_KEYROOT_CMD_USERKEY_SELFTEST:
		return cmd_userkey_selftest(pt, p);
	default:
		return TEE_ERROR_NOT_IMPLEMENTED;
	}
}
