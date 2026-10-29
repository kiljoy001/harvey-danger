/*
 * aarch64 Ethernet glue for port/devether.c.
 *
 * There is no PCI bus to probe: cards register themselves with
 * addethercard() and the reset function fills in an Ether.  The GMAC
 * driver is the first one; USB Ethernet can follow the same route.
 */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "../port/netif.h"
#include "etherif.h"

Ether *
archetherprobe(int ctlrno, char *type, int (*reset)(Ether *))
{
	Ether *ether;

	USED(type);
	ether = mallocz(sizeof(Ether), 1);
	if(ether == nil)
		return nil;
	ether->ctlrno = ctlrno;
	if(reset(ether) < 0){
		free(ether);
		return nil;
	}
	return ether;
}

void
archethershutdown(Ether *ether)
{
	USED(ether);
}
