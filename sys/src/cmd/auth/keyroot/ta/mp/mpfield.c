/*
 * Minimal field-modulus hook.
 *
 * The 9front implementation accelerates a few named fields (Curve25519
 * and the NIST primes) with dedicated reduction code.  AuthPAK uses
 * Ed448, which is not one of them, so general modular reduction is
 * correct here.  Keeping the hook lets the generated SPAKE2 code call
 * mpfield() unchanged.
 */
#include <u.h>
#include <libc.h>
#include <mp.h>

mpint*
mpfield(mpint *N)
{
	return N;
}
