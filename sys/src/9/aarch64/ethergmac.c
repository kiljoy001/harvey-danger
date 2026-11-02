/*
 * Kernel Ether driver for the RK3566 GMAC (snps,dwmac-4.20a) with the
 * Motorcomm YT8531 PHY on this board.
 *
 * This kernel does not dispatch device interrupts to kernel handlers,
 * so reception is polled from a clock link and handed to etheriq().
 * The link runs at 10 Mbps: that is the only speed this v1.1 board
 * passes traffic at (U-Boot fails at 100 and 1000 too).
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "../port/netif.h"
#include "etherif.h"

enum {
	GmacBase	= 0xfe010000,
	GrfBase		= 0xfdc60000,
	CruBase		= 0xfdd20000,

	/* dwmac4 MAC registers */
	GmacConfig	= 0x000,
	GmacIntEn	= 0x0b4,
	GmacFilt	= 0x008,
	GmacAddr0High	= 0x300,
	GmacAddr0Low	= 0x304,
	MdioAddr	= 0x200,
	MdioData	= 0x204,

	MtlTxQ0OpMode	= 0xd00,
	MtlRxQ0OpMode	= 0xd30,

	DmaBusMode	= 0x1000,
	DmaSysBusMode	= 0x1004,
	DmaCh0Control	= 0x1100,
	DmaCh0TxControl	= 0x1104,
	DmaCh0RxControl	= 0x1108,
	DmaCh0TxBaseHi	= 0x1110,
	DmaCh0TxBase	= 0x1114,
	DmaCh0RxBaseHi	= 0x1118,
	DmaCh0RxBase	= 0x111c,
	DmaCh0TxTail	= 0x1120,
	DmaCh0RxTail	= 0x1128,
	DmaCh0TxRingLen	= 0x112c,
	DmaCh0RxRingLen	= 0x1130,
	DmaCh0CurTxDesc	= 0x1144,
	DmaCh0CurRxDesc	= 0x114c,
	DmaCh0Status	= 0x1160,

	Gmac1Con0	= 0x388,
	Gmac1Con1	= 0x38c,
	CruGmac1Sel33	= 0x184,
	CruSoftrst14	= 0x438,	/* SOFTRST_CON(14) */
	GmacRstBit	= 1<<12,	/* SRST_A_GMAC1, reset id 0xec */

	MiiBusy		= 1<<0,
	MiiRead		= 3<<2,
	MiiWrite	= 1<<2,
	MiiBmcr		= 0,
	MiiBmsr		= 1,
	MiiPhysid1	= 2,
	MiiPhysid2	= 3,

	DescSize	= 16,
	TxRing		= 8,
	RxRing		= 32,
	BufSize		= 2048,

	TxDescOff	= 0x000,
	RxDescOff	= 0x100,
	TxBufOff	= 0x1000,
	RxBufOff	= 0x2000,
	DmaSize		= 0x20000,

	/* GMAC_CONFIG bits */
	CfgRe		= 1<<0,
	CfgTe		= 1<<1,
	CfgDcrs		= 1<<9,
	CfgDm		= 1<<13,
	CfgFes		= 1<<14,
	CfgPs		= 1<<15,
	CfgJe		= 1<<16,
	CfgJd		= 1<<17,
	CfgBe		= 1<<18,
	CfgCoreInit	= CfgJe | CfgJd | CfgBe | CfgDcrs | CfgPs,

	DescOwn		= 1u<<31,
	TdescIc		= 1u<<31,
	TdescLd		= 1u<<28,
	TdescFd		= 1u<<29,
	RdescB1Valid	= 1u<<24,
	RdescIc		= 1u<<30,

	DmaSt		= 1<<0,
	DmaSr		= 1<<0,
	DmaOsp		= 1<<4,
	DmaSftReset	= 1<<0,
	DmaSysMB	= 1<<14,
	DmaPbl		= 8,
};

typedef struct Gmac Gmac;
struct Gmac {
	QLock	lk;
	volatile u32 *regs, *grf, *cru;
	u8	*dma;
	u64	dmapa;
	u8	*txdesc, *rxdesc, *txbuf, *rxbuf;
	u64	txcpa, rxcpa, txbpa, rxbpa;
	int	txidx, rxidx;
	int	speed;
	Ether	*ether;
	int	started;
};

