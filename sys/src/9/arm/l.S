/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * ARMv7-A entry, exception vectors and coprocessor helpers.
 */

	.syntax unified
	.arch armv7-a
	.arm

	.section .text.head, "ax"
	.balign 16
	.global _start
_start:
	cpsid	if			/* no interrupts until the vectors exist */

	ldr	sp, =_boot_stack_top
	mov	r4, r2			/* keep the DTB across bss clearing */

	/* clear .bss (the MMU is still off; these are physical addresses) */
	ldr	r0, =__bss_start
	ldr	r1, =__bss_end
	mov	r2, #0
	mov	r3, #0
1:	cmp	r0, r1
	strlo	r2, [r0], #4
	blo	1b

	mov	r0, r4			/* Linux/ARM boot protocol: r2 = DTB */
	bl	arm32main

2:	wfi
	b	2b

/*
 * Exception vectors.  VBAR points here, so the table is at its linked
 * address rather than 0x00000000.
 */
	.section .text.vectors, "ax"
	.balign 32
	.global arm32vectors
arm32vectors:
	b	arm32reset		/* 0x00 reset */
	b	arm32undef		/* 0x04 undefined instruction */
	b	arm32svc		/* 0x08 supervisor call */
	b	arm32pabort		/* 0x0c prefetch abort */
	b	arm32dabort		/* 0x10 data abort */
	b	arm32reserved		/* 0x14 reserved */
	b	arm32irq		/* 0x18 interrupt request */
	b	arm32fiq		/* 0x1c fast interrupt */

arm32reset:
	b	arm32fault

arm32undef:
	b	arm32fault

arm32reserved:
	b	arm32fault

arm32fiq:
	b	arm32fault

	.balign 4
arm32fault:
	mov	r0, lr
	bl	arm32unexpected
3:	wfi
	b	3b

/*
 * IRQ: LR_irq holds the interrupted PC + 4.  Push the exception return
 * state on the SVC stack, then handle the interrupt with interrupts
 * disabled, and return through RFE.
 */
	.balign 4
arm32irq:
	sub	lr, lr, #4
	srsdb	sp!, #0x13		/* save LR_irq, SPSR_irq on SVC stack */
	cpsid	if, #0x13
	push	{r0-r3, r12, lr}
	bl	arm32irqhandler
	pop	{r0-r3, r12, lr}
	rfeia	sp!

/*
 * SVC: the kernel's own software interrupts (and, later, syscalls from
 * user mode).  Return by restoring CPSR from SPSR.
 */
	.balign 4
arm32svc:
	push	{r0-r3, r12, lr}
	bl	arm32svchandler
	pop	{r0-r3, r12, lr}
	movs	pc, lr

	.balign 4
arm32pabort:
	mov	r0, lr
	bl	arm32prefetchabort
4:	wfi
	b	4b

	.balign 4
arm32dabort:
	sub	lr, lr, #8
	mov	r0, lr
	bl	arm32dataabort
5:	wfi
	b	5b

/*
 * Coprocessor helpers.  Kept in assembly so the C code stays readable.
 */
	.text
	.balign 4
	.global arm32setttbr0
arm32setttbr0:
	mcr	p15, 0, r0, c2, c0, 0
	dsb
	isb
	bx	lr

	.global arm32setdacr
arm32setdacr:
	mcr	p15, 0, r0, c3, c0, 0
	isb
	bx	lr

	.global arm32setvbar
arm32setvbar:
	mcr	p15, 0, r0, c12, c0, 0
	isb
	bx	lr

	.global arm32tlbflush
arm32tlbflush:
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0	/* TLBIALL */
	dsb
	isb
	bx	lr

	.global arm32cacheflush
arm32cacheflush:
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0	/* ICIALLU */
	dsb
	isb
	bx	lr

	.global arm32mmuenable
arm32mmuenable:
	mrc	p15, 0, r0, c1, c0, 0
	orr	r0, r0, #(1 << 0)	/* M: MMU */
	orr	r0, r0, #(1 << 2)	/* C: data cache */
	orr	r0, r0, #(1 << 11)	/* Z: branch prediction */
	orr	r0, r0, #(1 << 12)	/* I: instruction cache */
	bic	r0, r0, #(1 << 1)	/* A: no alignment faults */
	mcr	p15, 0, r0, c1, c0, 0
	dsb
	isb
	bx	lr

	.global arm32cntfrq
arm32cntfrq:
	mrc	p15, 0, r0, c14, c0, 0
	bx	lr

	.global arm32cntvctl
arm32cntvctl:
	mrc	p15, 0, r0, c14, c3, 1
	bx	lr

	.global arm32setcntvctl
arm32setcntvctl:
	mcr	p15, 0, r0, c14, c3, 1
	isb
	bx	lr

	.global arm32setcntvtval
arm32setcntvtval:
	mcr	p15, 0, r0, c14, c3, 0
	isb
	bx	lr

	.global arm32cntpct
arm32cntpct:
	isb
	mrc	p15, 0, r0, c14, c0, 1
	bx	lr

	.global arm32id
arm32id:
	mrc	p15, 0, r0, c0, c0, 0
	bx	lr

	.global arm32dfar
arm32dfar:
	mrc	p15, 0, r0, c6, c0, 0
	bx	lr

	.global arm32dfsr
arm32dfsr:
	mrc	p15, 0, r0, c5, c0, 0
	bx	lr

	.section .bss.stack, "aw", %nobits
	.balign 16
	.skip 32768
_boot_stack_top:
