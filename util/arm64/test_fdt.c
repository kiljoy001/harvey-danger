/* Host-side malformed-DTB checks for the freestanding ARM64 parser. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fdt.h"

enum { Maxdtb = 16 * 1024 * 1024 };

static void
put4(u8 *p, u32 n)
{
	p[0] = n >> 24;
	p[1] = n >> 16;
	p[2] = n >> 8;
	p[3] = n;
}

static u32
get4(u8 *p)
{
	return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3];
}

static void
reject(u8 *p, usize n, char *reason)
{
	Fdtinfo info;

	if(fdtparse(p, n, &info) == 0){
		fprintf(stderr, "accepted malformed DTB: %s\n", reason);
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	FILE *f;
	u8 *original, *work;
	usize n;
	u32 structure, strings, strsize, i, x;
	Fdtinfo info;

	if(argc != 2 || (f = fopen(argv[1], "rb")) == nil)
		return 2;
	original = malloc(Maxdtb);
	work = malloc(Maxdtb);
	if(original == nil || work == nil)
		return 2;
	n = fread(original, 1, Maxdtb, f);
	fclose(f);
	if(n < 80 || fdtparse(original, n, &info) < 0 ||
	   info.rambase != 0x40000000 || info.ramsize != 0x20000000 ||
	   info.uartbase != 0x09000000 || info.gicdist != 0x08000000 ||
	   info.giccpu != 0x08010000 || info.timerirq != 27)
		return 1;
	structure = get4(original+8);
	strings = get4(original+12);
	strsize = get4(original+32);
	if(structure+24 >= n || strings >= n || strsize > n-strings)
		return 1;
	reject(original, 39, "short header");
	reject(original, n-1, "truncated blob");
	memcpy(work, original, n);
	put4(work, 0);
	reject(work, n, "bad magic");
	memcpy(work, original, n);
	put4(work+4, 0xffffffff);
	reject(work, n, "oversized blob");
	memcpy(work, original, n);
	put4(work+8, n-2);
	reject(work, n, "structure outside blob");
	memcpy(work, original, n);
	put4(work+12, n-2);
	reject(work, n, "strings outside blob");
	memcpy(work, original, n);
	put4(work+36, 4);
	reject(work, n, "truncated structure");
	memcpy(work, original, n);
	put4(work+structure, 0xffffffff);
	reject(work, n, "unknown token");
	memcpy(work, original, n);
	put4(work+structure+16, strsize);
	reject(work, n, "property name outside strings");
	/* Mutate the real structure; the parser may accept or reject, but never overread. */
	x = 0x9e3779b9;
	for(i = 0; i < 2000; i++){
		memcpy(work, original, n);
		x = x*1664525+1013904223;
		work[structure + x%get4(original+36)] ^= (u8)(x >> 16) | 1;
		fdtparse(work, n, &info);
	}
	free(work);
	free(original);
	puts("FDT parser valid, malformed, and mutation checks PASS");
	return 0;
}
