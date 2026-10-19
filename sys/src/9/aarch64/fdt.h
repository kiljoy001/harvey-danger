#ifndef HARVEY_ARM64_FDT_H
#define HARVEY_ARM64_FDT_H

#include <u.h>

typedef struct Fdtinfo Fdtinfo;
typedef struct Fdtreserve Fdtreserve;
typedef struct Fdtvirtio Fdtvirtio;
typedef struct Fdtmemory Fdtmemory;
typedef struct Fdtdev Fdtdev;
struct Fdtreserve {
	u64 base;
	u64 size;
};

/* Device classes discovered from board peripheral nodes. */
enum {
	Fdtclassnone = 0,
	Fdtclassuart,
	Fdtclassi2c,
	Fdtclassspi,
	Fdtclassgpio,
	Fdtclasspwm,
	Fdtclassgmac,
	Fdtclassmmc,
	Fdtclassusb,
	Fdtclasspcie,
	Fdtclasssaradc,
	Fdtclasstsadc,
	Fdtclasswdt,
	Fdtclassi2s,
	Fdtclasscru,
	Fdtclassgrf,
	Fdtclassnpu,
	Fdtclassgpu,
	Fdtclassvop,
	Fdtclasshdmi,
	Fdtclassdsi,
	Fdtclassedp,
	Fdtclasssfc,
	Fdtclassvpu,
};

enum {
	Maxfdtdev = 96,
	Maxfdtname = 32,
	Maxfdtcompat = 48,
};

struct Fdtdev {
	char name[Maxfdtname];
	char compat[Maxfdtcompat];
	u64 base;
	u64 size;
	u32 irq;	/* GIC interrupt ID (SPI+32, PPI+16); 0 when absent */
	u32 irqflags;	/* trigger flags from the first interrupt cell */
	u32 clas;
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
	u32 ndev;
	Fdtdev dev[Maxfdtdev];
};

int fdtparse(void *, usize, Fdtinfo *);
int fdtselectram(Fdtinfo *, u64, u64);
const char *fdtclassname(u32);
int arm64timerinit(Fdtinfo *);
void arm64timerstop(void);
int arm64timerwait(void);
int arm64irqmaskprobe(void);
u64 arm64timerdiag(void);
extern volatile u32 arm64ticks;
extern volatile u32 arm64el0irqs;

#endif
