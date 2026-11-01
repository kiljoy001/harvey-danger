#include <tee_internal_api.h>
#include "u.h"
#include "libc.h"
#include "libsec.h"

void *
mallocz(usize n, int clr)
{
	void *p;

	p = malloc(n);
	if(p != nil && clr)
		memset(p, 0, n);
	return p;
}

void
sysfatal(char *fmt, ...)
{
	(void)fmt;
	TEE_Panic(0x1001);
}

void
abort(void)
{
	TEE_Panic(0x1002);
}

void
setmalloctag(void *p, usize t)
{
	(void)p;
	(void)t;
}

usize
getcallerpc(void)
{
	return 0;
}

void
genrandom(u8 *buf, int nbytes)
{
	TEE_GenerateRandom(buf, nbytes);
}

/* Base32/64 are not used by the AuthPAK constants. */
int
dec64(u8 *out, int nout, char *in, int nin)
{
	(void)out; (void)nout; (void)in; (void)nin;
	return -1;
}

int
dec32(u8 *out, int nout, char *in, int nin)
{
	(void)out; (void)nout; (void)in; (void)nin;
	return -1;
}
