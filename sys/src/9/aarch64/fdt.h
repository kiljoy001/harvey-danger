#ifndef HARVEY_ARM64_FDT_H
#define HARVEY_ARM64_FDT_H

#include <u.h>

typedef struct Fdtinfo Fdtinfo;
struct Fdtinfo {
	u64 rambase;
	u64 ramsize;
	u64 uartbase;
	u64 gicdist;
	u64 giccpu;
	u32 timerirq;
};

int fdtparse(void *, usize, Fdtinfo *);
int arm64timerinit(Fdtinfo *);
void arm64timerstop(void);
int arm64timerwait(void);
u64 arm64timerdiag(void);
extern volatile u32 arm64ticks;

#endif
