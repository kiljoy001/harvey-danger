/* ARM64 Image header and early entry for QEMU virt's direct boot. */

	.section .text.head, "ax"
	.balign 8
	.globl _start
_start:
	b	_startcode
	.word	0
	.quad	0x80000		/* load offset from the start of RAM */
	.quad	__image_end - _start
	.quad	0		/* little endian, 4 KiB pages */
	.quad	0
	.quad	0
	.quad	0
	.word	0x644d5241	/* ARM64 Image magic */
	.word	0

_startcode:
	/* x0 holds the device tree address.  Do not touch it before main. */
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
