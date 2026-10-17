/* QEMU virt PL011 backend for Harvey's shared #t UART device. */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

enum {
	Dr = 0x000 / 4,
	Fr = 0x018 / 4,
	Ibrd = 0x024 / 4,
	Fbrd = 0x028 / 4,
	Lcrh = 0x02c / 4,
	Cr = 0x030 / 4,
	Imsc = 0x038 / 4,
	Icr = 0x044 / 4,
	Rxempty = 1 << 4,
	Txfull = 1 << 5,
	Uarten = 1,
	Txen = 1 << 8,
	Rxen = 1 << 9,
	Fifo = 1 << 4,
	Virtclock = 24000000,
};

static volatile u32 *regs;
static Uart pl011uart;
extern PhysUart pl011physuart;

void
pl011configure(u64 base)
{
	regs = (volatile u32 *)(usize)base;
}

static Uart *
pl011pnp(void)
{
	if(regs == nil)
		return nil;
	pl011uart.regs = (void *)regs;
	pl011uart.phys = &pl011physuart;
	pl011uart.name = "pl011";
	pl011uart.freq = Virtclock;
	pl011uart.baud = 115200;
	pl011uart.bits = 8;
	pl011uart.stop = 1;
	pl011uart.parity = 'n';
	return &pl011uart;
}

static void
pl011enable(Uart *p, int interrupt)
{
	volatile u32 *r;

	USED(interrupt);
	r = p->regs;
	r[Imsc] = 0;
	r[Icr] = 0x7ff;
	r[Cr] |= Uarten | Txen | Rxen;
}

static void
pl011disable(Uart *p)
{
	volatile u32 *r;

	r = p->regs;
	r[Imsc] = 0;
	/* The early console continues to use this UART. */
}

static int
pl011baud(Uart *p, int baud)
{
	volatile u32 *r;
	u64 divisor;

	if(baud <= 0 || baud > p->freq / 16)
		return -1;
	r = p->regs;
	divisor = ((u64)p->freq * 4 + baud / 2) / baud;
	if(divisor < 64 || divisor / 64 > 0xffff)
		return -1;
	r[Ibrd] = divisor / 64;
	r[Fbrd] = divisor % 64;
	r[Lcrh] = r[Lcrh];
	p->baud = baud;
	return 0;
}

static int
pl011bits(Uart *p, int bits)
{
	volatile u32 *r;

	if(bits < 5 || bits > 8)
		return -1;
	r = p->regs;
	r[Lcrh] = (r[Lcrh] & ~(3U << 5)) | ((bits - 5) << 5);
	p->bits = bits;
	return 0;
}

static int
pl011stop(Uart *p, int stop)
{
	volatile u32 *r;

	if(stop != 1 && stop != 2)
		return -1;
	r = p->regs;
	r[Lcrh] = stop == 2 ? r[Lcrh] | (1 << 3) : r[Lcrh] & ~(1 << 3);
	p->stop = stop;
	return 0;
}

static int
pl011parity(Uart *p, int parity)
{
	volatile u32 *r;
	u32 lcr;

	if(parity != 'n' && parity != 'e' && parity != 'o')
		return -1;
	r = p->regs;
	lcr = r[Lcrh] & ~((1 << 1) | (1 << 2));
	if(parity != 'n')
		lcr |= 1 << 1;
	if(parity == 'e')
		lcr |= 1 << 2;
	r[Lcrh] = lcr;
	p->parity = parity;
	return 0;
}

static void
pl011break(Uart *p, int ms)
{
	volatile u32 *r;

	r = p->regs;
	r[Lcrh] |= 1;
	if(ms > 0)
		delay(ms);
	r[Lcrh] &= ~1U;
}

static void
pl011modem(Uart *p, int on)
{
	USED(p);
	USED(on);
}

static void
pl011fifo(Uart *p, int on)
{
	volatile u32 *r;

	r = p->regs;
	r[Lcrh] = on ? r[Lcrh] | Fifo : r[Lcrh] & ~Fifo;
}

static int
pl011getc(Uart *p)
{
	volatile u32 *r;

	r = p->regs;
	if(r[Fr] & Rxempty)
		return -1;
	return r[Dr] & 0xff;
}

static void
pl011putc(Uart *p, int c)
{
	volatile u32 *r;

	r = p->regs;
	while(r[Fr] & Txfull)
		;
	r[Dr] = c;
}

static void
pl011kick(Uart *p)
{
	volatile u32 *r;
	int i;

	r = p->regs;
	for(i = 0; i < 128 && (r[Fr] & Txfull) == 0; i++){
		if(p->op >= p->oe && uartstageoutput(p) == 0)
			break;
		r[Dr] = *p->op++;
	}
}

static void
pl011poll(Uart *p)
{
	int i, c;

	for(i = 0; i < 128 && (c = pl011getc(p)) >= 0; i++)
		uartrecv(p, c);
	pl011kick(p);
}

static i32
pl011status(Uart *p, void *buf, i32 n, i32 offset)
{
	char status[96];

	snprint(status, sizeof(status), "b%d l%d p%c s%d poll\n",
		p->baud, p->bits, p->parity, p->stop);
	return readstr(offset, buf, n, status);
}

PhysUart pl011physuart = {
	.name = "pl011",
	.pnp = pl011pnp,
	.enable = pl011enable,
	.disable = pl011disable,
	.kick = pl011kick,
	.dobreak = pl011break,
	.baud = pl011baud,
	.bits = pl011bits,
	.stop = pl011stop,
	.parity = pl011parity,
	.modemctl = pl011modem,
	.rts = pl011modem,
	.dtr = pl011modem,
	.status = pl011status,
	.fifo = pl011fifo,
	.getc = pl011getc,
	.putc = pl011putc,
	.poll = pl011poll,
};
