/*
 * This file is part of the Harvey operating system. It is subject to the
 * GNU GPL v2 in the top-level LICENSE.gpl file.
 *
 * The frame the SVC handler builds: r0-r12, the user bank's sp and lr,
 * then the interrupted pc and psr.
 */
#ifndef HARVEY_ARM_UREG_H
#define HARVEY_ARM_UREG_H

typedef struct Ureg Ureg;

struct Ureg {
	u32	r0;
	u32	r1;
	u32	r2;
	u32	r3;
	u32	r4;
	u32	r5;
	u32	r6;
	u32	r7;
	u32	r8;
	u32	r9;
	u32	r10;
	u32	r11;
	u32	r12;
	u32	sp;
	u32	lr;
	u32	pc;
	u32	psr;
};

#endif
