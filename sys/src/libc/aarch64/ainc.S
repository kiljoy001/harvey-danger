// This file is part of the Harvey operating system.  It is subject to the
// license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
// directory of this distribution and at http://www.gnu.org/licenses/gpl-2.0.txt
//
// No part of Harvey operating system, including this file, may be copied,
// modified, propagated, or distributed except according to the terms
// contained in the LICENSE.gpl file.

.text

// i32 ainc(i32*): atomically increment and return the new value.
.globl ainc
ainc:	LDXR	w1, [x0]
	ADD	w1, w1, #1
	STLXR	w9, w1, [x0]
	CBNZ	w9, ainc
	DMB	ISH
	MOV	w0, w1
	RET

// i32 adec(i32*): atomically decrement and return the new value.
.globl adec
adec:	LDXR	w1, [x0]
	SUB	w1, w1, #1
	STLXR	w9, w1, [x0]
	CBNZ	w9, adec
	DMB	ISH
	MOV	w0, w1
	RET
