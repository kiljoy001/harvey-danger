#include <u.h>
#include <libc.h>
/*
 * tpm_tis.c — raw TPM2 transport over a kernel device.
 *
 * The kernel side owns MMIO and TIS/CRB framing; this backend just
 * writes a complete TPM2 command and reads the response.  The default
 * path is /dev/tpm/data, matching a devtpm-style #T tree.  It can be
 * pointed at an I2C/SPI portal later without changing tpmfs.
 */
#include "tpm.h"

typedef struct Tis Tis;
struct Tis {
	char	*path;
	int	fd;
};

static int
tisopen(TpmTransport *tr)
{
	Tis *t = tr->aux;
	int fd;

	if(t->fd > 0)
		return 0;
	fd = open(t->path, ORDWR);
	if(fd < 0){
		werrstr("tpm: open %s: %r", t->path);
		return -1;
	}
	t->fd = fd;
	return 0;
}

static int
tissend(TpmTransport *tr, uchar *cmd, int n, uchar *resp, int *rnp)
{
	Tis *t = tr->aux;
	int i, r, rn;

	if(t->fd < 0 && tisopen(tr) < 0)
		return -1;
	if(write(t->fd, cmd, n) != n){
		werrstr("tpm: write %s: %r", t->path);
		return -1;
	}
	rn = *rnp;
	for(i = 0; i < 100; i++){
		r = read(t->fd, resp, rn);
		if(r > 0){
			*rnp = r;
			return 0;
		}
		if(r < 0){
			werrstr("tpm: read %s: %r", t->path);
			return -1;
		}
		sleep(1);
	}
	werrstr("tpm: no response from %s", t->path);
	return -1;
}

static void
tisclose(TpmTransport *tr)
{
	Tis *t = tr->aux;

	if(t->fd > 0)
		close(t->fd);
	t->fd = -1;
}

TpmTransport*
tpm_tis_transport(char *path)
{
	static Tis tis = { .fd = -1 };
	static TpmTransport tr = {
		.name = "tis",
		.aux = &tis,
		.open = tisopen,
		.send = tissend,
		.close = tisclose,
	};

	tis.path = path != nil ? path : "/dev/tpm/data";
	return &tr;
}
