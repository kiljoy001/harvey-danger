/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * ARMv7-A primitives: PL011 console, delays and cache maintenance.
 */
#include "u.h"
#include "mem.h"
#include "arm.h"

enum {
	Uartdr		= 0x00,
	Uartfr		= 0x18,
	Uartibrd	= 0x24,
	Uartfbrd	= 0x28,
	Uartlcrh	= 0x2c,
	Uartcr		= 0x30,
	Uarticr		= 0x44,

	Uarttxff	= 1 << 5,
	Uartrxfe	= 1 << 4,
	Uartfen		= 1 << 4,
	Uartwlen8	= 3 << 5,
	Uartuen		= 1 << 0,
	Uarttxe		= 1 << 8,
	Uartrxe		= 1 << 9,
};

#define UARTREG(r)	(*(volatile u32*)(UARTBASE + (r)))

void
uartinit(void)
{
	UARTREG(Uartcr) = 0;
	UARTREG(Uarticr) = 0x7ff;
	/* QEMU ignores the divisor; keep the usual 24 MHz / 115200 values. */
	UARTREG(Uartibrd) = 13;
	UARTREG(Uartfbrd) = 1;
	UARTREG(Uartlcrh) = Uartfen | Uartwlen8;
	UARTREG(Uartcr) = Uartuen | Uarttxe | Uartrxe;
}

void
uartputc(int c)
{
	while(UARTREG(Uartfr) & Uarttxff)
		;
	UARTREG(Uartdr) = c;
}

int
uartgetc(void)
{
	if(UARTREG(Uartfr) & Uartrxfe)
		return -1;
	return UARTREG(Uartdr);
}

void
putstr(char *s)
{
	while(*s)
		uartputc(*s++);
}


/*
 * The kernel's print() (port/print.c) formats into a buffer and hands it
 * to the console layer through consuartputs.  Route that at the PL011.
 */
void
arm32conswrite(char *s, int n)
{
	while(n-- > 0)
		uartputc(*s++);
}
