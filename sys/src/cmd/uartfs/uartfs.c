/* uartfs: a 9P file server that owns a UART through the kernel resource
 * grant.  It discovers the console UART from #P, maps its MMIO, and serves
 * data, status, and ctl over /srv/uart.
 *
 * The discovery, mapping, 9P tree and health contract are the driver kit's
 * (sys/include/drv.h); what is left here is the UART.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"

enum {
	Uartdw8250,	/* Synopsys DW 8250 / Rockchip rk3568 */
	Uartpl011,	/* ARM PrimeCell PL011 */

	/* DW 8250 register byte offsets, accessed as 32-bit words. */
	Dwthr = 0x00,	/* transmit holding */
	Dwrbr = 0x00,	/* receive buffer */
	Dwlsr = 0x14,	/* line status: bit 0 = data ready */
	Dwusr = 0x7c,	/* status: bit 1 = transmit FIFO not full */

	/* PL011 register byte offsets, accessed as 32-bit words. */
	Pl011dr = 0x00,
	Pl011fr = 0x18,	/* bit 4 = RX empty, bit 5 = TX full */
};

typedef struct Uart Uart;
struct Uart {
	Drv d;
	int type;
};

static Uart uart;

static int
uartgetc(Uart *u)
{
	Drv *d;

	d = &u->d;
	if(u->type == Uartpl011){
		if(drd(d, Pl011fr) & (1 << 4))
			return -1;
		return drd(d, Pl011dr) & 0xff;
	}
	if((drd(d, Dwlsr) & 1) == 0)
		return -1;
	return drd(d, Dwrbr) & 0xff;
}

static void
uartputc(Uart *u, int c)
{
	Drv *d;

	d = &u->d;
	if(u->type == Uartpl011){
		while(drd(d, Pl011fr) & (1 << 5))
			;
		drw(d, Pl011dr, c);
		return;
	}
	while((drd(d, Dwusr) & (1 << 1)) == 0)
		;
	drw(d, Dwthr, c);
}

static void
uartwrite(Uart *u, char *s, int n)
{
	while(n-- > 0)
		uartputc(u, *s++);
}

static void
fsread(Req *r, void *aux)
{
	Uart *u;
	char buf[128];
	int c, n;

	u = aux;
	if(strcmp(drvfile(r)->name, "data") == 0){
		n = 0;
		while(n < (int)r->ifcall.count && (c = uartgetc(u)) >= 0)
			buf[n++] = c;
		if(n > 0)
			memmove(r->ofcall.data, buf, n);
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}
	if(strcmp(drvfile(r)->name, "status") == 0){
		strcpy(buf, u->type == Uartpl011 ? "pl011\n" : "dw8250\n");
		readstr(r, buf);
		respond(r, nil);
		return;
	}
	respond(r, "read not permitted");
}

static void
fswrite(Req *r, void *aux)
{
	Uart *u;
	char *name;

	u = aux;
	name = drvfile(r)->name;
	if(strcmp(name, "data") == 0){
		uartwrite(u, (char *)r->ifcall.data, r->ifcall.count);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	if(strcmp(name, "ctl") == 0){
		/* Configuration is accepted but not interpreted yet. */
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	respond(r, "write not permitted");
}

static DrvFile files[] = {
	{ "data",	0666,	&uart,	fsread,	fswrite },
	{ "status",	0444,	&uart,	fsread,	nil },
	{ "ctl",	0666,	&uart,	nil,	fswrite },
};

void
main(int argc, char **argv)
{
	DrvSrv *s;

	USED(argc);
	USED(argv);
	if(drvfind(&uart.d, "uart", 1) < 0)
		drvfail("uartfs: no UART in #P");
	if(drvmap(&uart.d) < 0)
		drvfail("uartfs: cannot map %s: %r", uart.d.name);
	uart.type = strstr(uart.d.compat, "pl011") != nil ? Uartpl011 : Uartdw8250;

	uartwrite(&uart, "HARVEY-ARM64 UARTFS PASS\n",
	          sizeof("HARVEY-ARM64 UARTFS PASS\n") - 1);

	s = drvsrv("uart", files, nelem(files));
	if(s == nil)
		drvfail("uartfs: cannot build 9P tree");
	drvsrvpost(s);
	drvhealthrun("uartfs");
}
