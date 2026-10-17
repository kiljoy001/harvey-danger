/*
 * This file is part of the Harvey operating system. It is subject to the
 * GNU GPL v2 in the top-level LICENSE.gpl file.
 * AArch64 Label operations used by port/proc.c.
 */
	.section .text
	.balign 4
	.globl slim_setlabel
	.type slim_setlabel, %function
slim_setlabel:
	mov	x9, sp
	str	x9, [x0, #0]
	str	x30, [x0, #8]
	stp	x18, x19, [x0, #16]
	stp	x20, x21, [x0, #32]
	stp	x22, x23, [x0, #48]
	stp	x24, x25, [x0, #64]
	stp	x26, x27, [x0, #80]
	stp	x28, x29, [x0, #96]
	mov	x0, xzr
	ret

	.globl gotolabel
	.type gotolabel, %function
gotolabel:
	ldr	x9, [x0, #0]
	ldr	x30, [x0, #8]
	ldp	x18, x19, [x0, #16]
	ldp	x20, x21, [x0, #32]
	ldp	x22, x23, [x0, #48]
	ldp	x24, x25, [x0, #64]
	ldp	x26, x27, [x0, #80]
	ldp	x28, x29, [x0, #96]
	mov	sp, x9
	mov	x0, #1
	ret

	/* splhi/spllo return the previous DAIF state for splx. */
	.globl splhi
	.type splhi, %function
splhi:
	mrs	x0, daif
	msr	daifset, #2
	isb
	ret

	.globl spllo
	.type spllo, %function
spllo:
	mrs	x0, daif
	msr	daifclr, #3
	isb
	ret

	.globl splx
	.type splx, %function
splx:
	msr	daif, x0
	isb
	ret

	.globl islo
	.type islo, %function
islo:
	mrs	x0, daif
	tst	x0, #0x80
	cset	w0, eq
	ret

	.globl coherence
	.type coherence, %function
coherence:
	dmb	ish
	ret
