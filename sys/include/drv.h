/*
 * drv.h - the userspace driver kit.
 *
 * A driver owns a device through #P: it finds the node, maps its registers,
 * waits for its interrupt, allocates DMA from the kernel's shared region, and
 * publishes a 9P service.  This header is the whole of that boilerplate;
 * the driver keeps its register logic and its file semantics.
 *
 *	Drv uart;
 *	DrvFile files[] = { ... };
 *
 *	if(drvfind(&uart, "uart", 1) < 0 || drvmap(&uart) < 0)
 *		drvfail("uartfs: no uart in #P");
 *	drvsrvpost(drvsrv("uart", files, nelem(files)));
 *	drvhealthrun("uartfs");
 *
 * The kit is linked into each driver, not shared through a service: a
 * segattach, a wait read and a DMA write are cheap, and a driver that cannot
 * reach them is not a driver.  Include this after the usual Plan 9 preamble:
 *
 *	#include <u.h>
 *	#include <libc.h>
 *	#include <fcall.h>
 *	#include <thread.h>
 *	#include <9p.h>
 *	#include "drv.h"
 *
 * <health.h> comes from sys/src/cmd/o9drvman, which the userland build adds
 * to the include path for driver programs.
 */
#ifndef _DRV_H_
#define _DRV_H_

#include "health.h"

enum {
	DrvClean,
	DrvInval,
	DrvCleanInval,
};

typedef struct Drv Drv;
struct Drv {
	char	name[64];	/* #P node name, for segattach and /wait */
	char	klass[32];	/* class file, "uart", "gmac", ... */
	char	compat[160];	/* compatible file */
	u64	base;		/* reg */
	u64	size;		/* size */
	int	irq;		/* irq, -1 if the node has none */
	int	console;	/* console file: 1 for the firmware console */

	void	*regs;		/* mapped, adjusted to base */
	int	waitfd;		/* #P/<name>/wait, -1 until first wait */

	void	*dmava;		/* the #P/dma window */
	u64	dmabase;
	u64	dmasize;
};

/* Discovery: scan #P for a node of this class.  wantconsole prefers the
 * device the firmware marked as its console, else the first match.  "*"
 * matches any node that publishes registers.  Both return 0 on success. */
int	drvfind(Drv*, char *class, int wantconsole);
int	drvfindreg(Drv*, u64 base);	/* by register base */
int	drvfindexact(Drv*, char *node);	/* by #P node name */

/* Map the MMIO window (2 MiB aligned, like the kernel's grant). */
int	drvmap(Drv*);
void	drvunmap(Drv*);

/* Registers: byte offset, 32-bit, little-endian, as every driver writes
 * them.  Offsets are expected to be aligned; use a byte pointer for the
 * odd-sized register a device insists on. */
u32	drd(Drv*, u32 off);
void	drw(Drv*, u32 off, u32 v);

/* Interrupts: block until the device's grant reports one.  The driver still
 * has to acknowledge the device and clear its own status.  The wait file is
 * opened on first use, in the process that waits. */
int	drvwaitfd(Drv*);
int	drvwait(Drv*);

/* DMA: the shared region is mapped once; drvdma returns a user address and
 * the physical address the device needs.  Clean/inval around ownership
 * changes where the device is not cache-coherent.  drvpa maps a physical
 * address already inside the arena. */
int	drvdmamap(Drv*);
void	*drvdma(Drv*, u64 size, u64 align, u64 *pa);
void	drvdmafree(u64 pa, u64 size);
int	drvdmacache(int op, u64 pa, u64 len);
void	*drvpa(Drv*, u64 pa);

/* Logging: the message goes to the console (and stderr, then exit).  Drivers
 * prefix their own name, as "uartfs: ...". */
void	drvlog(char *fmt, ...);
void	drvfail(char *fmt, ...);

/* A 9P service: files with read/write handlers, posted by name.  The handler
 * is called with the aux pointer from its DrvFile. */
typedef struct DrvFile DrvFile;
struct DrvFile {
	char	*name;
	u32	mode;
	void	*aux;
	void	(*read)(Req*, void*);
	void	(*write)(Req*, void*);
};

typedef struct DrvSrv DrvSrv;
struct DrvSrv {
	Srv	srv;
	Tree	*tree;
	DrvFile	*files;
	int	nfiles;
	char	name[32];
};

DrvSrv	*drvsrv(char *name, DrvFile *files, int nfiles);
void	drvsrvpost(DrvSrv*);
DrvFile	*drvfile(Req*);	/* the file the request is on */

/* Health: report ready, then the standard once-a-second loop until drvman
 * asks the driver to stop.  Without a health environment the calls are
 * harmless; drvhealthrun parks the process instead of exiting, so a driver
 * started from rc stays up.  code must be a health token (letters, digits,
 * '.', '_', '-') or nil. */
DrvHealth *drvhealthopen(char *name);
void	drvhealthreport(DrvHealth*, int state, char *code);
int	drvhealthstop(DrvHealth*, int milliseconds);
void	drvhealthclose(DrvHealth*);
void	drvhealthrun(char *name);

#endif
