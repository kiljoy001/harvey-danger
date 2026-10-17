#ifndef HARVEY_ARM64_MMU_H
#define HARVEY_ARM64_MMU_H

#include "phys.h"

#define ARM64USERBASE 0x4000000000ULL

u64 arm64mmubuild(Fdtinfo *);
u64 arm64mmuuser(u64, u64, u64);
void arm64mmuon(u64);
void arm64mmuswitch(u64);
u64 arm64mmukernelroot(void);
int arm64mmuactive(void);
int arm64mmutranslate(u64, u64 *);

#endif
