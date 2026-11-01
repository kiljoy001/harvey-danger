/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Normal-world randombytes for the verifier-side build.  The verifier
 * does not generate keys, but the Dilithium sign.c object references
 * this symbol, so provide a harmless implementation.
 */
#include <u.h>
#include <libc.h>
#include <stddef.h>
#include <stdint.h>

#include "randombytes.h"

int
randombytes(uint8_t *output, size_t n)
{
	int fd, r;

	fd = open("/dev/random", OREAD);
	if(fd < 0){
		memset(output, 0, n);
		return -1;
	}
	r = readn(fd, output, n);
	close(fd);
	if(r != (int)n){
		memset(output, 0, n);
		return -1;
	}
	return 0;
}
