#ifndef HARVEY_ARM64_FDT_H
#define HARVEY_ARM64_FDT_H

#include <u.h>

typedef struct Fdtinfo Fdtinfo;
typedef struct Fdtreserve Fdtreserve;
typedef struct Fdtvirtio Fdtvirtio;
typedef struct Fdtmemory Fdtmemory;
struct Fdtreserve {
	u64 base;
	u64 size;
};

struct Fdtvirtio {
	u64 base;
	u64 size;
};

struct Fdtmemory {
	u64 base;
	u64 size;
};

enum {
	Maxfdtreserve = 32,
	Maxfdtvirtio = 32,
	Maxfdtmemory = 16,
	Fdtpl011 = 1,
	Fdtdwuart = 2,
};
struct Fdtinfo {
	u64 rambase;
	u64 ramsize;
	u64 uartbase;
	u32 uarttype;
	u32 gicversion;
	u64 gicdist;
	u64 giccpu;
	u32 timerirq;
	u32 nmemory;
	Fdtmemory memory[Maxfdtmemory];
	u32 nreserve;
	Fdtreserve reserve[Maxfdtreserve];
	u32 nvirtio;
	Fdtvirtio virtio[Maxfdtvirtio];
};

int fdtparse(void *, usize, Fdtinfo *);
int fdtselectram(Fdtinfo *, u64, u64);
int arm64timerinit(Fdtinfo *);
void arm64timerstop(void);
int arm64timerwait(void);
int arm64irqmaskprobe(void);
u64 arm64timerdiag(void);
extern volatile u32 arm64ticks;
extern volatile u32 arm64el0irqs;

#endif
