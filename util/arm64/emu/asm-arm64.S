/*
 * AArch64 pieces of the Inferno host layer.
 *
 * tramp()    - start a function on a new stack (emu's kproc child).
 * FPsave()/FPrestore() - the FPU control/status state emu keeps per Proc.
 *
 * AAPCS64: x0..x7 arguments, sp 16-byte aligned at calls.
 */
	.text

	.globl	tramp
	.type	tramp, %function
tramp:
	/* x0 = stack top, x1 = fn, x2 = arg */
	mov	sp, x0
	mov	x0, x2
	blr	x1
	mov	x0, #0
	bl	exits
	brk	#0
	.size	tramp, .-tramp

	.globl	FPsave
	.type	FPsave, %function
FPsave:
	/* x0 = FPU* */
	mrs	x1, fpsr
	str	x1, [x0, #0]
	mrs	x1, fpcr
	str	x1, [x0, #8]
	ret
	.size	FPsave, .-FPsave

	.globl	FPrestore
	.type	FPrestore, %function
FPrestore:
	/* x0 = FPU* */
	ldr	x1, [x0, #0]
	msr	fpsr, x1
	ldr	x1, [x0, #8]
	msr	fpcr, x1
	ret
	.size	FPrestore, .-FPrestore

	.globl	getcallerpc
	.type	getcallerpc, %function
getcallerpc:
	mov	x0, x30
	ret
	.size	getcallerpc, .-getcallerpc

	.globl	FPinit
	.type	FPinit, %function
FPinit:
	ret
	.size	FPinit, .-FPinit
