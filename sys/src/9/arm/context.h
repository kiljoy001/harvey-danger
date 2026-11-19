/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * The scheduler's coroutine primitive.  A Label is one saved stack; the
 * shared proc code stores one in each Proc and Mach.
 */
#ifndef HARVEY_ARM_CONTEXT_H
#define HARVEY_ARM_CONTEXT_H

typedef struct Label Label;
struct Label {
	u32 sp;
};

int slim_setlabel(Label *) __attribute__((returns_twice));
void gotolabel(Label *) __attribute__((noreturn));
void labelstart(Label *, void (*)(void), void *);
u32 splhi(void);
u32 spllo(void);
void splx(u32);

#endif
