/*
 * aarch64 Ethernet glue for port/devether.c, modelled on the amd64
 * ether.c.  There is no PCI bus to probe: cards register themselves
 * with addethercard() and the first reset that succeeds claims the
 * controller.
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
	char name[32];
	int i, j;
	u32 limit;

	snprint(name, sizeof(name), "ether%d", ctlrno);
	ether = mallocz(sizeof(Ether), 1);
	if(ether == nil)
		return nil;
	ether->ctlrno = ctlrno;
	ether->Netif.mbps = 10;
	ether->Netif.minmtu = ETHERMINTU;
	ether->Netif.mtu = ETHERMAXTU;
	ether->Netif.maxmtu = ETHERMAXTU;

	if(reset(ether) < 0){
		free(ether);
		return nil;
	}

	print("#l%d: %s: %dMbps %2.2x%2.2x%2.2x%2.2x%2.2x%2.2x\n",
	      ctlrno, type, ether->Netif.mbps,
	      ether->ea[0], ether->ea[1], ether->ea[2],
	      ether->ea[3], ether->ea[4], ether->ea[5]);

	j = ether->Netif.mbps;
	if(j > 1000)
		j *= 10;
	for(i = 0; j >= 100; i++)
		j /= 10;
	limit = (128 << i) * 1024;
	netifinit(&ether->Netif, name, Ntypes, limit);
	if(ether->oq == nil)
		ether->oq = qopen(limit, Qmsg, 0, 0);
	if(ether->oq == nil)
		panic("etherreset %s", name);
	ether->Netif.alen = Eaddrlen;
	memmove(ether->Netif.addr, ether->ea, Eaddrlen);
	memset(ether->Netif.bcast, 0xFF, Eaddrlen);
	return ether;
}

void
archethershutdown(Ether *ether)
{
	USED(ether);
}
