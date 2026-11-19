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

	/*
	 * Every exception mode has a banked SP.  Give IRQ, abort, undefined
	 * and FIQ their own small stacks before anything can fault; a fault
	 * taken with an uninitialised SP corrupts whatever it lands on.
	 */
	cps	#0x12			/* IRQ */
	ldr	sp, =_irq_stack_top
	cps	#0x17			/* ABT */
	ldr	sp, =_abort_stack_top
	cps	#0x1b			/* UND */
	ldr	sp, =_und_stack_top
	cps	#0x11			/* FIQ */
	ldr	sp, =_fiq_stack_top
	cps	#0x13			/* back to SVC */
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
 * Exception vectors.
 *
 * A non-secure PL1 guest ignores writes to VBAR, so the table is copied
 * to the high-vector page (0xffff0000) once the MMU maps it.  The entries
 * therefore have to be position-independent: eight LDRs whose literal
 * pool follows the table, copied along with it.
 */
	.section .vectors, "ax"
	.balign 32
	.global arm32vectors
arm32vectors:
	ldr	pc, [pc, #24]		/* 0x00 reset */
	ldr	pc, [pc, #24]		/* 0x04 undefined instruction */
	ldr	pc, [pc, #24]		/* 0x08 supervisor call */
	ldr	pc, [pc, #24]		/* 0x0c prefetch abort */
	ldr	pc, [pc, #24]		/* 0x10 data abort */
	ldr	pc, [pc, #24]		/* 0x14 reserved */
	ldr	pc, [pc, #24]		/* 0x18 interrupt request */
	ldr	pc, [pc, #24]		/* 0x1c fast interrupt */
	.word	arm32reset
	.word	arm32undef
	.word	arm32svc
	.word	arm32pabort
	.word	arm32dabort
	.word	arm32reserved
	.word	arm32irq
	.word	arm32fiq
	.global arm32vectors_end
arm32vectors_end:

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
	/*
	 * SVC from user mode.  The S-bit form of STM/LDM (the ^ suffix)
	 * accesses the user bank, which is how the user's r13/r14 reach
	 * the Ureg without switching modes (CPS to user cannot be undone).
	 *
	 * Frame (low to high): r0-r12, sp, lr, type, pc, psr.
	 */
	srsdb	sp!, #0x13		/* user pc, psr */
	cpsid	if, #0x13
	sub	sp, sp, #(15*4)
	stmia	sp, {r0-r14}^		/* user r0-r12, sp, lr */
	mov	r0, sp
	bl	arm32syscall
	ldmia	sp, {r0-r14}^
	add	sp, sp, #(15*4)		/* frame -> pc, psr */
	rfeia	sp!

/*
 * void arm32enteruser(u32 entry, u32 sp)
 *
 * Enter user mode for the first time.  The SVC bank keeps the kernel
 * stack; only the user bank's sp is set.
 */
	.balign 4
	.global arm32enteruser
arm32enteruser:
	/*
	 * Restore the user register bank from a zeroed frame (S-bit LDM
	 * writes the user bank, including its sp), then return through
	 * SPSR so the first fetch is from user memory.  Switching modes
	 * with MSR would fetch the next instruction from kernel text,
	 * which is mapped privileged-only.
	 */
	stmdb	sp!, {r0, r1}		/* entry, user sp */
	sub	sp, sp, #(15*4)
	mov	r2, #0
	mov	r3, sp
	mov	r4, #15
1:	str	r2, [r3], #4
	subs	r4, r4, #1
	bne	1b
	add	r5, sp, #(15*4)
	ldr	r6, [r5, #4]		/* user sp */
	str	r6, [sp, #(13*4)]
	ldmia	sp, {r0-r14}^		/* user bank: r0-r12, sp, lr */
	add	sp, sp, #(15*4)
	ldr	r4, [sp]		/* entry */
	mov	r5, #0x10		/* user mode, IRQs enabled */
	msr	spsr_cxsf, r5
	add	sp, sp, #8
	movs	pc, r4

	.balign 4
arm32pabort:
	mov	r0, lr			/* faulting PC + 4 */
	bl	arm32prefetchabort
2:	wfi
	b	2b

	.balign 4
arm32dabort:
	sub	lr, lr, #8
	mov	r0, lr			/* faulting PC */
	cps	#0x13			/* read the SVC stack and LR */
	mov	r1, sp
	mov	r2, lr
	cps	#0x17
	bl	arm32dataabort
3:	wfi
	b	3b

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

	.global arm32sethighvectors
arm32sethighvectors:
	mrc	p15, 0, r0, c1, c0, 0
	orr	r0, r0, #(1 << 13)	/* V: vectors at 0xffff0000 */
	mcr	p15, 0, r0, c1, c0, 0
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

	.global arm32setcntvctl
arm32setcntvctl:
	mcr	p15, 0, r0, c14, c3, 1
	isb
	bx	lr

/*
 * u64 __aeabi_uldivmod(u64 n, u64 d)
 *
 * A 16-byte struct return is passed back through a hidden pointer, not in
 * r0-r3, so the compiler's C version cannot match the EABI.  Returning
 * r0:r1 = quotient and r2:r3 = remainder has to be done in assembly.
 * Restoring division, 64 iterations, quotient built by shifting.
 */
	.global __aeabi_uldivmod
__aeabi_uldivmod:
	push	{r4-r11, lr}
	mov	r4, r0			/* n lo */
	mov	r5, r1			/* n hi */
	mov	r6, r2			/* d lo */
	mov	r7, r3			/* d hi */
	mov	r8, #0			/* q lo */
	mov	r9, #0			/* q hi */
	mov	r10, #0			/* r lo */
	mov	r11, #0			/* r hi */
	mov	r12, #64
1:	movs	r8, r8, lsl #1
	adcs	r9, r9, r9		/* q <<= 1 */
	movs	r4, r4, lsl #1
	adcs	r5, r5, r5		/* n <<= 1, C = next bit */
	adcs	r10, r10, r10
	adc	r11, r11, r11		/* r = (r << 1) | bit */
	cmp	r11, r7
	bhi	2f
	blo	3f
	cmp	r10, r6
	blo	3f
2:	subs	r10, r10, r6
	sbc	r11, r11, r7
	orr	r8, r8, #1
3:	subs	r12, r12, #1
	bne	1b
	mov	r0, r8
	mov	r1, r9
	mov	r2, r10
	mov	r3, r11
	pop	{r4-r11, pc}

/*
 * i64 __aeabi_ldivmod(i64 n, i64 d)
 *
 * Signed wrapper around the unsigned routine: take absolute values,
 * divide, then apply the signs (quotient negative when the signs differ,
 * remainder negative when the dividend was).
 */
	.global __aeabi_ldivmod
__aeabi_ldivmod:
	push	{r4-r11, lr}
	mov	r4, r0			/* n */
	mov	r5, r1
	mov	r6, r2			/* d */
	mov	r7, r3
	mov	r8, #0			/* quotient negative */
	mov	r9, #0			/* remainder negative */
	cmp	r5, #0
	bge	1f
	mvn	r8, r8
	mov	r9, r8
	rsbs	r4, r4, #0
	rsc	r5, r5, #0
1:	cmp	r7, #0
	bge	2f
	mvn	r8, r8
	rsbs	r6, r6, #0
	rsc	r7, r7, #0
2:	mov	r0, r4
	mov	r1, r5
	mov	r2, r6
	mov	r3, r7
	bl	__aeabi_uldivmod
	cmp	r9, #0
	beq	3f
	rsbs	r2, r2, #0
	rsc	r3, r3, #0
3:	cmp	r8, #0
	beq	4f
	rsbs	r0, r0, #0
	rsc	r1, r1, #0
4:	pop	{r4-r11, pc}

	.global arm32cntvtval
arm32cntvtval:
	mrc	p15, 0, r0, c14, c3, 0
	bx	lr

	.global arm32cntvctl
arm32cntvctl:
	mrc	p15, 0, r0, c14, c3, 1
	bx	lr

	.global arm32setcntvtval
arm32setcntvtval:
	mcr	p15, 0, r0, c14, c3, 0
	isb
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
	.skip 4096
_irq_stack_top:
	.skip 4096
_abort_stack_top:
	.skip 4096
_und_stack_top:
	.skip 4096
_fiq_stack_top:
