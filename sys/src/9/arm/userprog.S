/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in the top-level LICENSE.gpl file.
 *
 * A position-independent user program used to validate the EL0 path: it
 * asks for exits(0) and spins.  The kernel copies these bytes into a user
 * page, so only PC-relative references are allowed here.
 */

	.syntax unified
	.arch armv7-a
	.arm

	.section .text.userprog, "ax"
	.balign 4
	.global arm32userprog_start
	.global arm32userprog_end
arm32userprog_start:
	mov	r7, #99			/* invalid: exercises the return path */
	svc	#0
	mov	r7, #8			/* exits */
	mov	r0, #0
	svc	#0
1:	b	1b
arm32userprog_end:
