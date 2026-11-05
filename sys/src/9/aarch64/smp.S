/*
 * Secondary CPU entry, reached from PSCI CPU_ON with x0 pointing at the
 * CpuBoot record.  The core starts with the MMU off; the kernel image and
 * the record are identity mapped, so their link addresses are physical.
 */
	.text
	.globl	arm64cpuentry
	.type	arm64cpuentry, %function
arm64cpuentry:
	msr	daifset, #0xf		/* no interrupts until the CPU is set up */
	mov	x19, x0
	msr	spsel, #1		/* run on SP_EL1 for EL1h exceptions */
	ldr	x1, [x19, #0]		/* stack top */
	mov	sp, x1
	ldr	x1, [x19, #40]		/* Mach * */
	msr	tpidr_el1, x1
	ldr	x1, [x19, #8]		/* MAIR */
	msr	mair_el1, x1
	ldr	x1, [x19, #16]		/* TCR */
	msr	tcr_el1, x1
	ldr	x1, [x19, #48]		/* kernel root */
	msr	ttbr0_el1, x1
	ldr	x1, [x19, #32]		/* exception vectors */
	msr	vbar_el1, x1
	ldr	x1, [x19, #24]		/* SCTLR with MMU and caches on */
	msr	sctlr_el1, x1
	tlbi	vmalle1
	dsb	sy
	isb
	mov	x1, #(3 << 20)		/* FP/SIMD at EL0 and EL1 */
	msr	cpacr_el1, x1
	isb
	mov	x0, x19
	b	arm64cpustart
	.size	arm64cpuentry, .-arm64cpuentry
