/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * ARMv7 has no integer divide instruction (Cortex-A7 included), and the
 * freestanding link has no compiler runtime, so supply the EABI helpers
 * clang emits for "/" and "%".  Small structs are returned in r0/r1 and
 * r0-r3 under AAPCS, which is exactly what __aeabi_*mod requires.
 */

typedef unsigned int u32;
typedef int i32;
typedef unsigned long long u64;
typedef long long i64;

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

static u64
udivmod64(u64 n, u64 d, u64 *rp)
{
	u64 q, r;
	int i;

	q = 0;
	r = 0;
	if(d == 0){
		if(rp != 0)
			*rp = 0;
		return 0;
	}
	for(i = 63; i >= 0; i--){
		r = (r << 1) | ((n >> i) & 1);
		if(r >= d){
			r -= d;
			q |= 1ULL << i;
		}
	}
	if(rp != 0)
		*rp = r;
	return q;
}

struct u64pair {
	u64 quot;
	u64 rem;
};

struct u64pair
__aeabi_uldivmod(u64 n, u64 d)
{
	struct u64pair r;

	r.quot = udivmod64(n, d, &r.rem);
	return r;
}

struct i64pair {
	i64 quot;
	i64 rem;
};

struct i64pair
__aeabi_ldivmod(i64 n, i64 d)
{
	struct i64pair r;
	u64 rem;
	int neg;

	neg = n < 0;
	if(n < 0)
		n = -n;
	if(d < 0)
		d = -d;
	r.quot = (i64)udivmod64((u64)n, (u64)d, &rem);
	r.rem = (i64)rem;
	if(neg){
		r.quot = -r.quot;
		r.rem = -r.rem;
	}
	return r;
}
