#ifndef HARVEY_ARM64_PHYS_H
#define HARVEY_ARM64_PHYS_H

#include "fdt.h"

int arm64physinit(Fdtinfo *, usize, usize);
u64 arm64physpage(void);

#endif
