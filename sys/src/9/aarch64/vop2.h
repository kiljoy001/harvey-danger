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

int vop2init(Fdtinfo *);
int dw_hdmi_start(u64, Vmode *);

#endif