static Gmac gmac;

static void gmacpoll(void);

extern u64 arm64dmabase, arm64dmasize;

static u32
rd(Gmac *g, int off)
{
	return g->regs[off/4];
}

static void
wr(Gmac *g, int off, u32 v)
{
	g->regs[off/4] = v;
}

static void
grfwr(Gmac *g, int off, u32 v)
{
	g->grf[off/4] = v;
}

static u32
grfrd(Gmac *g, int off)
{
	return g->grf[off/4];
}

static void
cruwr(Gmac *g, int off, u32 v)
{
	g->cru[off/4] = v;
}

static u32
cruRd(Gmac *g, int off)
{
	return g->cru[off/4];
}

static int
mdio(Gmac *g, int phy, int reg, int val, int write)
{
	u32 v;
	int i;

	for(i = 0; i < 100000; i++)
		if((rd(g, MdioAddr) & MiiBusy) == 0)
			break;
	if(i == 100000)
		return -1;
	wr(g, MdioData, write ? (val & 0xffff) : 0);
	v = MiiBusy | (phy << 21) | (reg << 16) | (1 << 8) |
	    (write ? MiiWrite : MiiRead);
	wr(g, MdioAddr, v);
	for(i = 0; i < 100000; i++)
		if((rd(g, MdioAddr) & MiiBusy) == 0)
			return rd(g, MdioData) & 0xffff;
	return -1;
}

static void
setspeed(Gmac *g, int mbps)
{
	u32 mux;

	mux = mbps == 1000 ? 0 : mbps == 100 ? 3 : 2;
	cruwr(g, CruGmac1Sel33, (mux << 4) | (3u << 20));
	g->speed = mbps;
}

static void
descwr(u8 *d, u32 a, u32 b, u32 c, u32 e)
{
	u32 *p = (u32 *)d;

	p[0] = a;
	p[1] = b;
	p[2] = c;
	p[3] = e;
}

static void
dmainit(Gmac *g)
{
	u64 pa;

	/* take the top 128 KiB of the reserved DMA region */
	pa = arm64dmabase + arm64dmasize - DmaSize;
	arm64dmasize -= DmaSize;
	g->dmapa = pa;
	g->dma = (u8 *)KADDR(pa);
	memset(g->dma, 0, DmaSize);

	g->txdesc = g->dma + TxDescOff;
	g->rxdesc = g->dma + RxDescOff;
	g->txbuf = g->dma + TxBufOff;
	g->rxbuf = g->dma + RxBufOff;
	g->txcpa = pa + TxDescOff;
	g->rxcpa = pa + RxDescOff;
	g->txbpa = pa + TxBufOff;
	g->rxbpa = pa + RxBufOff;
}

static void
dmacfg(Gmac *g)
{
	u32 cfg, i;

	wr(g, DmaBusMode, DmaSftReset);
	for(i = 0; i < 100000; i++){
		if((rd(g, DmaBusMode) & DmaSftReset) == 0)
			break;
		delay(1);
	}
	wr(g, DmaSysBusMode, DmaSysMB | (8u << 16) | (4u << 24) |
	     (1<<1) | (1<<2) | (1<<3));
	wr(g, DmaCh0Control, 0);
	wr(g, MtlTxQ0OpMode, (1<<1) | (1<<3));
	wr(g, MtlRxQ0OpMode, 1<<5);
	wr(g, DmaCh0TxControl, (DmaPbl<<16) | DmaOsp);
	wr(g, DmaCh0RxControl, (DmaPbl<<16) | (BufSize<<1));

	for(i = 0; i < RxRing; i++)
		descwr(g->rxdesc + i*DescSize, g->rxbpa + i*BufSize, 0, 0,
		       DescOwn | RdescB1Valid | RdescIc);
	dcacheclean(g->rxdesc, RxRing*DescSize);
	dcacheclean(g->txdesc, TxRing*DescSize);

	wr(g, DmaCh0TxBaseHi, 0);
	wr(g, DmaCh0TxBase, g->txcpa);
	wr(g, DmaCh0RxBaseHi, 0);
	wr(g, DmaCh0RxBase, g->rxcpa);
	wr(g, DmaCh0TxRingLen, TxRing - 1);
	wr(g, DmaCh0RxRingLen, RxRing - 1);
	wr(g, DmaCh0TxTail, g->txcpa);
	wr(g, DmaCh0RxTail, g->rxcpa + RxRing*DescSize);

	cfg = CfgCoreInit;
	if(g->speed == 100)
		cfg |= CfgFes | CfgPs;
	else if(g->speed == 10)
		cfg |= CfgPs;
	else
		cfg &= ~(CfgFes | CfgPs);
	cfg |= CfgTe | CfgRe | CfgDm;
	wr(g, GmacConfig, cfg);
	wr(g, GmacFilt, (1<<0) | (1u<<31));	/* promiscuous, receive all */
	wr(g, GmacIntEn, 0);

	wr(g, DmaCh0RxControl, rd(g, DmaCh0RxControl) | DmaSr);
	wr(g, DmaCh0TxControl, rd(g, DmaCh0TxControl) | DmaSt);
}

