/* Restore a saved ARM64 user register frame and return to EL0. */
	.section .text
	.balign 4
	.globl arm64enterureg
	.type arm64enterureg, %function
arm64enterureg:
	mov x9, x0
	ldr x10, [x9, #248]
	msr sp_el0, x10
	ldr x10, [x9, #272]
	msr elr_el1, x10
	ldr x10, [x9, #264]
	msr spsr_el1, x10
	ldp x0, x1, [x9, #0]
	ldp x2, x3, [x9, #16]
	ldp x4, x5, [x9, #32]
	ldp x6, x7, [x9, #48]
	ldr x8, [x9, #64]
	ldp x10, x11, [x9, #80]
	ldp x12, x13, [x9, #96]
	ldp x14, x15, [x9, #112]
	ldp x16, x17, [x9, #128]
	ldp x18, x19, [x9, #144]
	ldp x20, x21, [x9, #160]
	ldp x22, x23, [x9, #176]
	ldp x24, x25, [x9, #192]
	ldp x26, x27, [x9, #208]
	ldp x28, x29, [x9, #224]
	ldr x30, [x9, #240]
	ldr x9, [x9, #72]
	isb
	eret
