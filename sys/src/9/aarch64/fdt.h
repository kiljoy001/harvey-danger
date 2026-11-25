#ifndef HARVEY_ARM64_FDT_H
#define HARVEY_ARM64_FDT_H

#include <u.h>

typedef struct Fdtinfo Fdtinfo;
typedef struct Fdtreserve Fdtreserve;
typedef struct Fdtvirtio Fdtvirtio;
typedef struct Fdtmemory Fdtmemory;
typedef struct Fdtdev Fdtdev;
enum {
	Maxfdtdev = 96,
	Maxfdtname = 32,
	Maxfdtcompat = 48,
};

enum {
	Fdtboardunknown,
	Fdtboardqemuvirt,
	Fdtboardrk3566,
	Fdtboardrk3568,
	Fdtboardrk3588,
	Fdtboardbengal,
	Fdtboardsun50ih6,
};

struct Fdtreserve {
	char name[Maxfdtname];
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
	Fdtclassrng,
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
	Fdtclassiommu,
	Fdtclasspower,
	Fdtclassrtc,
};

enum { Maxfdtwin = 4 };		/* MMIO windows a node may describe */

struct Fdtdev {
	char name[Maxfdtname];
	char compat[Maxfdtcompat];
	char group[Maxfdtname];	/* harvey,group: devices granted together */
	u64 base;	/* first window */
	u64 size;
	u32 nwin;	/* windows in the node's reg, 1..Maxfdtwin */
	u64 wbase[Maxfdtwin-1];	/* windows 1..nwin-1 */
	u64 wsize[Maxfdtwin-1];
	u32 irq;	/* GIC interrupt ID (SPI+32, PPI+16); 0 when absent */
	u32 irqflags;	/* trigger flags from the first interrupt cell */
	u32 irq2;	/* second GIC interrupt ID; 0 when absent */
	u32 clas;
	u16 depth;	/* device-tree depth, for the parent pass */
	u32 parent;	/* nearest enclosing device in dev[], ~0 when none */
};

/*
 * Window i of a device.  Window 0 is base/size, the one #P names as
 * reg/size and a claim maps by default; a compound node's further
 * ranges are listed in #P/<node>/windows.
 */
static inline void
fdtdevwin(Fdtdev *d, u32 i, u64 *base, u64 *size)
{
	if(i == 0){
		*base = d->base;
		*size = d->size;
	}else{
		*base = d->wbase[i-1];
		*size = d->wsize[i-1];
	}
}
u32 fdtdevmap(Fdtdev *, u64 *, u64 *);

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
	Fdtmaxcpu = 16,
	Fdtpl011 = 1,
	Fdtdwuart = 2,
};
struct Fdtinfo {
	u64 rambase;
	u64 ramsize;
	u64 lklarena;	/* /chosen harvey,lkl-arena-size, 0 if absent */
	u32 board;		/* Fdtboard* */
	char model[96];		/* root model string */
	char rootcompat[160];	/* root compatible strings, space-separated */
	u64 bootarchbase;	/* /chosen harvey,bootarchive */
	u64 bootarchsize;
	u64 teeshmbase;		/* /chosen harvey,teeshm */
	u64 teeshmsize;
	u64 scmbase;		/* /chosen harvey,scm */
	u64 scmsize;
	u64 fixrambase;		/* /chosen harvey,ram, used when /memory is absent */
	u64 fixramsize;
	u64 fbbase;		/* simple-framebuffer */
	u64 fbsize;
	u32 fbwidth;
	u32 fbheight;
	u32 fbstride;
	u32 fbbpp;
	u64 uartbase;
	u32 uarttype;
	u32 gicversion;
	u64 gicdist;
	u64 giccpu;
	u64 giccpusize;
	u32 timerirq;
	u32 ncpu;
	u32 psci_method;	/* 1 = HVC, 2 = SMC */
	u32 psci_cpuon;
	u32 psci_std;		/* 0.2+/1.0 standard function ids */
	u64 cpumpidr[Fdtmaxcpu];
	u32 nmemory;
	Fdtmemory memory[Maxfdtmemory];
	u32 nusable;		/* memory banks, image bank first, 2 MiB aligned */
	Fdtmemory usable[Maxfdtmemory];
	u64 ramtotal;		/* sum of usable bank sizes */
	u32 nreserve;
	Fdtreserve reserve[Maxfdtreserve];
	u32 nvirtio;
	Fdtvirtio virtio[Maxfdtvirtio];
	u32 ndev;
	Fdtdev dev[Maxfdtdev];
};

int fdtparse(void *, usize, Fdtinfo *);
int fdtselectram(Fdtinfo *, u64, u64);
int fdtiscompat(Fdtinfo *, char *);
int fdtisrockchip(Fdtinfo *);
int fdtinram(Fdtinfo *, u64, u64);
Fdtdev *fdtdev(Fdtinfo *, u32);
Fdtdev *fdtdevcompat(Fdtinfo *, char *);
Fdtdev *fdtdevcompatn(Fdtinfo *, char *, u32);
Fdtdev *fdtdevname(Fdtinfo *, char *);
const char *fdtboardname(u32);
const char *fdtclassname(u32);
int arm64timerinit(Fdtinfo *);
int qcsmmuinit(Fdtinfo *);
void arm64timerstop(void);
int arm64timerwait(void);
void fb_stripe(int, u32);
void fb_draw_hex(int, int, u64, u32);
int arm64irqmaskprobe(void);
int arm64irqenable(u32);
void arm64irqedge(u32);
void arm64irqdisable(u32, int);
int arm64irqwait(u32, int);
u64 arm64timerdiag(void);
extern volatile u32 arm64ticks;
extern volatile u32 arm64el0irqs;

#endif
