/* SPDX-License-Identifier: CC0-1.0 */
/*
 * randombytes for the trusted-application build: use the secure-mode
 * random number generator instead of a normal-world entropy source.
 */
#include <tee_internal_api.h>
#include <stddef.h>
#include <stdint.h>

#include "randombytes.h"

int
randombytes(uint8_t *output, size_t n)
{
	TEE_GenerateRandom(output, n);
	return 0;
}
