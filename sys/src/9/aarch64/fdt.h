#ifndef HARVEY_ARM64_FDT_H
#define HARVEY_ARM64_FDT_H

#include <u.h>

typedef struct Fdtinfo Fdtinfo;
typedef struct Fdtreserve Fdtreserve;
typedef struct Fdtvirtio Fdtvirtio;
struct Fdtreserve {
	u64 base;
	u64 size;
};

struct Fdtvirtio {
	u64 base;
	u64 size;
};

enum { Maxfdtreserve = 32, Maxfdtvirtio = 32 };
struct Fdtinfo {
	u64 rambase;
	u64 ramsize;
	u64 uartbase;
	u64 gicdist;
	u64 giccpu;
	u32 timerirq;
	u32 nreserve;
	Fdtreserve reserve[Maxfdtreserve];
	u32 nvirtio;
	Fdtvirtio virtio[Maxfdtvirtio];
};

int fdtparse(void *, usize, Fdtinfo *);
int arm64timerinit(Fdtinfo *);
void arm64timerstop(void);
int arm64timerwait(void);
int arm64irqmaskprobe(void);
u64 arm64timerdiag(void);
extern volatile u32 arm64ticks;
extern volatile u32 arm64el0irqs;

#endif
