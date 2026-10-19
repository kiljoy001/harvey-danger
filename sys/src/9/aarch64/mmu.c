/* Temporary identity map for RAM and device-tree-discovered MMIO. */
#include "mmu.h"
#ifdef ARM64_FULL_KERNEL
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#endif

enum {
	Pagesz = 4096,
	L2blocksz = 2 * 1024 * 1024,
	L1blocksz = 1024 * 1024 * 1024,
};

static const u64 Maxidentity = 16ULL * 1024 * 1024 * 1024;
static u64 kernelroot;

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

int
arm64mmuuserdevice(u64 rootpa, u64 va, u64 pa)
{
	u64 *root, *l2, *l3;
	u64 entry;
	u32 i1, i2, i3;

	if((rootpa|va|pa) & (Pagesz-1) || va >= (1ULL<<39))
		return -1;
	root = (u64 *)rootpa;
	i1 = (va>>30)&511;
	i2 = (va>>21)&511;
	i3 = (va>>12)&511;
	entry = root[i1];
	if((entry & (Ptevalid|Ptetable)) != (Ptevalid|Ptetable))
		return -1;
	l2 = (u64 *)(entry & ~(u64)(Pagesz-1));
	entry = l2[i2];
	if((entry & (Ptevalid|Ptetable)) != (Ptevalid|Ptetable))
		return -1;
	l3 = (u64 *)(entry & ~(u64)(Pagesz-1));
	if(l3[i3] != 0)
		return -1;
	/* EL0 read/write, Device-nGnRE, never executable at either EL. */
	l3[i3] = pa | Ptevalid | Ptetable | Pteattrdevice | Pteaccessed |
	           (1ULL<<6) | (1ULL<<53) | (1ULL<<54);
	return 0;
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
	u64 rootpa, l2pa, ramend, pa, start, end;
	u64 *root, *l2;
	u32 i, j, k, regions;
	Fdtvirtio device[Maxfdtvirtio+7];

	if(info->rambase == 0 || info->ramsize == 0 ||
	   (info->rambase | info->ramsize) & (L2blocksz-1))
		return 0;
	ramend = info->rambase + info->ramsize;
	if(ramend < info->rambase || ramend > Maxidentity ||
	   info->nvirtio > Maxfdtvirtio)
		return 0;
	rootpa = arm64physpage();
	if(rootpa == 0)
		return 0;
	root = (u64 *)rootpa;
	for(pa = info->rambase; pa < ramend; pa += L2blocksz){
		i = pa/L1blocksz;
		j = (pa%L1blocksz)/L2blocksz;
		if(root[i] == 0){
			l2pa = arm64physpage();
			if(l2pa == 0)
				return 0;
			root[i] = l2pa | Ptevalid | Ptetable;
		}
		l2 = (u64 *)(root[i] & ~(u64)(Pagesz-1));
		l2[j] = blockpte(pa, 1);
	}
	device[0].base = info->uartbase;
	device[0].size = Pagesz;
	device[1].base = info->gicdist;
	device[1].size = 0x10000;
	device[2].base = info->giccpu;
	device[2].size = 0x20000;
	regions = 3;
	for(k = 0; k < info->nvirtio; k++)
		device[regions++] = info->virtio[k];
#ifdef ARM64_RK3566_NPU_PROBE
	/* Temporary register probe; a user-space driver will own these later. */
	device[regions++] = (Fdtvirtio){0xfdd90000ULL, Pagesz};
	device[regions++] = (Fdtvirtio){0xfdd20000ULL, Pagesz};
	device[regions++] = (Fdtvirtio){0xfde40000ULL, Pagesz};
	device[regions++] = (Fdtvirtio){0xfde4b000ULL, Pagesz};
#endif
	for(k = 0; k < regions; k++){
		pa = device[k].base;
		if(pa == 0 || device[k].size == 0 ||
		   pa >= Maxidentity || device[k].size > Maxidentity-pa ||
		   (pa < ramend && pa+device[k].size > info->rambase))
			return 0;
		start = pa & ~(u64)(L2blocksz-1);
		end = (pa+device[k].size+L2blocksz-1) & ~(u64)(L2blocksz-1);
		if(end > Maxidentity)
			return 0;
		for(pa = start; pa < end; pa += L2blocksz){
			i = pa/L1blocksz;
			j = (pa%L1blocksz)/L2blocksz;
			if(root[i] == 0){
				l2pa = arm64physpage();
				if(l2pa == 0)
					return 0;
				root[i] = l2pa | Ptevalid | Ptetable;
			}
			l2 = (u64 *)(root[i] & ~(u64)(Pagesz-1));
			l2[j] = blockpte(pa, 0);
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
	kernelroot = rootpa;
	__asm__ volatile("isb; tlbi vmalle1; dsb sy; isb" ::: "memory");
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr |= (1ULL<<12) | (1ULL<<2) | 1;
	__asm__ volatile("msr sctlr_el1, %0; isb" :: "r"(sctlr) : "memory");
}

u64
arm64mmukernelroot(void)
{
	return kernelroot;
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

#ifdef ARM64_FULL_KERNEL
struct Ptpage {
	Ptpage *next;
	u64 pa;
};

static u64
ptalloc(Proc *p)
{
	Ptpage *node;
	u64 pa;
	int color;

	color = 0;
	pa = physalloc(PGSZ, &color, p);
	if(pa == 0)
		panic("ARM64 page-table memory exhausted");
	node = malloc(sizeof(*node));
	if(node == nil){
		physfree(pa, PGSZ);
		panic("ARM64 page-table metadata exhausted");
	}
	memset(KADDR(pa), 0, PGSZ);
	node->pa = pa;
	node->next = p->MMU.ptpages;
	p->MMU.ptpages = node;
	return pa;
}

void
mmuput(usize va, Page *page, u32 attr)
{
	Proc *p;
	u64 *root, *l2, *l3, *kernel, pa, entry;
	u32 i1, i2, i3, size;

	p = externup();
	if(p == nil || page == nil || va >= USTKTOP ||
	   page->pgszi < 0 || page->pgszi >= NPGSZ)
		panic("ARM64 mmuput: invalid process or address");
	size = sys->pgsz[page->pgszi];
	if(size != PGSZ && size != BIGPGSZ)
		panic("ARM64 mmuput: unsupported page size");
	if((va | page->pa) & (size - 1))
		panic("ARM64 mmuput: unaligned mapping");
	kernel = (u64 *)arm64mmukernelroot();
	if(kernel == nil)
		panic("ARM64 mmuput: no kernel root");
	if(p->MMU.rootpa == 0){
		pa = ptalloc(p);
		memmove(KADDR(pa), kernel, PGSZ);
		p->MMU.rootpa = pa;
	}
	root = (u64 *)p->MMU.rootpa;
	i1 = (va >> 30) & 511;
	i2 = (va >> 21) & 511;
	i3 = (va >> 12) & 511;
	if(root[i1] == kernel[i1]){
		pa = ptalloc(p);
		if(root[i1] & Ptevalid)
			memmove(KADDR(pa), KADDR(root[i1] & ~(u64)(PGSZ - 1)), PGSZ);
		root[i1] = pa | Ptevalid | Ptetable;
	}
	l2 = (u64 *)(root[i1] & ~(u64)(PGSZ - 1));
	if(size == PGSZ){
		if((l2[i2] & (Ptevalid | Ptetable)) != (Ptevalid | Ptetable)){
			pa = ptalloc(p);
			l2[i2] = pa | Ptevalid | Ptetable;
		}
		l3 = (u64 *)(l2[i2] & ~(u64)(PGSZ - 1));
	}
	entry = 0;
	if(attr & PTEVALID){
		entry = page->pa | Ptevalid | Pteaccessed | Pteinnershare |
			(1ULL << 53);
		if(size == PGSZ)
			entry |= Ptetable;
		if(attr & PTEWRITE)
			entry |= 1ULL << 6;
		else
			entry |= 3ULL << 6;
		if(attr & PTENOEXEC)
			entry |= 1ULL << 54;
		if(attr & PTEUNCACHED)
			entry |= Pteattrdevice;
	}
	if(size == PGSZ)
		l3[i3] = entry;
	else
		l2[i2] = entry;
	__asm__ volatile("dsb ishst" ::: "memory");
	if(machp()->proc == p)
		arm64mmuswitch(p->MMU.rootpa);
}

void
arm64mmurelease(Proc *p)
{
	Ptpage *node, *next;

	if(p->MMU.rootpa == 0)
		return;
	if(machp()->proc == p)
		arm64mmuswitch(arm64mmukernelroot());
	for(node = p->MMU.ptpages; node != nil; node = next){
		next = node->next;
		physfree(node->pa, PGSZ);
		free(node);
	}
	p->MMU.ptpages = nil;
	p->MMU.rootpa = 0;
}
#endif
