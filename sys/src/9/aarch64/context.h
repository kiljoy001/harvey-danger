/*
 * This file is part of the Harvey operating system. It is subject to the
 * GNU GPL v2 in the top-level LICENSE.gpl file.
 */
#ifndef HARVEY_ARM64_CONTEXT_H
#define HARVEY_ARM64_CONTEXT_H

/* The shared scheduler stores one Label in each Proc and Mach. */
typedef struct Label Label;
struct Label {
	usize sp;
	usize pc;
	usize x18;
	usize x19;
	usize x20;
	usize x21;
	usize x22;
	usize x23;
	usize x24;
	usize x25;
	usize x26;
	usize x27;
	usize x28;
	usize x29;
};

int slim_setlabel(Label *) __attribute__((returns_twice));
void gotolabel(Label *) __attribute__((noreturn));
u64 splhi(void);
u64 spllo(void);
void splx(u64);

#endif
