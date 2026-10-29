/*
 * tpm.h — userspace TPM 2.0 command transport.
 *
 * The TPM2 protocol lives here; the backend only moves bytes.  A
 * backend is either the deterministic mock used by tests or a raw
 * device that already speaks TPM2 command/response frames (the
 * devtpm style #T/data interface, or any future I2C/SPI transport).
 */
#ifndef _TPM_H_
#define _TPM_H_

enum {
	TpmMax		= 4096,
	TpmHdrLen	= 10,

	/* TPM 2.0 structure tags */
	TpmStNoSessions	= 0x8001,
	TpmStSessions	= 0x8002,

	/* command codes */
	TpmCcStartup	= 0x00000144,
	TpmCcShutdown	= 0x00000145,
	TpmCcSelfTest	= 0x00000143,
	TpmCcGetRandom	= 0x0000017B,
	TpmCcPcrRead	= 0x0000017E,
	TpmCcGetCap	= 0x0000017A,

	/* return codes */
	TpmRcSuccess	= 0x00000000,
	TpmRcInitialize	= 0x00000100,
	TpmRcFailure	= 0x00000101,
	TpmRcCmdCode	= 0x00000143,
};

typedef struct TpmTransport TpmTransport;
struct TpmTransport {
	char	*name;
	void	*aux;
	int	(*open)(TpmTransport*);
	int	(*send)(TpmTransport*, uchar*, int, uchar*, int*);
	void	(*close)(TpmTransport*);
};

typedef struct Tpm Tpm;
struct Tpm {
	Lock		lk;
	TpmTransport	*tr;
	uchar		resp[TpmMax];
	int		nresp;
	char		status[128];
	uchar		blob[2048];
	int		nblob;
	uchar		plain[256];
	int		nplain;
};

extern Tpm *tpm_sapi_ctx;
int	tpm2_seal_to_srk(const uchar*, ushort, const uchar*, ushort, uchar*, ushort*);
int	tpm2_unseal_from_blob(const uchar*, ushort, const uchar*, ushort, uchar*, ushort*);

static ushort
tpmget16(uchar *p)
{
	return (ushort)(p[0]<<8 | p[1]);
}

static uint
tpmget32(uchar *p)
{
	return (uint)p[0]<<24 | (uint)p[1]<<16 | (uint)p[2]<<8 | p[3];
}

static void
tpmput16(uchar *p, ushort v)
{
	p[0] = v>>8;
	p[1] = v;
}

static void
tpmput32(uchar *p, uint v)
{
	p[0] = v>>24;
	p[1] = v>>16;
	p[2] = v>>8;
	p[3] = v;
}

TpmTransport*	tpm_mock_transport(void);
TpmTransport*	tpm_tis_transport(char *path);
TpmTransport*	tpm_crb_transport(void);

int	tpm_open(Tpm*, char *backend, char *path);
void	tpm_close(Tpm*);
int	tpm_send(Tpm*, uchar*, int);
int	tpm_startup(Tpm*);
int	tpm_selftest(Tpm*);
int	tpm_getrandom(Tpm*, uchar*, int);
int	tpm_pcrread(Tpm*, int);
int	tpm_pcrget(Tpm*, int, uchar*, int);

#endif
