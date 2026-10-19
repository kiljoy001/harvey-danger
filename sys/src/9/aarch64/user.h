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
int arm64userload(u64, u8 *, u8 *);
void arm64runprocess(u64, u64, u64) __attribute__((noreturn));
void arm64runprocessargs(u64, u64, u64, u64, u64) __attribute__((noreturn));
u64 arm64userdiag(void);
u64 arm64userdiagdetails(void);
void arm64usertrap(Arm64ureg *);
void arm64syscall(Arm64ureg *);

#endif
