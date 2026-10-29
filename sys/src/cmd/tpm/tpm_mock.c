#include <u.h>
#include <libc.h>
/*
 * tpm_mock.c — deterministic TPM 2.0 responder for development.
 *
 * Implements the small command set tpmfs exposes: Startup, SelfTest,
 * GetRandom and a capability query.  Random bytes are deterministic
 * (xorshift) so tests can assert framing without depending on hardware.
 */
#include "tpm.h"

typedef struct Mock Mock;
struct Mock {
	uvlong	state;
};

static uvlong
next(Mock *m)
{
	uvlong x;

	x = m->state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	m->state = x;
	return x;
}

static int
mocksend(TpmTransport *tr, uchar *cmd, int n, uchar *resp, int *rnp)
{
	Mock *m = tr->aux;
	u32 cc;
	int i, want, nresp, rn;

	rn = *rnp;
	if(n < TpmHdrLen || rn < TpmHdrLen)
		return -1;
	cc = tpmget32(cmd+6);

	resp[0] = cmd[0];
	resp[1] = cmd[1];
	tpmput32(resp+6, TpmRcSuccess);
	nresp = TpmHdrLen;

	switch(cc){
	case TpmCcStartup:
	case TpmCcSelfTest:
		break;

	case TpmCcPcrRead:
		if(n < TpmHdrLen + 10)
			return -1;
		nresp = TpmHdrLen + 4 + 4 + 2 + 1 + 3 + 4 + 2 + 32;
		if(nresp > rn)
			return -1;
		{
			uchar *p = resp + TpmHdrLen;
			tpmput32(p, 0); p += 4;		/* pcrUpdateCounter */
			tpmput32(p, 1); p += 4;		/* selection count */
			tpmput16(p, 0x000B); p += 2;	/* SHA256 */
			*p++ = 3;
			*p++ = cmd[17];
			*p++ = cmd[18];
			*p++ = cmd[19];
			tpmput32(p, 1); p += 4;		/* digest count */
			tpmput16(p, 32); p += 2;
			memset(p, 0x42, 32);
		}
		break;

	case TpmCcGetRandom:
		if(n < TpmHdrLen + 2)
			return -1;
		want = tpmget16(cmd+TpmHdrLen);
		if(want > 256)
			want = 256;
		if(TpmHdrLen + 2 + want > rn)
			return -1;
		tpmput16(resp+TpmHdrLen, want);
		for(i = 0; i < want; i++)
			resp[TpmHdrLen+2+i] = next(m) & 0xFF;
		nresp = TpmHdrLen + 2 + want;
		break;

	default:
		tpmput32(resp+6, TpmRcCmdCode);
		break;
	}

	tpmput32(resp+2, nresp);
	*rnp = nresp;
	return 0;
}

TpmTransport*
tpm_mock_transport(void)
{
	static Mock mock = { 0x9e3779b97f4a7c15ULL };
	static TpmTransport tr = {
		.name = "mock",
		.aux = &mock,
		.send = mocksend,
	};
	return &tr;
}
