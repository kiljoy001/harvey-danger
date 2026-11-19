/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in the top-level directory of this
 * distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * ARMv7 has no integer divide instruction (Cortex-A7 included), and the
 * freestanding link has no compiler runtime, so supply the EABI helpers
 * clang emits.
 *
 * The 32-bit *mod helpers return a two-word struct, which AAPCS passes
 * back in r0/r1.  The 64-bit ones cannot be written this way (a 16-byte
 * result uses a hidden pointer), so __aeabi_uldivmod and
 * __aeabi_ldivmod live in l.S.
 */

typedef unsigned int u32;
typedef int i32;

static u32
udivmod(u32 n, u32 d, u32 *rp)
{
	u32 q, r;
	int i;

	q = 0;
	r = 0;
	if(d == 0){
		if(rp != 0)
			*rp = 0;
		return 0;
	}
	for(i = 31; i >= 0; i--){
		r = (r << 1) | ((n >> i) & 1);
		if(r >= d){
			r -= d;
			q |= 1u << i;
		}
	}
	if(rp != 0)
		*rp = r;
	return q;
}

u32
__aeabi_uidiv(u32 n, u32 d)
{
	return udivmod(n, d, 0);
}

struct u32pair {
	u32 quot;
	u32 rem;
};

struct u32pair
__aeabi_uidivmod(u32 n, u32 d)
{
	struct u32pair r;

	r.quot = udivmod(n, d, &r.rem);
	return r;
}

i32
__aeabi_idiv(i32 n, i32 d)
{
	int neg;

	neg = 0;
	if(n < 0){
		n = -n;
		neg = !neg;
	}
	if(d < 0){
		d = -d;
		neg = !neg;
	}
	n = (i32)udivmod((u32)n, (u32)d, 0);
	return neg ? -n : n;
}

struct i32pair {
	i32 quot;
	i32 rem;
};

struct i32pair
__aeabi_idivmod(i32 n, i32 d)
{
	struct i32pair r;
	u32 rem;
	int neg;

	neg = n < 0;
	if(n < 0)
		n = -n;
	if(d < 0)
		d = -d;
	r.quot = (i32)udivmod((u32)n, (u32)d, &rem);
	r.rem = (i32)rem;
	if(neg){
		r.quot = -r.quot;
		r.rem = -r.rem;
	}
	return r;
}

/*
 * Clang emits these for aligned copies on ARM EABI targets.
 */
void *
__aeabi_memcpy4(void *d, const void *s, unsigned int n)
{
	unsigned char *dp = d;
	const unsigned char *sp = s;

	while(n-- > 0)
		*dp++ = *sp++;
	return d;
}

void *
__aeabi_memcpy8(void *d, const void *s, unsigned int n)
{
	return __aeabi_memcpy4(d, s, n);
}

void *
__aeabi_memmove4(void *d, const void *s, unsigned int n)
{
	unsigned char *dp = d;
	const unsigned char *sp = s;

	if(dp < sp){
		while(n-- > 0)
			*dp++ = *sp++;
	}else{
		dp += n;
		sp += n;
		while(n-- > 0)
			*--dp = *--sp;
	}
	return d;
}

void *
__aeabi_memmove8(void *d, const void *s, unsigned int n)
{
	return __aeabi_memmove4(d, s, n);
}

void *
__aeabi_memset4(void *d, unsigned int n, int c)
{
	unsigned char *dp = d;

	while(n-- > 0)
		*dp++ = c;
	return d;
}

void *
__aeabi_memset8(void *d, unsigned int n, int c)
{
	return __aeabi_memset4(d, n, c);
}

void *
__aeabi_memclr4(void *d, unsigned int n)
{
	return __aeabi_memset4(d, n, 0);
}

void *
__aeabi_memclr8(void *d, unsigned int n)
{
	return __aeabi_memset4(d, n, 0);
}
