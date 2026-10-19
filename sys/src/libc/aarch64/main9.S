#define NPRIVATES 16

	.text
	.balign 4
	.globl _main
	.type _main, %function
_main:
	/* sysexecregs supplies argc, argv and Tos in x0, x1 and x2. */
	adrp	x9, _tos
	add	x9, x9, :lo12:_tos
	str	x2, [x9]
	sub	x9, sp, #(NPRIVATES * 8)
	adrp	x10, _privates
	add	x10, x10, :lo12:_privates
	str	x9, [x10]
	mov	w9, #NPRIVATES
	adrp	x10, _nprivates
	add	x10, x10, :lo12:_nprivates
	str	w9, [x10]
	sub	sp, sp, #160
	bl	main
	adrp	x0, .Lmain
	add	x0, x0, :lo12:.Lmain
	bl	_exits
1:
	b	1b

	.globl gettls0
	.type gettls0, %function
gettls0:
	mov	x0, #0
	ret

	.section .rodata
.Lmain:
	.asciz "main"
