/*
 * tpmtest — exercise tpmfs without a TPM.
 *
 *	node /bin/tpmtest /tmp/tpm
 */
#include <u.h>
#include <libc.h>
#include "tpm.h"

static int
sendcmd(int fd, uchar *cmd, int n, uchar *resp, int rn)
{
	int r;

	if(write(fd, cmd, n) != n){
		fprint(2, "tpmtest: write: %r\n");
		return -1;
	}
	/* The file offset is shared between write and read. */
	if(seek(fd, 0, 0) < 0){
		fprint(2, "tpmtest: seek: %r\n");
		return -1;
	}
	r = read(fd, resp, rn);
	if(r < TpmHdrLen){
		fprint(2, "tpmtest: read: %d bytes: %r\n", r);
		return -1;
	}
	return r;
}

static void
usage(void)
{
	fprint(2, "usage: tpmtest [-t mock|tis] [mountpoint]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *mtpt, *backend, path[256], buf[256];
	uchar cmd[32], resp[512];
	u32 rc;
	int fd, r, i, n, want;

	mtpt = "/tmp/tpm";
	backend = nil;
	ARGBEGIN{
	case 't':
		backend = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc > 1)
		usage();
	if(argc == 1)
		mtpt = argv[0];

	snprint(path, sizeof path, "%s/status", mtpt);
	fd = open(path, OREAD);
	if(fd < 0)
		sysfatal("tpmtest: open %s: %r", path);
	r = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(r <= 0)
		sysfatal("tpmtest: empty status");
	buf[r] = 0;
	print("status: %s", buf);
	if(backend != nil && strncmp(buf, backend, strlen(backend)) != 0)
		sysfatal("tpmtest: status does not match backend %s", backend);

	snprint(path, sizeof path, "%s/data", mtpt);
	fd = open(path, ORDWR);
	if(fd < 0)
		sysfatal("tpmtest: open %s: %r", path);

	/* TPM2_Startup(CLEAR) */
	tpmput16(cmd, TpmStNoSessions);
	tpmput32(cmd+2, 12);
	tpmput32(cmd+6, TpmCcStartup);
	tpmput16(cmd+10, 0);
	r = sendcmd(fd, cmd, 12, resp, sizeof resp);
	if(r < 0)
		sysfatal("tpmtest: startup");
	rc = tpmget32(resp+6);
	if(rc != TpmRcSuccess && rc != TpmRcInitialize)
		sysfatal("tpmtest: startup rc %#ux", rc);
	print("startup: ok\n");

	/* TPM2_GetRandom(16) */
	want = 16;
	tpmput16(cmd, TpmStNoSessions);
	tpmput32(cmd+2, 12);
	tpmput32(cmd+6, TpmCcGetRandom);
	tpmput16(cmd+10, want);
	r = sendcmd(fd, cmd, 12, resp, sizeof resp);
	if(r < 0)
		sysfatal("tpmtest: getrandom");
	rc = tpmget32(resp+6);
	if(rc != TpmRcSuccess)
		sysfatal("tpmtest: getrandom rc %#ux", rc);
	n = tpmget16(resp+TpmHdrLen);
	if(n > want)
		n = want;
	print("getrandom %d:", n);
	for(i = 0; i < n; i++)
		print(" %02ux", resp[TpmHdrLen+2+i]);
	print("\n");
	close(fd);

	/* Control path */
	snprint(path, sizeof path, "%s/ctl", mtpt);
	fd = open(path, OWRITE);
	if(fd < 0)
		sysfatal("tpmtest: open %s: %r", path);
	if(write(fd, "selftest\n", 9) != 9)
		sysfatal("tpmtest: ctl write: %r");
	close(fd);

	print("TPM2 PASS\n");
	exits(nil);
}
