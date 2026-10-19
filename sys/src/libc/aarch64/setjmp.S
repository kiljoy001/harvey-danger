// This file is part of the Harvey operating system.  It is subject to the
// license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
// directory of this distribution and at http://www.gnu.org/licenses/gpl-2.0.txt
//
// No part of Harvey operating system, including this file, may be copied,
// modified, propagated, or distributed except according to the terms
// contained in the LICENSE.gpl file.

.text

.globl setjmp
setjmp: STP     x19, x20, [x0,#0]
	STP     x21, x22, [x0,#16]
	STP     x23, x24, [x0,#32]
	STP     x25, x26, [x0,#48]
	STP     x27, x28, [x0,#64]
	STP     x29, x30, [x0,#80]
	MOV     x2, sp
	STR     x2, [x0,#104]
	/* General-register-only userland reserves slots 14..21 for future FP
	 * state. Launcher arguments live in otherwise unused slots 22 and 23. */
	STP     x1, x2, [x0,#176]
	MOV     x0, #0
	RET

.globl longjmp
longjmp:
	MOV     x3, x1
	LDP     x19, x20, [x0,#0]
	LDP     x21, x22, [x0,#16]
	LDP     x23, x24, [x0,#32]
	LDP     x25, x26, [x0,#48]
	LDP     x27, x28, [x0,#64]
	LDP     x29, x30, [x0,#80]
	LDR     x2, [x0,#104]
	MOV     sp, x2
	LDP     x1, x2, [x0,#176]

	MOV     x0, x3
	CBNZ    x3, 1f
	MOV     x0, #1
1:	BR      x30
