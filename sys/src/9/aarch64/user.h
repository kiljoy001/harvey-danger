#ifndef HARVEY_ARM64_USER_H
#define HARVEY_ARM64_USER_H

#include "mmu.h"

typedef struct Arm64ureg Arm64ureg;
struct Arm64ureg {
	u64 x[32];
	u64 elr;
	u64 spsr;
	u64 esr;
	u64 far;
	u64 sp0;
};

int arm64userprobe(u64, u64, u64);
u64 arm64userdiag(void);
u64 arm64userdiagdetails(void);
void arm64usertrap(Arm64ureg *);

#endif
