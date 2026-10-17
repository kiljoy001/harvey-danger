/* Common Ethernet controller interface, independent of the bus driver. */
#ifndef HARVEY_AARCH64_ETHERIF_H
#define HARVEY_AARCH64_ETHERIF_H

enum { MaxEther = 48, Ntypes = 8 };

typedef struct Ether Ether;
struct Ether {
	ISAConf ISAConf;
	int ctlrno;
	int tbdf;
	u8 ea[Eaddrlen];
	void (*attach)(Ether *);
	void (*detach)(Ether *);
	void (*transmit)(Ether *);
	void (*interrupt)(Ureg *, void *);
	i32 (*ifstat)(Ether *, void *, i32, u32);
	i32 (*ctl)(Ether *, void *, i32);
	void (*power)(Ether *, int);
	void (*shutdown)(Ether *);
	void *ctlr;
	Queue *oq;
	Netif Netif;
};

extern Block *etheriq(Ether *, Block *, int);
extern void addethercard(char *, int (*)(Ether *));
extern u32 ethercrc(unsigned char *, int);
extern int parseether(unsigned char *, char *);

#define NEXT(x, l) (((x) + 1) % (l))
#define PREV(x, l) (((x) == 0) ? (l)-1 : (x)-1)
#endif