static int
txframe(Gmac *g, u8 *frame, int len)
{
	u8 *dp = g->txdesc + g->txidx*DescSize;
	u64 dpa = g->txcpa + g->txidx*DescSize;
	u32 *d = (u32 *)dp;
	int i;

	if(len < 60){
		memset(g->txbuf + len, 0, 60 - len);
		len = 60;
	}
	memmove(g->txbuf, frame, len);
	dcacheclean(g->txbuf, len);
	descwr(dp, g->txbpa, 0, (len & 0x3fff) | TdescIc,
	       DescOwn | TdescFd | TdescLd | (len & 0x7fff));
	dcacheclean(dp, DescSize);
	g->txidx = (g->txidx + 1) % TxRing;
	wr(g, DmaCh0TxTail, g->txcpa + g->txidx*DescSize);

	for(i = 0; i < 10000; i++){
		dcacheclean(dp, DescSize);
		if((d[3] & DescOwn) == 0)
			break;
		delay(1);
	}
	if(i == 10000)
		return -1;
	return (d[3] & (1<<15)) ? -1 : 0;
}

static void
gmacproc(void *arg)
{
	Rendez r;

	USED(arg);
	memset(&r, 0, sizeof r);
	for(;;){
		gmacpoll();
		tsleep(&r, return0, nil, 1);
	}
}

static void
transmit(Ether *ether)
{
	Gmac *g = ether->ctlr;
	Block *bp;

	if(!g->started)
		return;
	qlock(&g->lk);
	while((bp = qget(ether->oq)) != nil){
		if(txframe(g, bp->rp, BLEN(bp)) < 0)
			ether->Netif.oerrs++;
		else
			ether->Netif.outpackets++;
		freeb(bp);
	}
	qunlock(&g->lk);
}

/* Runs in a kernel process: allocb() needs a process context. */
static void
gmacpoll(void)
{
	Gmac *g = &gmac;
	Ether *ether = g->ether;
	u8 *dp;
	u32 *d;
	int len, i, n;

	if(ether == nil || !g->started)
		return;
	if(!canqlock(&g->lk))
		return;
	for(n = 0; n < RxRing; n++){
		dp = g->rxdesc + g->rxidx*DescSize;
		d = (u32 *)dp;
		dcacheclean(dp, DescSize);
		if(d[3] & DescOwn)
			break;
		len = d[3] & 0x7fff;
		if((d[3] & (1<<15)) || len <= 0 || len > BufSize){
			if(d[3] & (1<<15))
				ether->Netif.crcs++;
		}else{
			Block *bp = allocb(len);
			if(bp != nil){
				dcacheclean(g->rxbuf + g->rxidx*BufSize, len);
				memmove(bp->rp, g->rxbuf + g->rxidx*BufSize, len);
				bp->wp += len;
				etheriq(ether, bp, 1);
			}else
				ether->Netif.overflows++;
		}
		descwr(dp, g->rxbpa + g->rxidx*BufSize, 0, 0,
		       DescOwn | RdescB1Valid | RdescIc);
		dcacheclean(dp, DescSize);
		g->rxidx = (g->rxidx + 1) % RxRing;
		wr(g, DmaCh0RxTail, g->rxcpa + g->rxidx*DescSize);
	}
	qunlock(&g->lk);
}

