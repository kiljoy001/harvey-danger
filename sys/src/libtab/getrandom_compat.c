/*
 * getrandom() compatibility shim.
 *
 * tab_hashed.c calls getrandom(2) for argon2id salts.  Keep the implementation
 * in Plan 9 libc terms so the same source builds under plan9port and native
 * Plan 9: try /dev/urandom first for Unix hosts, then fall back to Plan 9's
 * /dev/random.
 */
#include <u.h>
#include <libc.h>

long
getrandom(void *buf, unsigned long buflen, unsigned int flags)
{
	uchar *p;
	int fd;
	long n, off;

	USED(flags);
	fd = open("/dev/urandom", OREAD);
	if(fd < 0)
		fd = open("/dev/random", OREAD);
	if(fd < 0)
		return -1;
	p = buf;
	off = 0;
	while(off < (long)buflen){
		n = read(fd, p + off, (long)buflen - off);
		if(n <= 0){
			close(fd);
			return -1;
		}
		off += n;
	}
	close(fd);
	return off;
}
