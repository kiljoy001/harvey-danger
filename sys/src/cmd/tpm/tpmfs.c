/*
 * tpmfs — userspace TPM 2.0 file server.
 *
 * Exposes a TPM to the namespace so factotum, libtab and ordinary
 * programs never touch the transport:
 *
 *	/mnt/tpm/status	read-only state
 *	/mnt/tpm/data	write a complete TPM2 command, read its response
 *	/mnt/tpm/ctl	write startup, selftest, getrandom N, pcrread N
 *	/mnt/tpm/rand	read TPM2_GetRandom bytes
 *
 * The transport is mock (deterministic, for tests) or tis (a raw
 * /dev/tpm/data style device).  Only one command is in flight at a
 * time; the TPM is a serial command processor.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "tpm.h"

/*
 * libthread gives threadmain an 8 KiB stack by default.  The CRB
 * transport reads the ACPI tables and the SAPI marshaling keeps a few
 * KiB of command/response frames, so give the main thread room.
 */
int mainstacksize = 128*1024;

static Srv fs;
static Tpm tpm;

static void
setstatus(char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	vsnprint(tpm.status, sizeof tpm.status, fmt, va);
	va_end(va);
}

/* Transport call with tpm.lk held. */
static int
transmit(Tpm *t, uchar *cmd, int n)
{
	int rn;

	if(n < TpmHdrLen || n > TpmMax){
		werrstr("tpm: bad command length %d", n);
		return -1;
	}
	if(tpmget32(cmd+2) != (uint)n){
		werrstr("tpm: command size mismatch");
		return -1;
	}
	rn = sizeof t->resp;
	if(t->tr->send(t->tr, cmd, n, t->resp, &rn) < 0)
		return -1;
	if(rn < TpmHdrLen){
		werrstr("tpm: short response %d", rn);
		return -1;
	}
	t->nresp = rn;
	return 0;
}

int
tpm_open(Tpm *t, char *backend, char *path)
{
	memset(t, 0, sizeof *t);
	if(backend == nil || strcmp(backend, "mock") == 0)
		t->tr = tpm_mock_transport();
	else if(strcmp(backend, "tis") == 0)
		t->tr = tpm_tis_transport(path);
	else if(strcmp(backend, "crb") == 0)
		t->tr = tpm_crb_transport();
	else {
		werrstr("tpm: unknown backend %s", backend);
		return -1;
	}
	if(t->tr->open != nil && t->tr->open(t->tr) < 0)
		return -1;
	tpm_sapi_ctx = t;
	setstatus("%s ready", t->tr->name);
	return 0;
}

void
tpm_close(Tpm *t)
{
	if(t->tr != nil && t->tr->close != nil)
		t->tr->close(t->tr);
	t->tr = nil;
}

int
tpm_send(Tpm *t, uchar *cmd, int n)
{
	int rc;

	lock(&t->lk);
	rc = transmit(t, cmd, n);
	unlock(&t->lk);
	return rc;
}

int
tpm_startup(Tpm *t)
{
	uchar cmd[12];
	uint rc;

	tpmput16(cmd, TpmStNoSessions);
	tpmput32(cmd+2, sizeof cmd);
	tpmput32(cmd+6, TpmCcStartup);
	tpmput16(cmd+10, 0);		/* TPM_SU_CLEAR */

	lock(&t->lk);
	if(transmit(t, cmd, sizeof cmd) < 0){
		unlock(&t->lk);
		return -1;
	}
	rc = tpmget32(t->resp+6);
	if(rc == TpmRcInitialize)	/* already started */
		rc = TpmRcSuccess;
	setstatus("%s startup %s", t->tr->name,
		rc == TpmRcSuccess ? "ok" : "failed");
	unlock(&t->lk);
	return rc == TpmRcSuccess ? 0 : -1;
}

