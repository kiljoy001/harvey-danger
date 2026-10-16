/*
 * This file is part of the Harvey operating system.  It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * First ARM64 Harvey milestone: enter from an ARM64 Image, retain the
 * firmware's device tree, and report progress on QEMU virt's PL011.
 * The UART address is temporary; the real driver must obtain it from DT.
 */

#include <u.h>

enum {
	Uartbase = 0x09000000,
	Uartdr = 0x00 / 4,
	Uartfr = 0x18 / 4,
	Uarttxff = 1 << 5,
	Fdtmagic = 0xd00dfeed,
	Fdtmin = 40,
	Fdtmax = 16 * 1024 * 1024,
};

static void
uartputc(int c)
{
	volatile u32 *uart;

	uart = (volatile u32 *)Uartbase;
	while(uart[Uartfr] & Uarttxff)
		;
	uart[Uartdr] = c;
}

static void
uartputs(char *s)
{
	for(; *s != 0; s++){
		if(*s == '\n')
			uartputc('\r');
		uartputc(*s);
	}
}

static u32
beget4(u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static void
uarthex(usize n)
{
	int i, c;

	uartputs("0x");
	for(i = 15; i >= 0; i--){
		c = (n >> (i * 4)) & 15;
		uartputc(c < 10 ? '0' + c : 'a' + c - 10);
	}
}

void
arm64main(usize dtb)
{
	u8 *p;
	u32 size;

	uartputs("HARVEY-ARM64 ENTRY\n");
	if(dtb == 0 || (dtb & 7) != 0){
		uartputs("dtb: missing or unaligned\n");
		return;
	}
	p = (u8 *)dtb;
	size = beget4(p + 4);
	if(beget4(p) != Fdtmagic || size < Fdtmin || size > Fdtmax){
		uartputs("dtb: invalid header\n");
		return;
	}
	uartputs("dtb: valid at ");
	uarthex(dtb);
	uartputs(", bytes ");
	uarthex(size);
	uartputs("\nHARVEY-ARM64 EARLY-BOOT PASS\n");
}
