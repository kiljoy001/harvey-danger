/* Monotonic early physical-page allocator, before the MMU and page tables. */
#include "phys.h"

enum { Pagesz = 4096 };

extern char _start[], __image_end[];

static u64 nextpage, ramend;
static Fdtreserve excluded[Maxfdtreserve+1];
static u32 nexcluded;

static u64
pageup(u64 n)
{
	if(n > ~(u64)0 - (Pagesz-1))
		return 0;
	return (n + Pagesz-1) & ~(u64)(Pagesz-1);
}

int
arm64physinit(Fdtinfo *info, usize dtb, usize dtbsize)
{
	u32 i;
	u64 start;

	if(info->ramsize == 0 || info->rambase+info->ramsize < info->rambase ||
	   dtb+dtbsize < dtb || info->nreserve > Maxfdtreserve)
		return -1;
	ramend = info->rambase + info->ramsize;
	start = (u64)__image_end;
	if((u64)_start < info->rambase || start > ramend ||
	   (nextpage = pageup(start)) == 0 || nextpage > ramend)
		return -1;
	nexcluded = info->nreserve+1;
	for(i = 0; i < info->nreserve; i++)
		excluded[i] = info->reserve[i];
	excluded[info->nreserve].base = dtb;
	excluded[info->nreserve].size = dtbsize;
	return 0;
}

u64
arm64physpage(void)
{
	u64 p, end;
	u32 i, j;
	u64 *word;

	p = nextpage;
	for(;;){
		if(p == 0 || p > ramend || ramend-p < Pagesz)
			return 0;
		for(i = 0; i < nexcluded; i++){
			end = excluded[i].base + excluded[i].size;
			if(p < end && p+Pagesz > excluded[i].base){
				p = pageup(end);
				break;
			}
		}
		if(i == nexcluded)
			break;
	}
	nextpage = p+Pagesz;
	word = (u64 *)p;
	for(j = 0; j < Pagesz/sizeof(u64); j++)
		word[j] = 0;
	return p;
}