int
tpm_selftest(Tpm *t)
{
	uchar cmd[11];
	uint rc;

	tpmput16(cmd, TpmStNoSessions);
	tpmput32(cmd+2, sizeof cmd);
	tpmput32(cmd+6, TpmCcSelfTest);
	cmd[10] = 1;			/* fullTest */

	lock(&t->lk);
	if(transmit(t, cmd, sizeof cmd) < 0){
		unlock(&t->lk);
		return -1;
	}
	rc = tpmget32(t->resp+6);
	setstatus("%s selftest %s", t->tr->name,
		rc == TpmRcSuccess ? "ok" : "failed");
	unlock(&t->lk);
	return rc == TpmRcSuccess ? 0 : -1;
}

int
tpm_getrandom(Tpm *t, uchar *out, int n)
{
	uchar cmd[12];
	uint rc;
	int got;

	if(n <= 0)
		return 0;
	if(n > 256)
		n = 256;

	tpmput16(cmd, TpmStNoSessions);
	tpmput32(cmd+2, sizeof cmd);
	tpmput32(cmd+6, TpmCcGetRandom);
	tpmput16(cmd+10, n);

	lock(&t->lk);
	if(transmit(t, cmd, sizeof cmd) < 0){
		unlock(&t->lk);
		return -1;
	}
	rc = tpmget32(t->resp+6);
	if(rc != TpmRcSuccess){
		setstatus("%s getrandom failed: %#ux", t->tr->name, rc);
		unlock(&t->lk);
		return -1;
	}
	got = tpmget16(t->resp+TpmHdrLen);
	if(got > n)
		got = n;
	memmove(out, t->resp+TpmHdrLen+2, got);
	setstatus("%s getrandom %d bytes", t->tr->name, got);
	unlock(&t->lk);
	return got;
}

int
tpm_pcrread(Tpm *t, int pcr)
{
	uchar cmd[20];
	int idx;

	if(pcr < 0 || pcr > 23){
		werrstr("tpm: pcr %d out of range", pcr);
		return -1;
	}
	tpmput16(cmd, TpmStNoSessions);
	tpmput32(cmd+2, sizeof cmd);
	tpmput32(cmd+6, TpmCcPcrRead);
	tpmput32(cmd+10, 1);		/* one selection */
	tpmput16(cmd+14, 0x000B);	/* TPM_ALG_SHA256 */
	cmd[16] = 3;
	idx = pcr / 8;
	cmd[17+idx] = 1 << (pcr % 8);

	lock(&t->lk);
	if(transmit(t, cmd, sizeof cmd) < 0){
		unlock(&t->lk);
		return -1;
	}
	if(tpmget32(t->resp+6) != TpmRcSuccess){
		setstatus("%s pcrread failed", t->tr->name);
		unlock(&t->lk);
		return -1;
	}
	setstatus("%s pcrread %d ok", t->tr->name, pcr);
	unlock(&t->lk);
	return 0;
}

int
tpm_pcrget(Tpm *t, int pcr, uchar *digest, int len)
{
	uchar cmd[20], *p, rsp[512];
	int idx, n, dlen, i, count;

	if(pcr < 0 || pcr > 23 || len <= 0)
		return -1;
	tpmput16(cmd, TpmStNoSessions);
	tpmput32(cmd+2, sizeof cmd);
	tpmput32(cmd+6, TpmCcPcrRead);
	tpmput32(cmd+10, 1);
	tpmput16(cmd+14, 0x000B);	/* SHA256 */
	cmd[16] = 3;
	idx = pcr / 8;
	cmd[17+idx] = 1 << (pcr % 8);

	lock(&t->lk);
	if(transmit(t, cmd, sizeof cmd) < 0){
		unlock(&t->lk);
		return -1;
	}
	if(tpmget32(t->resp+6) != TpmRcSuccess){
		unlock(&t->lk);
		return -1;
	}
	n = t->nresp;
	if(n > sizeof rsp)
		n = sizeof rsp;
	memmove(rsp, t->resp, n);
	unlock(&t->lk);

	if(n < TpmHdrLen + 4)
		return -1;
	p = rsp + TpmHdrLen;
	p += 4;					/* pcrUpdateCounter */
	if(p + 4 > rsp+n)
		return -1;
	count = tpmget32(p);
	p += 4;
	for(i = 0; i < count; i++){
		if(p + 3 > rsp+n)
			return -1;
		p += 2;				/* hash alg */
		i += 0;
		idx = *p++;
		if(p + idx > rsp+n)
			return -1;
		p += idx;
	}
	if(p + 4 > rsp+n)
		return -1;
	count = tpmget32(p);
	p += 4;
	if(count < 1 || p + 2 > rsp+n)
		return -1;
	dlen = tpmget16(p);
	p += 2;
	if(dlen > len)
		dlen = len;
	if(p + dlen > rsp+n)
		return -1;
	memmove(digest, p, dlen);
	return dlen;
}

