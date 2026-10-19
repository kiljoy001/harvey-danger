/* One-shot EL0 entry and isolated code used to validate the trap path. */
	.section .text.user, "ax"
	.balign 4
	.globl arm64enteruser
arm64enteruser:
	sub	sp, sp, #112
	str	x18, [sp, #0]
	stp	x19, x20, [sp, #16]
	stp	x21, x22, [sp, #32]
	stp	x23, x24, [sp, #48]
	stp	x25, x26, [sp, #64]
	stp	x27, x28, [sp, #80]
	stp	x29, x30, [sp, #96]
	msr	sp_el0, x1
	msr	elr_el1, x0
	mov	x0, x2
	mov	x1, x3
	mov	x2, x4
	mov	x15, #0x340	/* EL0t with IRQ enabled */
	msr	spsr_el1, x15
	/* Do not expose the kernel's general registers to EL0. */
	mov	x3, xzr
	mov	x4, xzr
	mov	x5, xzr
	mov	x6, xzr
	mov	x7, xzr
	mov	x8, xzr
	mov	x9, xzr
	mov	x10, xzr
	mov	x11, xzr
	mov	x12, xzr
	mov	x13, xzr
	mov	x14, xzr
	mov	x15, xzr
	mov	x16, xzr
	mov	x17, xzr
	mov	x18, xzr
	mov	x19, xzr
	mov	x20, xzr
	mov	x21, xzr
	mov	x22, xzr
	mov	x23, xzr
	mov	x24, xzr
	mov	x25, xzr
	mov	x26, xzr
	mov	x27, xzr
	mov	x28, xzr
	mov	x29, xzr
	mov	x30, xzr
	isb
	eret
	.globl arm64userresume
arm64userresume:
	ldr	x18, [sp, #0]
	ldp	x19, x20, [sp, #16]
	ldp	x21, x22, [sp, #32]
	ldp	x23, x24, [sp, #48]
	ldp	x25, x26, [sp, #64]
	ldp	x27, x28, [sp, #80]
	ldp	x29, x30, [sp, #96]
	add	sp, sp, #112
	ret

	.globl arm64usercode
arm64usercode:
	cbnz	x18, 1f
	cbnz	x19, 1f
	cbnz	x30, 1f
	mov	x0, #40
	svc	#1
	mov	x3, #0x1000000
2:
	subs	x3, x3, #1
	b.ne	2b
	add	x0, x0, #1
	str	x0, [sp, #-8]!
	ldr	x0, [sp], #8
	svc	#0
	b	.
1:
	svc	#2
	b	.
	.globl arm64usercodeend
arm64usercodeend:

	.globl arm64userfaultcode
arm64userfaultcode:
	ldr	x0, [x0]
	svc	#0
	b	.
	.globl arm64userfaultcodeend
arm64userfaultcodeend:

	.globl arm64userwritecode
arm64userwritecode:
	str	xzr, [x0]
	svc	#0
	b	.
	.globl arm64userwritecodeend
arm64userwritecodeend:

	.globl arm64userexecstack
arm64userexecstack:
	br	x0
	svc	#0
	b	.
	.globl arm64userexecstackend
arm64userexecstackend:

	/* Minimal payload used to prove that a scheduled Harvey Proc owns an
	 * address space and can cross the EL1/EL0 boundary. */
