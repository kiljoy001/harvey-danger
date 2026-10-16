/* Temporary identity map for QEMU virt.  Later process maps replace it. */
#include "mmu.h"

enum {
	Pagesz = 4096,
	L2blocksz = 2 * 1024 * 1024,
	L1blocksz = 1024 * 1024 * 1024,
};

static const u64 Maxidentity = 16ULL * 1024 * 1024 * 1024;

enum {
	Ptevalid = 1,
	Ptetable = 2,
	Pteattrdevice = 1 << 2,
	Pteinnershare = 3 << 8,
	Pteaccessed = 1 << 10,
};

u64
arm64mmuuser(u64 kernelrootpa, u64 codepa, u64 stackpa)
{
	u64 rootpa, l2pa, l3pa;
	u64 *kernelroot, *root, *l2, *l3;
	u32 index, i;

	if((kernelrootpa | codepa | stackpa) & (Pagesz-1))
		return 0;
	kernelroot = (u64 *)kernelrootpa;
	rootpa = arm64physpage();
	if(rootpa == 0)
		return 0;
	root = (u64 *)rootpa;
	for(i = 0; i < Pagesz/sizeof(u64); i++)
		root[i] = kernelroot[i];
	index = (ARM64USERBASE>>30) & 511;
	if(root[index] != 0)
		return 0;
	l2pa = arm64physpage();
	l3pa = arm64physpage();
	if(l2pa == 0 || l3pa == 0)
		return 0;
	l2 = (u64 *)l2pa;
	l3 = (u64 *)l3pa;
	root[index] = l2pa | Ptevalid | Ptetable;
	l2[0] = l3pa | Ptevalid | Ptetable;
	/* Code: EL0 read/execute, EL1 read-only and privileged execute-never. */
	l3[0] = codepa | Ptevalid | Ptetable | Pteaccessed |
	        Pteinnershare | (3ULL<<6) | (1ULL<<53);
	/* Stack: EL0 read/write, never executable. */
	l3[1] = stackpa | Ptevalid | Ptetable | Pteaccessed |
	        Pteinnershare | (1ULL<<6) | (1ULL<<53) | (1ULL<<54);
	return rootpa;
}

static u64
blockpte(u64 pa, int ram)
{
	u64 pte;

	pte = pa | Ptevalid | Pteaccessed | (1ULL<<54);
	if(ram)
		pte |= Pteinnershare;
	else
		pte |= Pteattrdevice | (1ULL<<53);
	return pte;
}

u64
arm64mmubuild(Fdtinfo *info)
{
	u64 rootpa, l2pa, ramend, n, pa, stop;
	u64 *root, *l2;
	u32 i, j;

	if(info->rambase == 0 || info->ramsize == 0 ||
	   (info->rambase | info->ramsize) & (L2blocksz-1))
		return 0;
	ramend = info->rambase + info->ramsize;
	if(ramend < info->rambase || ramend > Maxidentity ||
	   info->uartbase >= info->rambase || info->gicdist >= info->rambase ||
	   info->giccpu >= info->rambase)
		return 0;
	rootpa = arm64physpage();
	if(rootpa == 0)
		return 0;
	root = (u64 *)rootpa;
	n = (ramend+L1blocksz-1)/L1blocksz;
	for(i = 0; i < n; i++){
		l2pa = arm64physpage();
		if(l2pa == 0)
			return 0;
		l2 = (u64 *)l2pa;
		root[i] = l2pa | Ptevalid | Ptetable;
		stop = (u64)(i+1)*L1blocksz;
		if(stop > ramend)
			stop = ramend;
		for(j = 0; j < L1blocksz/L2blocksz; j++){
			pa = (u64)i*L1blocksz + (u64)j*L2blocksz;
			if(pa >= stop)
				break;
			l2[j] = blockpte(pa, pa >= info->rambase);
		}
	}
	return rootpa;
}

void
arm64mmuon(u64 rootpa)
{
	u64 tcr, mmfr, sctlr;

	/* 4 KiB granule, 39-bit lower VA, inner-shareable WB page walks. */
	__asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr));
	mmfr &= 15;
	if(mmfr > 5)
		mmfr = 5;
	tcr = (mmfr<<32) | (1ULL<<23) | (25ULL<<16) |
	      (3ULL<<12) | (1ULL<<10) | (1ULL<<8) | 25;
	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("msr mair_el1, %0" :: "r"((u64)0x04ff) : "memory");
	__asm__ volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");
	__asm__ volatile("msr ttbr0_el1, %0" :: "r"(rootpa) : "memory");
	__asm__ volatile("isb; tlbi vmalle1; dsb sy; isb" ::: "memory");
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr |= (1ULL<<12) | (1ULL<<2) | 1;
	__asm__ volatile("msr sctlr_el1, %0; isb" :: "r"(sctlr) : "memory");
}

void
arm64mmuswitch(u64 rootpa)
{
	__asm__ volatile("dsb ishst; msr ttbr0_el1, %0; isb; "
	                 "tlbi vmalle1; dsb ish; isb"
	                 :: "r"(rootpa) : "memory");
}

int
arm64mmuactive(void)
{
	u64 sctlr;

	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	return (sctlr & ((1ULL<<12) | (1ULL<<2) | 1)) ==
	       ((1ULL<<12) | (1ULL<<2) | 1);
}

int
arm64mmutranslate(u64 va, u64 *pa)
{
	u64 par;

	__asm__ volatile("at s1e1r, %1; isb; mrs %0, par_el1"
	                 : "=r"(par) : "r"(va) : "memory");
	if(par & 1)
		return -1;
	*pa = (par & 0x0000fffffffff000ULL) | (va & 4095);
	return 0;
}
