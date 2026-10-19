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
put8(u8 *p, u64 n)
{
	put4(p, n>>32);
	put4(p+4, n);
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

static int
finddev(Fdtinfo *info, u32 clas)
{
	u32 i;

	for(i = 0; i < info->ndev; i++)
		if(info->dev[i].clas == clas)
			return (int)i;
	return -1;
}

int
main(int argc, char **argv)
{
	FILE *f;
	u8 *original, *work;
	usize n;
	u32 structure, strings, strsize, i, x;
	Fdtinfo info;

	if((argc != 2 && argc != 3) || (f = fopen(argv[1], "rb")) == nil)
		return 2;
	original = malloc(Maxdtb);
	work = malloc(Maxdtb);
	if(original == nil || work == nil)
		return 2;
	n = fread(original, 1, Maxdtb, f);
	fclose(f);
	if(n < 80 || fdtparse(original, n, &info) < 0)
		return 1;
	if(argc == 3){
		if(strcmp(argv[2], "board") != 0 ||
		   info.nmemory != 2 || info.memory[0].base != 0x20000000 ||
		   info.memory[0].size != 0x10000000 ||
		   info.memory[1].base != 0x40000000 ||
		   info.memory[1].size != 0x80000000 ||
		   info.uartbase != 0xfe660000 || info.uarttype != Fdtdwuart ||
		   info.gicdist != 0xfd400000 || info.giccpu != 0xfd460000 ||
		   info.gicversion != 3 || info.timerirq != 27 ||
		   fdtselectram(&info, 0x40080000, 0x40400000) < 0 ||
		   info.rambase != 0x40000000 || info.ramsize != 0x80000000 ||
		   fdtselectram(&info, 0x30000000, 0x40000000) == 0){
			fprintf(stderr, "board: memory %u selected %#llx/%#llx uart %#llx/%u gic %#llx/%#llx/%u timer %u\n",
			        info.nmemory,
			        (unsigned long long)info.rambase,
			        (unsigned long long)info.ramsize,
			        (unsigned long long)info.uartbase, info.uarttype,
			        (unsigned long long)info.gicdist,
			        (unsigned long long)info.giccpu,
			        info.gicversion, info.timerirq);
			return 1;
		}
		{
			int i2c, spi, gpio, pwm, gmac, mmc, uart;

			i2c = finddev(&info, Fdtclassi2c);
			spi = finddev(&info, Fdtclassspi);
			gpio = finddev(&info, Fdtclassgpio);
			pwm = finddev(&info, Fdtclasspwm);
			gmac = finddev(&info, Fdtclassgmac);
			mmc = finddev(&info, Fdtclassmmc);
			uart = finddev(&info, Fdtclassuart);
			if(info.ndev != 7 || i2c < 0 || spi < 0 || gpio < 0 ||
			   pwm < 0 || gmac < 0 || mmc < 0 || uart < 0 ||
			   info.dev[i2c].base != 0xfe5a0000ULL ||
			   info.dev[i2c].size != 0x1000 ||
			   info.dev[i2c].irq != 100 ||
			   info.dev[spi].base != 0xfe610000ULL ||
			   info.dev[spi].irq != 113 ||
			   info.dev[gpio].base != 0xfdd60000ULL ||
			   info.dev[gpio].irq != 117 ||
			   info.dev[pwm].base != 0xfdd70000ULL ||
			   info.dev[pwm].irq != 0 ||
			   info.dev[gmac].base != 0xfe010000ULL ||
			   info.dev[gmac].irq != 64 ||
			   info.dev[mmc].base != 0xfe310000ULL ||
			   info.dev[mmc].irq != 78 ||
			   info.dev[uart].base != 0xfe660000ULL ||
			   strcmp(info.dev[i2c].name, "i2c@fe5a0000") != 0 ||
			   strcmp(info.dev[gmac].compat, "rockchip,rk3568-gmac") != 0){
				fprintf(stderr, "board: device discovery mismatch (ndev %u)\n",
				        info.ndev);
				return 1;
			}
		}
		free(work);
		free(original);
		puts("FDT alternate board discovery PASS");
		return 0;
	}else if(info.rambase != 0x40000000 || info.ramsize != 0x20000000 ||
	         info.uartbase != 0x09000000 || info.uarttype != Fdtpl011 ||
	         info.gicdist != 0x08000000 || info.giccpu != 0x08010000 ||
	         info.gicversion != 2 || info.timerirq != 27 ||
	         info.nvirtio != 32 || info.virtio[0].size != 0x200 ||
	         fdtselectram(&info, 0x40080000, 0x40400000) < 0 ||
	         info.rambase != 0x40000000 || info.ramsize != 0x20000000)
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
	memcpy(work, original, n);
	put4(work+16, 0x31);
	reject(work, n, "unaligned reservation map");
	/* Insert one firmware reservation before the structure block. */
	memcpy(work, original, n);
	memmove(work+structure+16, work+structure, n-structure-16);
	put4(work+8, structure+16);
	put4(work+12, strings+16);
	put8(work+structure-16, 0x40090000);
	put8(work+structure-8, 0x1000);
	memset(work+structure, 0, 16);
	if(fdtparse(work, n, &info) < 0 || info.nreserve != 1 ||
	   info.reserve[0].base != 0x40090000 || info.reserve[0].size != 0x1000)
		return 1;
	put8(work+structure-16, ~(u64)0);
	put8(work+structure-8, 0x1000);
	reject(work, n, "overflowing reservation");
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
