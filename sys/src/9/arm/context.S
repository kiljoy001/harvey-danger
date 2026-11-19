/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * ARMV7-A context switch, interrupt masking and the small helpers the
 * shared scheduler needs.
 *
 * A saved context is just a stack pointer: slim_setlabel pushes the
 * callee-saved registers and the return address, gotolabel pops them
 * back.  labelstart() fabricates the same frame so a new task can be
 * entered for the first time through gotolabel.
 */

	.syntax unified
	.arch armv7-a
	.arm

	.text

/*
 * int slim_setlabel(Label *l)
 *
 * Returns 0 on the initial call and 1 when gotolabel resumes the label.
 * Nothing is pushed on the stack: the resume state lives in the Label.
 */
	.balign 4
	.global slim_setlabel
slim_setlabel:
	str	sp, [r0, #0]
	str	lr, [r0, #4]
	str	r4, [r0, #8]
	str	r5, [r0, #12]
	str	r6, [r0, #16]
	str	r7, [r0, #20]
	str	r8, [r0, #24]
	str	r9, [r0, #28]
	str	r10, [r0, #32]
	str	r11, [r0, #36]
	mov	r0, #0
	bx	lr

/*
 * void gotolabel(Label *l)
 */
	.balign 4
	.global gotolabel
gotolabel:
	ldr	sp, [r0, #0]
	ldr	lr, [r0, #4]
	ldr	r4, [r0, #8]
	ldr	r5, [r0, #12]
	ldr	r6, [r0, #16]
	ldr	r7, [r0, #20]
	ldr	r8, [r0, #24]
	ldr	r9, [r0, #28]
	ldr	r10, [r0, #32]
	ldr	r11, [r0, #36]
	mov	r0, #1
	bx	lr

/*
 * void labelstart(Label *l, void (*fn)(void), void *stacktop)
 */
	.balign 4
	.global labelstart
labelstart:
	bic	r2, r2, #7
	mov	r3, #0
	str	r2, [r0, #0]
	str	r1, [r0, #4]
	str	r3, [r0, #8]
	str	r3, [r0, #12]
	str	r3, [r0, #16]
	str	r3, [r0, #20]
	str	r3, [r0, #24]
	str	r3, [r0, #28]
	str	r3, [r0, #32]
	str	r3, [r0, #36]
	bx	lr

/*
 * Interrupt masks.  The old CPSR is returned so splx can restore it.
 */
	.balign 4
	.global splhi
splhi:
	mrs	r0, cpsr
	orr	r1, r0, #0xc0		/* I and F */
	msr	cpsr_c, r1
	bx	lr

	.balign 4
	.global spllo
spllo:
	mrs	r0, cpsr
	bic	r1, r0, #0xc0
	msr	cpsr_c, r1
	bx	lr

	.balign 4
	.global splx
splx:
	msr	cpsr_c, r0
	bx	lr
