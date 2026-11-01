#ifndef KEYROOT_TA_LIBSEC_H
#define KEYROOT_TA_LIBSEC_H

#include "u.h"

typedef struct DigestState DigestState;
struct DigestState
{
	u64	len;
	union {
		u32	state[8];
		u64	bstate[8];
	};
	u8	buf[256];
	int	blen;
	char	malloced;
	char	seeded;
};

enum
{
	ChachaBsize = 64,
	SHA2_256dlen = 32,
};

typedef struct Chachastate Chachastate;
struct Chachastate
{
	union{
		u32int	input[16];
		struct {
			u32int	constant[4];
			u32int	key[8];
			u32int	counter;
			u32int	iv[3];
		};
	};
	u32int	xkey[8];
	int	rounds;
	int	ivwords;
};

void	setupChachastate(Chachastate*, u8*, ulong, u8*, ulong, int);
void	chacha_setiv(Chachastate*, u8*);
void	chacha_setblock(Chachastate*, u64int);
void	chacha_encrypt(u8*, ulong, Chachastate*);
void	chacha_encrypt2(u8*, u8*, ulong, Chachastate*);

void	ccpoly_encrypt(u8 *dat, ulong ndat, u8 *aad, ulong naad,
		       u8 tag[16], Chachastate *cs);
int	ccpoly_decrypt(u8 *dat, ulong ndat, u8 *aad, ulong naad,
		       u8 tag[16], Chachastate *cs);
DigestState*	poly1305(u8*, ulong, u8*, ulong, u8*, DigestState*);
int	tsmemcmp(void*, void*, ulong);

void	genrandom(u8 *buf, int nbytes);

/* Trusted-application SHA-256 / HKDF. */
void	ta_sha256(const u8 *data, u32 len, u8 out[32]);
void	ta_hmac_sha256(const u8 *key, u32 klen, const u8 *data, u32 dlen,
		       u8 out[32]);
void	ta_hkdf_sha256(const u8 *salt, u32 nsalt, const u8 *info,
		       u32 ninfo, const u8 *ikm, u32 nikm, u8 *out,
		       u32 dlen);

#endif