static void
fsopen(Req *r)
{
	respond(r, nil);
}

static void
fsread(Req *r)
{
	char *name, buf[128];
	uchar rnd[256];
	int n;

	name = r->fid->file->name;
	if(strcmp(name, "seal") == 0){
		lock(&tpm.lk);
		readbuf(r, tpm.blob, tpm.nblob);
		unlock(&tpm.lk);
		respond(r, nil);
		return;
	}
	if(strcmp(name, "unseal") == 0){
		lock(&tpm.lk);
		readbuf(r, tpm.plain, tpm.nplain);
		unlock(&tpm.lk);
		respond(r, nil);
		return;
	}
	if(r->fid->file->aux != nil){
		int pcr;
		uchar digest[64];

		pcr = (int)(uintptr)r->fid->file->aux - 1;
		n = tpm_pcrget(&tpm, pcr, digest, sizeof digest);
		if(n < 0){
			respond(r, "tpm: pcr read failed");
			return;
		}
		readbuf(r, digest, n);
		respond(r, nil);
		return;
	}
	if(strcmp(name, "status") == 0 || strcmp(name, "ctl") == 0){
		lock(&tpm.lk);
		snprint(buf, sizeof buf, "%s\n", tpm.status);
		unlock(&tpm.lk);
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	if(strcmp(name, "data") == 0){
		lock(&tpm.lk);
		readbuf(r, tpm.resp, tpm.nresp);
		unlock(&tpm.lk);
		respond(r, nil);
		return;
	}
	if(strcmp(name, "rand") == 0){
		n = r->ifcall.count;
		if(n > sizeof rnd)
			n = sizeof rnd;
		n = tpm_getrandom(&tpm, rnd, n);
		if(n < 0){
			respond(r, "tpm: getrandom failed");
			return;
		}
		readbuf(r, rnd, n);
		respond(r, nil);
		return;
	}
	respond(r, "not a tpm file");
}

static int
tpuctl(Tpm *t, char *cmd, char **args, int nargs)
{
	int n;

	if(strcmp(cmd, "startup") == 0)
		return tpm_startup(t);
	if(strcmp(cmd, "selftest") == 0)
		return tpm_selftest(t);
	if(strcmp(cmd, "getrandom") == 0){
		uchar buf[256];

		n = nargs > 0 ? atoi(args[0]) : 16;
		if(n > sizeof buf)
			n = sizeof buf;
		return tpm_getrandom(t, buf, n) < 0 ? -1 : 0;
	}
	if(strcmp(cmd, "pcrread") == 0){
		n = nargs > 0 ? atoi(args[0]) : 0;
		return tpm_pcrread(t, n);
	}
	if(strcmp(cmd, "status") == 0)
		return 0;
	werrstr("tpm: unknown ctl command %s", cmd);
	return -1;
}

static void
fswrite(Req *r)
{
	char *name, *buf, *args[6];
	int n, nargs;

	name = r->fid->file->name;
	n = r->ifcall.count;

	if(strcmp(name, "seal") == 0){
		ushort blen;

		if(n <= 0 || n > (int)sizeof tpm.plain){
			respond(r, "tpm: seal data too large");
			return;
		}
		blen = sizeof tpm.blob;
		if(tpm2_seal_to_srk((uchar*)r->ifcall.data, n, nil, 0,
		    tpm.blob, &blen) < 0){
			respond(r, "tpm: seal failed");
			return;
		}
		lock(&tpm.lk);
		tpm.nblob = blen;
		unlock(&tpm.lk);
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}
	if(strcmp(name, "unseal") == 0){
		ushort plen;

		if(n <= 0 || n > (int)sizeof tpm.blob){
			respond(r, "tpm: unseal blob too large");
			return;
		}
		plen = sizeof tpm.plain;
		if(tpm2_unseal_from_blob((uchar*)r->ifcall.data, n, nil, 0,
		    tpm.plain, &plen) < 0){
			respond(r, "tpm: unseal failed");
			return;
		}
		lock(&tpm.lk);
		tpm.nplain = plen;
		unlock(&tpm.lk);
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}
	if(strcmp(name, "data") == 0){
		if(tpm_send(&tpm, (uchar*)r->ifcall.data, n) < 0){
			respond(r, "tpm: command failed");
			return;
		}
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}
	if(strcmp(name, "ctl") == 0){
		buf = malloc(n+1);
		if(buf == nil){
			respond(r, "out of memory");
			return;
		}
		memmove(buf, r->ifcall.data, n);
		buf[n] = 0;
		nargs = tokenize(buf, args, nelem(args));
		if(nargs == 0){
			free(buf);
			respond(r, nil);
			return;
		}
		if(tpuctl(&tpm, args[0], args+1, nargs-1) < 0){
			free(buf);
			respond(r, "tpm: ctl command failed");
			return;
		}
		free(buf);
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}
	respond(r, "not a writable tpm file");
}

static void
usage(void)
{
	fprint(2, "usage: tpmfs [-D] [-t mock|tis] [-d device] [-s srvname] [-m mtpt]\n");
	exits("usage");
}

void
threadmain(int argc, char **argv)
{
	char *backend, *dev, *mtpt, *srvname;

	backend = "mock";
	dev = nil;
	mtpt = nil;
	srvname = "tpm";

	ARGBEGIN{
	case 'D':
		chatty9p++;
		break;
	case 't':
		backend = EARGF(usage());
		break;
	case 'd':
		dev = EARGF(usage());
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	if(tpm_open(&tpm, backend, dev) < 0)
		sysfatal("tpmfs: %r");
	/* postmountsrv wants either a service name or a mount point. */
	if(mtpt != nil)
		srvname = nil;

	fs.tree = alloctree(nil, nil, DMDIR|0777, nil);
	if(fs.tree == nil)
		sysfatal("tpmfs: alloctree: %r");
	createfile(fs.tree->root, "status", getuser(), 0444, nil);
	createfile(fs.tree->root, "data", getuser(), 0600, nil);
	createfile(fs.tree->root, "ctl", getuser(), 0600, nil);
	createfile(fs.tree->root, "rand", getuser(), 0400, nil);
	createfile(fs.tree->root, "seal", getuser(), 0600, nil);
	createfile(fs.tree->root, "unseal", getuser(), 0600, nil);
	{
		File *pcr;
		char name[8];
		int i;

		pcr = createfile(fs.tree->root, "pcr", getuser(), DMDIR|0555, nil);
		for(i = 0; i < 24; i++){
			snprint(name, sizeof name, "%d", i);
			createfile(pcr, name, getuser(), 0444, (void*)(uintptr)(i+1));
		}
	}
	fs.open = fsopen;
	fs.read = fsread;
	fs.write = fswrite;

	threadpostmountsrv(&fs, srvname, mtpt, MREPL|MCREATE);
	threadexits(nil);
}
