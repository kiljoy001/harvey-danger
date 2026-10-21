#ifndef HARVEY_ARM64_VOP2_H
#define HARVEY_ARM64_VOP2_H

#include <u.h>
#include "fdt.h"

typedef struct Vmode Vmode;
struct Vmode {
	int clock;		/* pixel clock in kHz */
	int hactive;
	int hfront;
	int hsync;
	int hback;
	int vactive;
	int vfront;
	int vsync;
	int vback;
};

/* Scanout buffer exported by the VOP device (#V/display). */
typedef struct Vopfb Vopfb;
struct Vopfb {
	u64	pa;		/* physical address of the linear buffer */
	u64	iova;		/* VOP IOMMU address */
	u64	size;		/* bytes; 0 if no linear buffer */
	u64	seq;		/* frame/commit sequence */
	u32	width;
	u32	height;
	u32	stride;		/* bytes per line */
	u32	format;		/* 0 = XRGB8888 */
};
extern Vopfb vopfb;

int vop2init(Fdtinfo *);
int dw_hdmi_start(u64, Vmode *);

#endif
