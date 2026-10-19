// This file is part of the Harvey operating system.  It is subject to the
// license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
// directory of this distribution and at http://www.gnu.org/licenses/gpl-2.0.txt
//
// No part of Harvey operating system, including this file, may be copied,
// modified, propagated, or distributed except according to the terms
// contained in the LICENSE.gpl file.

.text

// int _tas(int*): atomically exchange *p with 0xbeefdead, return the old
// value (0 means the lock was acquired).
.globl _tas
_tas:
	MOVK	w1, #0xdead
	MOVZ	w1, #0xbeef
	LDXR	w2, [x0]
	CBNZ	w2, 1f
	STLXR	w9, w1, [x0]
	DMB	ISH
1:	MOV	w0, w2
	RET
