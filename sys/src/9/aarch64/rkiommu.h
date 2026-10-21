#ifndef HARVEY_ARM64_RKIOMMU_H
#define HARVEY_ARM64_RKIOMMU_H

#include <u.h>
#include "fdt.h"

/*
 * Rockchip IOMMU v2 (RK3568).
 *
 * Two-level table:
 *   1024 directory entries -> 1024 page-table entries -> 4 KiB pages
 * This covers 4 GiB of IOVA.  PTEs carry a 40-bit physical address, so
 * pages from the RK3566's high DRAM bank can be mapped into the VOP's
 * 32-bit IOVA space.
 */

typedef struct Rkdomain Rkdomain;
typedef struct Rkiommu Rkiommu;

enum {
	RkiommuDtes = 1024,
	RkiommuPtes = 1024,
	RkiommuPagesz = 4096,
	RkiommuDteshift = 22,
	RkiommuPteshift = 12,
	RkiommuDtemask = 0x3ff,
	RkiommuPtemask = 0x3ff,
	Maxrkiommu = 8,
};

struct Rkdomain {
	Lock l;
	u32 *dt;
	u64 dtpa;
	u32 *pt[RkiommuDtes];
	u64 ptpa[RkiommuDtes];
};

struct Rkiommu {
	char name[32];
	u64 base[2];
	u32 size[2];
	int nbase;
	u32 irq;
	Rkdomain *dom;
	int enabled;
};

Rkiommu *rkiommuinit(Fdtinfo *);
Rkiommu *rkiommubybase(u64);
Rkdomain *rkiommudomain(void);
void rkiommudomainfree(Rkdomain *);
int rkiommuattach(Rkiommu *, Rkdomain *);
int rkiommuenable(Rkiommu *);
int rkiommudisable(Rkiommu *);
int rkiommumap(Rkiommu *, u64 iova, u64 pa, usize size, int writable);
int rkiommuunmap(Rkiommu *, u64 iova, usize size);
void rkiommuzap(Rkiommu *, u64 iova, usize size);
void rkiommufault(Rkiommu *);
int rkiommuptest(void);

#endif
