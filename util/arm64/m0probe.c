#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define BASE 0x1000000000UL      /* 64 GiB: AArch64 ASan shadow offset */
#define LEN  0x1000000000UL      /* 64 GiB: shadow for a 512 GiB user VA */

int
main(void)
{
	void *p, *n;
	volatile unsigned char *s;
	unsigned long a;

	printf("M0 base=%#lx len=%#lx\n", (unsigned long)BASE, (unsigned long)LEN);

	p = mmap((void *)BASE, LEN, PROT_READ|PROT_WRITE,
		MAP_PRIVATE|MAP_ANON|MAP_FIXED, -1, 0);
	if(p == (void *)-1){
		perror("M0 mmap shadow");
		return 1;
	}
	if((unsigned long)p != BASE){
		printf("M0-MMAP WRONG %p\n", p);
		return 1;
	}
	printf("M0-MMAP OK %p\n", p);

	s = (volatile unsigned char *)p;
	s[0] = 0x5a;
	s[4096] = 0xa5;
	s[LEN/2] = 0x33;
	s[LEN-4096] = 0x7e;
	if(s[0] != 0x5a || s[4096] != 0xa5 ||
	   s[LEN/2] != 0x33 || s[LEN-4096] != 0x7e){
		printf("M0-TOUCH BAD\n");
		return 1;
	}
	printf("M0-TOUCH OK\n");

	if(mprotect((void *)BASE, 4096, PROT_NONE) < 0){
		perror("M0 mprotect none");
		return 1;
	}
	printf("M0-MPROTECT-NONE OK\n");
	if(mprotect((void *)BASE, 4096, PROT_READ|PROT_WRITE) < 0){
		perror("M0 mprotect rw");
		return 1;
	}
	printf("M0-MPROTECT-RW OK\n");

	n = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
		MAP_PRIVATE|MAP_ANON, -1, 0);
	if(n == (void *)-1){
		perror("M0 mmap normal");
		return 1;
	}
	a = (unsigned long)n;
	printf("M0-NORMAL-MAP %p\n", n);
	if(a >= BASE && a < BASE + LEN){
		printf("M0-NORMAL IN SHADOW\n");
		return 1;
	}
	*(volatile char *)n = 1;
	printf("M0-NORMAL OK\n");

	if(munmap((void *)BASE, LEN) < 0){
		perror("M0 munmap shadow");
		return 1;
	}
	printf("M0-MUNMAP OK\n");

	p = mmap((void *)BASE, 4096, PROT_READ|PROT_WRITE,
		MAP_PRIVATE|MAP_ANON|MAP_FIXED, -1, 0);
	if(p == (void *)-1 || (unsigned long)p != BASE){
		perror("M0 mmap remap");
		return 1;
	}
	printf("M0-REMAP OK\n");
	munmap(p, 4096);
	munmap(n, 4096);
	printf("M0-DONE\n");
	return 0;
}
