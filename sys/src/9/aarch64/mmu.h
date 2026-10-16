#ifndef HARVEY_ARM64_MMU_H
#define HARVEY_ARM64_MMU_H

#include "phys.h"

u64 arm64mmubuild(Fdtinfo *);
void arm64mmuon(u64);
int arm64mmuactive(void);
int arm64mmutranslate(u64, u64 *);

#endif
