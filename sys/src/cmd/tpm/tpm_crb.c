/*
 * tpm_crb.c — userspace TPM 2.0 CRB transport.
 *
 * This is the hardware path used on babyFileServer: find the ACPI TPM2
 * table through #P/acpitbls, then drive the CRB control area and
 * command/response buffers through #P/acpimem.  #P/acpimem must be
 * writable (hostowner on 9front); a kernel #T device is the narrower
 * alternative.
 */
#include <u.h>
#include <libc.h>
#include "tpm.h"

typedef struct Crb Crb;
struct Crb {
	char	*dir;
	int	memfd;
	uvlong	ca;
	uvlong	cmdpa;
	ulong	cmdsz;
	uvlong	rsppa;
	ulong	rspsz;
};

static ulong
get32le(uchar *p)
{
	return (ulong)p[0] | (ulong)p[1]<<8 | (ulong)p[2]<<16 | (ulong)p[3]<<24;
}

static uvlong
get64le(uchar *p)
{
	return (uvlong)get32le(p) | (uvlong)get32le(p+4)<<32;
}

static ushort
get16le(uchar *p)
{
	return p[0] | p[1]<<8;
}

static void
put32le(uchar *p, ulong v)
{
	p[0] = v;
	p[1] = v>>8;
	p[2] = v>>16;
	p[3] = v>>24;
}

static void
put16be(uchar *p, ulong v)
{
	p[0] = v>>8;
	p[1] = v;
}

static void
put32be(uchar *p, ulong v)
{
	p[0] = v>>24;
	p[1] = v>>16;
	p[2] = v>>8;
	p[3] = v;
}

static void
mread(Crb *c, uvlong pa, void *buf, int n)
{
	if(seek(c->memfd, pa, 0) < 0)
		sysfatal("tpmcrb: seek %#llux: %r", pa);
	if(readn(c->memfd, buf, n) != n)
		sysfatal("tpmcrb: read %#llux: %r", pa);
}

static void
mwrite(Crb *c, uvlong pa, void *buf, int n)
{
	if(seek(c->memfd, pa, 0) < 0)
		sysfatal("tpmcrb: seek %#llux: %r", pa);
	if(write(c->memfd, buf, n) != n)
		sysfatal("tpmcrb: write %#llux: %r", pa);
}

static uchar*
readall(char *path, int *np)
{
	int fd, n, nr;
	uchar *buf, tmp[8192];

	fd = open(path, OREAD);
	if(fd < 0)
		sysfatal("tpmcrb: open %s: %r", path);
	buf = nil;
	n = 0;
	for(;;){
		nr = read(fd, tmp, sizeof tmp);
		if(nr < 0)
			sysfatal("tpmcrb: read %s: %r", path);
		if(nr == 0)
			break;
		buf = realloc(buf, n+nr);
		if(buf == nil)
			sysfatal("tpmcrb: realloc");
		memmove(buf+n, tmp, nr);
		n += nr;
	}
	close(fd);
	*np = n;
	return buf;
}

static uvlong
findtpm2(char *path)
{
	int n, off;
	ulong len;
	uchar *buf, *p;
	uvlong ca;

	buf = readall(path, &n);
	for(off = 0; off+36 <= n; off += len){
		p = buf + off;
		len = get32le(p+4);
		if(len < 36 || off+len > n)
			break;
		if(memcmp(p, "TPM2", 4) == 0){
			if(len < 52)
				sysfatal("tpmcrb: short TPM2 table");
			ca = get64le(p+40);
			free(buf);
			return ca;
		}
	}
	free(buf);
	sysfatal("tpmcrb: no TPM2 ACPI table");
	return 0;
}

static int
crbopen(TpmTransport *tr)
{
	Crb *c = tr->aux;
	char *tbl;
	uchar b[48];
	ulong start;

	if(c->memfd > 0)
		return 0;
	c->dir = smprint("/tmp/tpmcrb.%d", getpid());
	if(c->dir == nil)
		return -1;
	if(access(c->dir, AEXIST) < 0 &&
	   create(c->dir, OREAD, DMDIR|0700) < 0){
		werrstr("tpmcrb: create %s: %r", c->dir);
		return -1;
	}
	if(bind("#P", c->dir, MREPL) < 0){
		werrstr("tpmcrb: bind #P: %r");
		return -1;
	}
	tbl = smprint("%s/acpitbls", c->dir);
	if(tbl == nil)
		return -1;
	c->ca = findtpm2(tbl);
	free(tbl);

	c->memfd = open(smprint("%s/acpimem", c->dir), ORDWR);
	if(c->memfd < 0){
		werrstr("tpmcrb: open acpimem: %r");
		return -1;
	}
	mread(c, c->ca, b, sizeof b);
	c->cmdsz = get32le(b+0x18);
	c->cmdpa = get64le(b+0x1c);
	c->rspsz = get32le(b+0x24);
	c->rsppa = get64le(b+0x28);
	if(c->cmdsz < TpmHdrLen || c->rspsz < TpmHdrLen){
		werrstr("tpmcrb: bad buffer sizes");
		return -1;
	}

	/* Clear a stale start bit and make sure the TPM is idle. */
	put32le(b, 0);
	mwrite(c, c->ca+0x08, b, 4);
	for(start = 1; ; ){
		mread(c, c->ca+0x0c, b, 4);
		start = get32le(b) & 1;
		if(start == 0)
			break;
		sleep(1);
	}
	return 0;
}

static int
crbsend(TpmTransport *tr, uchar *cmd, int n, uchar *resp, int *rnp)
{
	Crb *c = tr->aux;
	uchar b[16], hdr[10];
	ulong rsize;
	int i, rn;

	if(c->memfd <= 0 && crbopen(tr) < 0)
		return -1;
	if(n < TpmHdrLen || n > c->cmdsz){
		werrstr("tpmcrb: command too large (%d)", n);
		return -1;
	}

	put32le(b, 0);
	mwrite(c, c->ca+0x08, b, 4);	/* ctrl_cancel */
	mwrite(c, c->cmdpa, cmd, n);
	put32le(b, 1);
	mwrite(c, c->ca+0x0c, b, 4);	/* ctrl_start */

	for(i = 0; i < 30000; i++){
		mread(c, c->ca+0x0c, b, 4);
		if((get32le(b) & 1) == 0)
			break;
		sleep(1);
	}
	if(i >= 30000){
		werrstr("tpmcrb: timeout waiting for TPM");
		return -1;
	}

	mread(c, c->rsppa, hdr, sizeof hdr);
	rsize = (ulong)hdr[2]<<24 | (ulong)hdr[3]<<16 |
		(ulong)hdr[4]<<8 | hdr[5];
	if(rsize < TpmHdrLen || rsize > c->rspsz || rsize > *rnp){
		werrstr("tpmcrb: bad response size %lud", rsize);
		return -1;
	}
	mread(c, c->rsppa, resp, rsize);
	*rnp = rsize;
	return 0;
}

static void
crbclose(TpmTransport *tr)
{
	Crb *c = tr->aux;

	if(c->memfd > 0)
		close(c->memfd);
	c->memfd = -1;
}

TpmTransport*
tpm_crb_transport(void)
{
	static Crb crb = { .memfd = -1 };
	static TpmTransport tr = {
		.name = "crb",
		.aux = &crb,
		.open = crbopen,
		.send = crbsend,
		.close = crbclose,
	};
	return &tr;
}
