/* ARM64 Image header and early entry for QEMU virt's direct boot. */

	.section .text.head, "ax"
	.balign 8
	.globl _start
_start:
	b	_startcode
	.word	0
	.quad	0x80000		/* load offset from the start of RAM */
	.quad	__image_end - _start
	.quad	8		/* little endian; firmware may retain the load base */
	.quad	0
	.quad	0
	.quad	0
	.word	0x644d5241	/* ARM64 Image magic */
	.word	0

_startcode:
	/* x0 holds the device tree address.  Do not touch it before main. */
	mrs	x1, CurrentEL
	cmp	x1, #(2 << 2)
	b.ne	0f
	mov	x1, #3
	msr	cnthctl_el2, x1
	msr	cntvoff_el2, xzr
	mrs	x1, id_aa64pfr0_el1
	ubfx	x1, x1, #24, #4
	cbz	x1, 4f
	mov	x1, #0xf
	msr	S3_4_C12_C9_5, x1	/* icc_sre_el2: allow EL1 GICv3 sysregs */
4:
	msr	cptr_el2, xzr
	mov	x1, #(1 << 31)
	msr	hcr_el2, x1
	mov	x1, #0x3c5		/* EL1h, DAIF masked */
	msr	spsr_el2, x1
	adr	x1, 0f
	msr	elr_el2, x1
	isb
	eret
0:
	/* Establish the EL1 alignment policy after U-Boot.  The build also
	 * uses -mstrict-align because pre-MMU unaligned accesses still fault. */
	mrs	x1, sctlr_el1
	bic	x1, x1, #(1 << 1)
	msr	sctlr_el1, x1
	isb
	adrp	x1, __bss_start
	add	x1, x1, :lo12:__bss_start
	adrp	x2, __bss_end
	add	x2, x2, :lo12:__bss_end
1:
	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:
	adrp	x1, _boot_stack_top
	add	x1, x1, :lo12:_boot_stack_top
	mov	sp, x1
	bl	arm64main
3:
	wfi
	b	3b

	.section .bss.stack, "aw", %nobits
	.balign 16
	.skip 16384
_boot_stack_top:

	.section .text.vectors, "ax"
	.balign 2048
	.globl arm64vectors
arm64vectors:
	/* EL1t, EL1h, lower AArch64, lower AArch32: four slots each. */
	.rept 4
	b	_arm64fault
	.space 124
	.endr
	b	_arm64usersync	/* EL1h synchronous faults in a process context. */
	.space 124
	b	_arm64irq
	.space 124
	.rept 2
	b	_arm64fault
	.space 124
	.endr
	b	_arm64usersync
	.space 124
	b	_arm64irq
	.space 124
	.rept 6
	b	_arm64fault
	.space 124
	.endr

_arm64fault:
	bl	arm64fault
	b	_arm64fault

_arm64irq:
	sub	sp, sp, #288
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30, [sp, #240]
	add	x9, sp, #288
	str	x9, [sp, #248]
	str	xzr, [sp, #256]
	mrs	x9, spsr_el1
	str	x9, [sp, #264]
	mrs	x9, elr_el1
	str	x9, [sp, #272]
	mov	x0, sp
	bl	arm64irq
	ldr	x9, [sp, #272]
	msr	elr_el1, x9
	ldr	x9, [sp, #264]
	msr	spsr_el1, x9
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30, [sp, #240]
	add	sp, sp, #288
	eret

_arm64usersync:
	sub	sp, sp, #304
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30, [sp, #240]
	mrs	x0, elr_el1
	str	x0, [sp, #256]
	mrs	x0, spsr_el1
	str	x0, [sp, #264]
	mrs	x0, esr_el1
	str	x0, [sp, #272]
	mrs	x0, far_el1
	str	x0, [sp, #280]
	mrs	x0, sp_el0
	str	x0, [sp, #288]
	mov	x0, sp
	bl	arm64usertrap
	ldr	x0, [sp, #256]
	msr	elr_el1, x0
	ldr	x0, [sp, #264]
	msr	spsr_el1, x0
	ldr	x0, [sp, #288]
	msr	sp_el0, x0
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30, [sp, #240]
	add	sp, sp, #304
	eret