static i32
gmacifstat(Ether *ether, void *a, i32 n, u32 offset)
{
	USED(ether);
	USED(a);
	USED(n);
	USED(offset);
	return 0;
}

static i32
gmacctl(Ether *ether, void *a, i32 n)
{
	USED(ether);
	USED(a);
	USED(n);
	return 0;
}

static void
gmacshutdown(Ether *ether)
{
	Gmac *g = ether->ctlr;

	g->started = 0;
}

/*
 * Bring the MAC out of the reset that the bootloader leaves asserted.
 * Rockchip soft-reset registers are hiword-masked: bits 31:16 select
 * the bits to write, so only that form touches the register.
 *
 * Do not touch CLKGATE_CON(17): the bootloader leaves every GMAC clock
 * gate closed and the MAC still answers MDIO, while enabling those
 * gates here wedges the register block.
 */
static void
gmacwake(Gmac *g)
{
	cruwr(g, CruSoftrst14, GmacRstBit<<16);	/* 0 = deassert */
	microdelay(2000);
}

static int
gmacreset(Ether *ether)
{
	Gmac *g = &gmac;
	int phy, st, i;
	uchar mac[Eaddrlen] = { 0x02, 0x48, 0x41, 0x52, 0x56, 0x45 };
	Rendez r;

	if(g->started)
		return -1;
	memset(&r, 0, sizeof r);
	memset(g, 0, sizeof *g);
	g->regs = (volatile u32 *)KADDR(GmacBase);
	g->grf = (volatile u32 *)KADDR(GrfBase);
	g->cru = (volatile u32 *)KADDR(CruBase);
	g->ether = ether;

	gmacwake(g);

	/* RGMII select plus the MAC TX/RX clock delay enables (CON1 = 0x13) */
	grfwr(g, Gmac1Con1, (0x13U<<16) | 0x13U);
	print("gmac: ver %ux cfg %ux feat %ux grf %ux rst %ux\n",
	      rd(g, 0x110), rd(g, 0), rd(g, 0x11c),
	      grfrd(g, Gmac1Con1), cruRd(g, CruSoftrst14));

	phy = -1;
	for(i = 0; i < 8; i++){
		int id1 = mdio(g, i, MiiPhysid1, 0, 0);
		int id2 = mdio(g, i, MiiPhysid2, 0, 0);

		if(id1 < 0 || id2 < 0 || id1 == 0xffff || id1 == 0)
			continue;
		phy = i;
		break;
	}
	if(phy < 0){
		print("gmac: no phy\n");
		return -1;
	}
	st = mdio(g, phy, 0x11, 0, 0);
	if(st < 0 || !(st & (1<<10)) || ((st >> 14) & 3) != 0){
		/* advertise 10 Mbps full duplex and renegotiate */
		mdio(g, phy, 0x9, 0, 1);
		mdio(g, phy, 0x4, 0x0061, 1);
		mdio(g, phy, MiiBmcr, 0x1200, 1);
		for(i = 0; i < 50; i++){
			st = mdio(g, phy, 0x11, 0, 0);
			if(st > 0 && (st & (1<<10)))
				break;
			tsleep(&r, return0, nil, 100);
		}
		tsleep(&r, return0, nil, 5000);
	}
	setspeed(g, 10);
	dmainit(g);
	dmacfg(g);

	memmove(ether->ea, mac, Eaddrlen);
	memmove(ether->Netif.addr, mac, Eaddrlen);
	ether->Netif.mbps = 10;
	ether->Netif.link = 1;
	ether->transmit = transmit;
	ether->interrupt = nil;
	ether->ctl = gmacctl;
	ether->ifstat = gmacifstat;
	ether->shutdown = gmacshutdown;
	ether->ctlr = g;
	g->started = 1;
	kproc("gmac", gmacproc, nil);
	print("gmac: %d Mbps, phy %d status %ux\n", g->speed, phy, st);
	return 0;
}

void
ethergmacinit(void)
{
	addethercard("gmac", gmacreset);
}
