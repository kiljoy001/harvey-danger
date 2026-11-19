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
 */
	.balign 4
	.global slim_setlabel
slim_setlabel:
	push	{r4-r11, lr}
	str	sp, [r0]
	mov	r0, #0
	bx	lr

/*
 * void gotolabel(Label *l)
 */
	.balign 4
	.global gotolabel
gotolabel:
	ldr	sp, [r0]
	mov	r0, #1			/* setlabel returns 1 when resumed */
	pop	{r4-r11, pc}

/*
 * void labelstart(Label *l, void (*fn)(void), void *stacktop)
 *
 * Build the frame gotolabel expects: r4-r11, then the entry point
 * popped into pc.
 */
	.balign 4
	.global labelstart
labelstart:
	bic	r2, r2, #7
	sub	r2, r2, #36		/* 8 registers + pc */
	mov	r3, #0
	str	r1, [r2, #32]
	str	r3, [r2, #0]
	str	r3, [r2, #4]
	str	r3, [r2, #8]
	str	r3, [r2, #12]
	str	r3, [r2, #16]
	str	r3, [r2, #20]
	str	r3, [r2, #24]
	str	r3, [r2, #28]
	str	r2, [r0]
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
