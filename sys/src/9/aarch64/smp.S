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
	/*
	 * PSCI starts a secondary at the EL the secure monitor uses for the
	 * non-secure world, which on the RK3566 is EL2.  The primary drops to
	 * EL1 in _startcode; do the same here, or every EL1 register write is
	 * ignored and the core runs with its MMU off (exclusive accesses such
	 * as the lock word then never complete).
	 */
	mrs	x1, CurrentEL
	cmp	x1, #(2 << 2)
	b.ne	2f
	mov	x1, #3
	msr	cnthctl_el2, x1
	msr	cntvoff_el2, xzr
	mrs	x1, id_aa64pfr0_el1
	ubfx	x1, x1, #24, #4
	cbz	x1, 1f
	mov	x1, #0xf
	msr	S3_4_C12_C9_5, x1	/* icc_sre_el2: EL1 GICv3 sysregs */
1:
	msr	cptr_el2, xzr
	mov	x1, #(1 << 31)
	msr	hcr_el2, x1
	mov	x1, #0x3c5		/* EL1h, DAIF masked */
	msr	spsr_el2, x1
	adr	x1, 2f
	msr	elr_el2, x1
	isb
	eret
2:
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
