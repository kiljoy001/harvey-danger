/*
 * gmac: RK3566 GMAC (snps,dwmac-4.20a) driver.
 *
 * Runs against the #P grant for ethernet@fe010000.  The MAC is a
 * DesignWare GMAC4: descriptor rings in the #P/dma region, MTL queues,
 * MDIO for the YT8531 PHY.
 *
 *   gmac probe            MAC/GRF/MDIO identity and link state
 *   gmac regs             dump MAC and GRF registers
 *   gmac mdio <phy> <reg> read one MDIO register
 *   gmac tx [count]       send test frames on the wire
 *   gmac rx [seconds]     receive frames and print them
 */
#include <u.h>
#include <libc.h>

enum {
	/* dwmac4 MAC registers */
	GmacConfig	= 0x000,
	GmacFilt	= 0x008,
	GmacIntEn	= 0x0b4,
	GmacFeature0	= 0x11c,
	GmacFeature1	= 0x120,
	GmacFeature2	= 0x124,
	GmacFeature3	= 0x128,
	MdioAddr	= 0x200,
	MdioData	= 0x204,

	/* MTL queues */
	MtlTxQ0OpMode	= 0xd00,
	MtlRxQ0OpMode	= 0xd30,

	/* DMA */
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
	DmaCh0IntrEna	= 0x1134,
	DmaCh0Status	= 0x1160,

	/* GRF syscon@fdc60000 */
	Gmac1Con0	= 0x388,
	Gmac1Con1	= 0x38c,

	/* CRU */
	CruGmac1Sel33	= 0x184,	/* CLKSEL_CON(33) */
	CruGmac1Gate17	= 0x344,	/* CLKGATE_CON(17) */

	MiiBusy		= 1<<0,
	MiiRead		= 3<<2,
	MiiWrite	= 1<<2,

	MiiBmcr		= 0,
	MiiBmsr		= 1,
	MiiPhysid1	= 2,
	MiiPhysid2	= 3,

	/* descriptor rings */
	DescSize	= 16,
	TxRing		= 8,
	RxRing		= 8,
	BufSize		= 2048,

	TxDescOff	= 0x000,
	RxDescOff	= 0x100,
	TxBufOff	= 0x200,
	RxBufOff	= 0x1000,
	DmaSize		= 0x8000,

	/* MAC_CONFIG bits */
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

	/* descriptor bits */
	DescOwn		= 1u<<31,
	TdescIc		= 1u<<31,
	TdescLd		= 1u<<28,
	TdescFd		= 1u<<29,
	RdescB1Valid	= 1u<<24,
	RdescIc		= 1u<<30,

	/* DMA control */
	DmaSt		= 1<<0,
	DmaSr		= 1<<0,
	DmaOsp		= 1<<4,
	DmaSftReset	= 1<<0,
	DmaSysMB	= 1<<14,
	DmaPbl		= 8,
};

static u8 *mmio, *grf, *cru;
static u8 *dma, *txdesc, *rxdesc, *txbuf, *rxbuf;
static u64 dmabase, dmasize, dmadatapa, txcpa, rxcpa, txbpa, rxbpa;
static int dmafd[3];
static int csr = 1;
static int speed = 1000;
static int macphy;
static int txidx, rxidx;

static u32
rd32(int off)
{
	return *(volatile u32 *)(mmio + off);
}

static void
wr32(int off, u32 v)
{
	*(volatile u32 *)(mmio + off) = v;
}

static u32
grd(int off)
{
	return *(volatile u32 *)(grf + off);
}

static void
gwr(int off, u32 v)
{
	*(volatile u32 *)(grf + off) = v;
}

static u32
crd(int off)
{
	return *(volatile u32 *)(cru + off);
}

static void
cwr(int off, u32 v)
{
	*(volatile u32 *)(cru + off) = v;
}

static u8 *
mapnode(char *node, u64 want)
{
	char path[128], buf[128], *p;
	u64 base, size, len;
	u8 *m;
	int fd;

	snprint(path, sizeof path, "#P/%s/reg", node);
	if((fd = open(path, OREAD)) < 0)
		sysfatal("gmac: no %s in #P: %r", node);
	if(read(fd, buf, sizeof buf - 1) <= 0)
		sysfatal("gmac: cannot read %s reg", node);
	close(fd);
	buf[sizeof buf - 1] = 0;
	base = strtoull(buf, &p, 0);
	snprint(path, sizeof path, "#P/%s/size", node);
	if((fd = open(path, OREAD)) < 0)
		sysfatal("gmac: cannot open %s size", node);
	if(read(fd, buf, sizeof buf - 1) <= 0)
		sysfatal("gmac: cannot read %s size", node);
	close(fd);
	buf[sizeof buf - 1] = 0;
	size = strtoull(buf, &p, 0);
	if(base != want || size == 0)
		sysfatal("gmac: unexpected %s %#llux+%#llux", node, base, size);
	len = (size + 0x1fffff) & ~0x1fffffULL;
	m = segattach(0, node, 0, len);
	if(m == (void *)-1)
		sysfatal("gmac: segattach %s: %r", node);
	m += base & 0x1fffff;
	print("gmac: %s %#llux mapped at %p\n", node, base, m);
	return m;
}

static u64
readnum(char *path)
{
	char buf[64];
	int fd, n;

	if((fd = open(path, OREAD)) < 0)
		sysfatal("gmac: open %s: %r", path);
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		sysfatal("gmac: read %s: %r", path);
	buf[n] = 0;
	return strtoull(buf, nil, 0);
}

static u64
dmaalloc(u64 size, u64 align)
{
	char buf[64];
	int fd, n;

	if((fd = open("#P/dma/alloc", ORDWR)) < 0)
		sysfatal("gmac: no #P/dma/alloc: %r");
	snprint(buf, sizeof buf, "%llud %llud\n", size, align);
	if(write(fd, buf, strlen(buf)) != strlen(buf))
		sysfatal("gmac: dma alloc: %r");
	n = pread(fd, buf, sizeof buf - 1, 0);
	close(fd);
	if(n <= 0)
		sysfatal("gmac: dma alloc read: %r");
	buf[n] = 0;
	return strtoull(buf, nil, 0);
}

static void
dmacache(int op, u64 pa, u64 len)
{
	static char *name[3] = { "clean", "inval", "cleaninval" };
	char buf[64], path[32];

	if(dmafd[op] == 0){
		snprint(path, sizeof path, "#P/dma/%s", name[op]);
		if((dmafd[op] = open(path, OWRITE)) < 0)
			sysfatal("gmac: open %s: %r", path);
	}
	snprint(buf, sizeof buf, "0x%llx 0x%llx\n", pa, len);
	if(pwrite(dmafd[op], buf, strlen(buf), 0) != strlen(buf))
		sysfatal("gmac: #P/dma/%s: %r", name[op]);
}

static int
mdio(int phy, int reg, int val, int write)
{
	u32 v;
	int i;

	for(i = 0; i < 100000; i++)
		if((rd32(MdioAddr) & MiiBusy) == 0)
			break;
	if(i == 100000)
		return -1;
	wr32(MdioData, write ? (val & 0xffff) : 0);
	v = MiiBusy | (phy << 21) | (reg << 16) | (csr << 8) |
	    (write ? MiiWrite : MiiRead);
	wr32(MdioAddr, v);
	for(i = 0; i < 100000; i++)
		if((rd32(MdioAddr) & MiiBusy) == 0)
			return rd32(MdioData) & 0xffff;
	return -1;
}

/*
 * Drive the PHY reset line (GPIO3_C2 = bank 3, pin 18, active low)
 * through the gpio file server, so the pin stays owned by its driver.
 */
static void
phyreset(void)
{
	char *ctl = "/mnt/gmacgpio/gpio3/ctl";
	int srv, fd;

	srv = open("/srv/gpio", OREAD);
	if(srv < 0){
		print("gmac: no gpio service: %r\n");
		return;
	}
	create("/mnt/gmacgpio", OREAD, DMDIR|0755);
	if(mount(srv, -1, "/mnt/gmacgpio", MREPL|MCREATE, "") < 0){
		close(srv);
		print("gmac: cannot mount gpio: %r\n");
		return;
	}
	close(srv);
	fd = open(ctl, OWRITE);
	if(fd < 0){
		print("gmac: cannot open %s: %r\n", ctl);
		return;
	}
	write(fd, "18 out 0", 8);
	close(fd);
	sleep(50);
	fd = open(ctl, OWRITE);
	if(fd < 0)
		return;
	write(fd, "18 out 1", 8);
	close(fd);
	sleep(200);
	print("gmac: phy reset via gpio3.18\n");
}

static void
devregs(void)
{
	print("gmac: config %08ux feature %08ux %08ux %08ux %08ux\n",
	      rd32(GmacConfig), rd32(GmacFeature0), rd32(GmacFeature1),
	      rd32(GmacFeature2), rd32(GmacFeature3));
	print("gmac: grf gmac1 con0 %08ux con1 %08ux\n",
	      grd(Gmac1Con0), grd(Gmac1Con1));
	print("gmac: cru sel33 %08ux gate17 %08ux\n",
	      crd(CruGmac1Sel33), crd(CruGmac1Gate17));
	print("gmac: mdio addr %08ux data %08ux\n",
	      rd32(MdioAddr), rd32(MdioData));
}

/*
 * RGMII delays.  -1 leaves whatever the bootloader set, 0 disables the
 * MAC delays (phy-mode rgmii-id: the PHY adds them), 2 reprograms them
 * with the vendor device-tree values, 1 keeps U-Boot's values.
 */
static int macdelay = -1;
static int forcespeed;
static int noreset;
static int txdelay = -1, rxdelay = -1;
static int clksrc = -1;
static int noinit;

static void
setrgmii(void)
{
	u32 v;

	v = (1<<4) | (1<<20);		/* GRF_BIT(4): select RGMII */
	v |= (1<<21) | (1<<22);		/* GRF_CLR_BIT(5), GRF_CLR_BIT(6) */
	if(macdelay >= 0){
		/* bits 0 and 1 enable the MAC TX/RX clock delays */
		v |= (1<<16) | (1<<17);
		if(macdelay)
			v |= (1<<0) | (1<<1);
	}
	gwr(Gmac1Con1, v);
	if(macdelay == 2){
		/* tx_delay 0x30, rx_delay 0x10 from the vendor DTS */
		gwr(Gmac1Con0, 0x30 | (0x7fu<<16) | (0x10u<<8) | (0x7fu<<24));
	}
	if(txdelay >= 0 || rxdelay >= 0){
		u32 tx = txdelay >= 0 ? txdelay : 0x4f;
		u32 rx = rxdelay >= 0 ? rxdelay : 0x26;

		/* enable both MAC delays, then set the values */
		gwr(Gmac1Con1, (1<<0)|(1<<16) | (1<<1)|(1<<17) |
		     (1<<4)|(1<<20) | (1<<21) | (1<<22));
		gwr(Gmac1Con0, (tx & 0x7f) | (0x7fu<<16) |
		     ((rx & 0x7f)<<8) | (0x7fu<<24));
	}
	print("gmac: grf gmac1 con0 %08ux con1 %08ux\n",
	      grd(Gmac1Con0), grd(Gmac1Con1));
}

/*
 * CLKSEL_CON(33): bits 1:0 select the RX/TX clock, 2 the MAC clock
 * source and 5:4 the RGMII speed divider (0 = /1, 3 = /5, 2 = /50).
 */
static void
setspeed(int mbps)
{
	u32 mux, v;

	mux = mbps == 1000 ? 0 : mbps == 100 ? 3 : 2;
	v = (mux << 4) | (3u << 20);
	if(clksrc >= 0){
		v |= 1u << 18;			/* mask bit 2 */
		if(clksrc)
			v |= 1u << 2;		/* gmac1_clkin from the PHY */
	}
	cwr(CruGmac1Sel33, v);
	speed = mbps;
	print("gmac: cru sel33 %08ux (%d Mbps, clock %s)\n",
	      crd(CruGmac1Sel33), mbps,
	      clksrc < 0 ? "as booted" : clksrc ? "input" : "internal");
}

/* Motorcomm extended registers: address at MDIO 0x1e, data at 0x1f. */
static int
phyextwrite(int phy, u32 reg, u16 val)
{
	if(mdio(phy, 0x1e, reg & 0xffff, 1) < 0)
		return -1;
	return mdio(phy, 0x1f, val, 1);
}

static int
phyextread(int phy, u32 reg)
{
	if(mdio(phy, 0x1e, reg & 0xffff, 1) < 0)
		return -1;
	return mdio(phy, 0x1f, 0, 0);
}

/*
 * yt8531_config_init()/yt8531_rxclk_duty_init() from the Linux driver:
 * enable the 125 MHz PHY_CLK_OUT, set the RGMII RX clock delay and the
 * output drive strength.  The board runs phy-mode "rgmii-id", so the
 * PHY supplies the delays and the MAC's own delay lines must be off.
 */
static void
phyinit8531(int phy)
{
	u32 r;
	int v;

	phyextwrite(phy, 0xa012, 0xd0);
	phyextwrite(phy, 0xa010, 0xdbcf);
	phyextwrite(phy, 0xa040, 0xffff);
	phyextwrite(phy, 0xa041, 0xff);
	phyextwrite(phy, 0xa039, 0xbf00);
	for(r = 0xa03a; r <= 0xa03f; r++)
		phyextwrite(phy, r, 0x4040);
	v = phyextread(phy, 0x57);
	if(v >= 0)
		phyextwrite(phy, 0x57, (v & ~(0xf<<8)) | (7<<8));
	print("gmac: phy 0xa012 %04x 0xa03a %04x 0x57 %04x\n",
	      phyextread(phy, 0xa012), phyextread(phy, 0xa03a),
	      phyextread(phy, 0x57));
}

static int
scanphy(int aneg)
{
	int p, id1, id2, bmsr, bmcr, spec, found, i;

	found = -1;
	for(p = 0; p < 8; p++){
		id1 = mdio(p, MiiPhysid1, 0, 0);
		id2 = mdio(p, MiiPhysid2, 0, 0);
		if(id1 < 0 || id2 < 0)
			continue;
		if(id1 == 0xffff || id1 == 0 || id2 == 0xffff)
			continue;
		print("gmac: phy %d id %04ux:%04ux\n", p, id1, id2);
		if(found < 0)
			found = p;
	}
	if(found < 0)
		return -1;
	bmcr = mdio(found, MiiBmcr, 0, 0);
	if(aneg)
		mdio(found, MiiBmcr, bmcr | (1<<9), 1);	/* restart autoneg */
	for(i = 0; i < (aneg ? 60 : 2); i++){
		bmsr = mdio(found, MiiBmsr, 0, 0);
		bmsr = mdio(found, MiiBmsr, 0, 0);	/* link is latching */
		if((bmsr & (1<<5)) && (bmsr & (1<<2)))
			break;
		sleep(100);
	}
	spec = mdio(found, 0x11, 0, 0);
	print("gmac: phy %d bmcr %04ux bmsr %04ux link %d aneg %d\n",
	      found, bmcr, bmsr, (bmsr>>2)&1, (bmsr>>5)&1);
	if(spec >= 0){
		speed = (spec>>14) == 0 ? 10 : (spec>>14) == 1 ? 100 : 1000;
		print("gmac: phy status %04ux link %d speed %d duplex %d\n",
		      spec, (spec>>10)&1, speed, (spec>>13)&1);
	}
	return found;
}

static void
dmainit(void)
{
	u64 pa;

	dmabase = readnum("#P/dma/base");
	dmasize = readnum("#P/dma/size");
	dma = segattach(0, "dma", 0, dmasize);
	if(dma == (void *)-1)
		sysfatal("gmac: segattach dma: %r");
	dmadatapa = dmaalloc(DmaSize, 4096);
	memset(dma + (dmadatapa - dmabase), 0, DmaSize);
	pa = dmadatapa;
	txdesc = dma + (pa + TxDescOff - dmabase);
	rxdesc = dma + (pa + RxDescOff - dmabase);
	txbuf = dma + (pa + TxBufOff - dmabase);
	rxbuf = dma + (pa + RxBufOff - dmabase);
	txcpa = pa + TxDescOff;
	rxcpa = pa + RxDescOff;
	txbpa = pa + TxBufOff;
	rxbpa = pa + RxBufOff;
	print("gmac: dma %#llux txdesc %#llux rxdesc %#llux\n",
	      pa, txcpa, rxcpa);
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
dmacfg(void)
{
	u32 cfg, i;

	/* soft reset the DMA engine */
	wr32(DmaBusMode, DmaSftReset);
	for(i = 0; i < 100000; i++){
		if((rd32(DmaBusMode) & DmaSftReset) == 0)
			break;
		sleep(1);
	}
	if(i == 100000)
		sysfatal("gmac: dma reset timeout");
	wr32(DmaSysBusMode, DmaSysMB);
	wr32(DmaCh0IntrEna, 0);			/* polled */
	wr32(MtlTxQ0OpMode, (1<<1) | (1<<3));	/* TSF | TXQEN */
	wr32(MtlRxQ0OpMode, 1<<5);		/* RSF */
	wr32(DmaCh0TxControl, (DmaPbl<<16) | DmaOsp);
	wr32(DmaCh0RxControl, (DmaPbl<<16) | (BufSize<<1));

	/* rings */
	for(i = 0; i < RxRing; i++)
		descwr(rxdesc + i*DescSize, rxbpa + i*BufSize, 0, 0,
		       DescOwn | RdescB1Valid | RdescIc);
	dmacache(0, rxcpa, RxRing*DescSize);
	dmacache(0, txcpa, TxRing*DescSize);
	wr32(DmaCh0TxBaseHi, 0);
	wr32(DmaCh0TxBase, txcpa);
	wr32(DmaCh0RxBaseHi, 0);
	wr32(DmaCh0RxBase, rxcpa);
	wr32(DmaCh0TxRingLen, TxRing);
	wr32(DmaCh0RxRingLen, RxRing);
	wr32(DmaCh0TxTail, txcpa);
	wr32(DmaCh0RxTail, rxcpa + RxRing*DescSize);

	/* MAC: 1000/100/10, full duplex, promiscuous for bring-up */
	cfg = CfgCoreInit;
	if(speed == 100)
		cfg |= CfgFes | CfgPs;
	else if(speed == 10)
		cfg |= CfgPs;
	else
		cfg &= ~(CfgFes | CfgPs);
	cfg |= CfgTe | CfgRe | CfgDm;
	wr32(GmacConfig, cfg);
	wr32(GmacFilt, (1<<0) | (1u<<31));	/* promiscuous + receive all */
	wr32(GmacIntEn, 0);

	/* start DMA */
	wr32(DmaCh0RxControl, rd32(DmaCh0RxControl) | DmaSr);
	wr32(DmaCh0TxControl, rd32(DmaCh0TxControl) | DmaSt);
	print("gmac: mac config %08ux mtl tx %08ux rx %08ux\n",
	      rd32(GmacConfig), rd32(MtlTxQ0OpMode), rd32(MtlRxQ0OpMode));
}

static int
txframe(u8 *frame, int len)
{
	u8 *dp = txdesc + txidx*DescSize;
	u64 dpa = txcpa + txidx*DescSize;
	u32 *d = (u32 *)dp;
	int i;

	if(len < 60){
		memset(txbuf + len, 0, 60 - len);
		len = 60;
	}
	memmove(txbuf, frame, len);
	dmacache(0, txbpa, len);
	descwr(dp, txbpa, 0, (len & 0x3fff) | TdescIc,
	       DescOwn | TdescFd | TdescLd | (len & 0x7fff));
	dmacache(0, dpa, DescSize);
	/* the tail is the descriptor after the last valid one */
	txidx = (txidx + 1) % TxRing;
	wr32(DmaCh0TxTail, txcpa + txidx*DescSize);

	for(i = 0; i < 5000; i++){
		dmacache(1, dpa, DescSize);
		if((d[3] & DescOwn) == 0)
			break;
		sleep(1);
	}
	if(i == 5000){
		print("gmac: tx timeout desc3 %08ux status %08ux\n",
		      d[3], rd32(DmaCh0Status));
		return -1;
	}
	if(d[3] & (1<<15)){
		print("gmac: tx error desc3 %08ux\n", d[3]);
		return -1;
	}
	return 0;
}

static int
rxpoll(u8 *out, int max)
{
	u8 *dp = rxdesc + rxidx*DescSize;
	u64 dpa = rxcpa + rxidx*DescSize;
	u32 *d = (u32 *)dp;
	int len;

	dmacache(1, dpa, DescSize);
	if(d[3] & DescOwn)
		return 0;
	len = d[3] & 0x7fff;
	if(d[3] & (1<<15)){
		print("gmac: rx error desc3 %08ux\n", d[3]);
		len = 0;
	}
	if(len > max)
		len = max;
	if(len > 0){
		dmacache(1, rxbpa + rxidx*BufSize, len);
		memmove(out, rxbuf + rxidx*BufSize, len);
	}
	/* re-arm and advance the tail */
	descwr(dp, rxbpa + rxidx*BufSize, 0, 0,
	       DescOwn | RdescB1Valid | RdescIc);
	dmacache(0, dpa, DescSize);
	rxidx = (rxidx + 1) % RxRing;
	wr32(DmaCh0RxTail, rxcpa + rxidx*DescSize);
	return len;
}

static u32
parseip(char *s)
{
	u32 v[4];
	char *p;
	int i;

	for(i = 0; i < 4; i++){
		v[i] = strtoul(s, &p, 10);
		if(i < 3 && *p != '.')
			return 0;
		s = p + 1;
	}
	return (v[0]<<24) | (v[1]<<16) | (v[2]<<8) | v[3];
}

static int
arpreq(u8 *f, uchar *src, u32 sip, u32 tip)
{
	memset(f, 0, 64);
	memset(f, 0xff, 6);
	memmove(f + 6, src, 6);
	f[12] = 0x08;
	f[13] = 0x06;
	f[14] = 0x00; f[15] = 0x01;		/* htype ethernet */
	f[16] = 0x08; f[17] = 0x00;		/* ptype IPv4 */
	f[18] = 6; f[19] = 4;
	f[20] = 0x00; f[21] = 0x01;		/* request */
	memmove(f + 22, src, 6);
	f[28] = sip>>24; f[29] = sip>>16; f[30] = sip>>8; f[31] = sip;
	memset(f + 32, 0, 6);
	f[38] = tip>>24; f[39] = tip>>16; f[40] = tip>>8; f[41] = tip;
	return 42;
}

void
main(int argc, char **argv)
{
	int phy, i, n, nf;
	u8 frame[256], in[2048];

	mmio = mapnode("ethernet@fe010000", 0xfe010000);
	grf = mapnode("syscon@fdc60000", 0xfdc60000);
	cru = mapnode("clock-controller@fdd20000", 0xfdd20000);
	devregs();
	if(argc > 1 && strcmp(argv[1], "regs") == 0)
		exits(nil);
	if(argc > 1 && strcmp(argv[1], "mmc") == 0){
		int o;

		for(o = 0x700; o < 0x800; o += 16)
			print("gmac: mmc %03x: %08ux %08ux %08ux %08ux\n",
			      o, rd32(o), rd32(o+4), rd32(o+8), rd32(o+12));
		exits(nil);
	}
	if(argc > 2 && strcmp(argv[1], "mdio") == 0){
		int reg = argc > 3 ? atoi(argv[3]) : 0;

		print("gmac: mdio %d %d = %04x\n", atoi(argv[2]), reg,
		      mdio(atoi(argv[2]), reg, 0, 0));
		exits(nil);
	}
	if(argc > 1 && strncmp(argv[1], "arp", 3) == 0){
		/* arp <tries> <src> <dst> <delay mode> */
		if(argc > 5)
			macdelay = atoi(argv[5]);
	}
	if(argc > 1 && strcmp(argv[1], "tx") == 0){
		/* tx <count> <delay mode> */
		if(argc > 3)
			macdelay = atoi(argv[3]);
	}
	if(argc > 1 && strcmp(argv[1], "arp") == 0 && argc > 6)
		forcespeed = atoi(argv[6]);
	if(argc > 1 && strcmp(argv[1], "tx") == 0 && argc > 4)
		forcespeed = atoi(argv[4]);
	if(argc > 1 && strcmp(argv[1], "tx") == 0 && argc > 5)
		txdelay = strtoul(argv[5], nil, 0);
	if(argc > 1 && strcmp(argv[1], "tx") == 0 && argc > 6)
		rxdelay = strtoul(argv[6], nil, 0);
	if(argc > 1 && strcmp(argv[1], "tx") == 0 && argc > 7)
		clksrc = atoi(argv[7]);
	if(argc > 1 && strcmp(argv[1], "txnr") == 0 && argc > 7)
		clksrc = atoi(argv[7]);
	if(argc > 1 && strcmp(argv[1], "txnr") == 0){
		noreset = 1;
		if(argc > 3)
			macdelay = atoi(argv[3]);
		if(argc > 5)
			txdelay = strtoul(argv[5], nil, 0);
		if(argc > 6)
			rxdelay = strtoul(argv[6], nil, 0);
	}
	if(argc > 1 && strcmp(argv[1], "adv") == 0){
		noreset = 1;
		noinit = 1;
	}
	setrgmii();
	if(!noreset)
		phyreset();
	phy = scanphy(!noreset);
	if(phy < 0){
		print("gmac: no phy found\n");
		exits(nil);
	}
	macphy = phy;
	if(!noinit)
		phyinit8531(phy);
	if(argc > 1 && strcmp(argv[1], "adv") == 0){
		int mb = argc > 2 ? atoi(argv[2]) : 1000;
		int cnt = argc > 3 ? atoi(argv[3]) : 6;
		int st;

		mdio(phy, 0x9, mb == 1000 ? 0x0300 : 0, 1);
		mdio(phy, 0x4, mb == 1000 ? 0x01e1 :
		     mb == 100 ? 0x0181 : 0x0061, 1);
		mdio(phy, MiiBmcr, 0x1200, 1);
		for(i = 0; i < 50; i++){
			st = mdio(phy, 0x11, 0, 0);
			if(st > 0 && (st & (1<<10)))
				break;
			sleep(100);
		}
		speed = (st>>14) == 1 ? 100 : (st>>14) == 2 ? 1000 : 10;
		print("gmac: renegotiated %d Mbps (status %04x)\n", speed, st);
		setspeed(speed);
		dmainit();
		dmacfg();
		for(i = 0; i < cnt; i++){
			memset(frame, 0, sizeof frame);
			memset(frame, 0xff, 6);
			frame[6] = 0x02; frame[11] = 0x01;
			frame[12] = 0x88; frame[13] = 0xb5;
			snprint((char *)frame + 14, sizeof frame - 14, "ADV %d", i);
			n = 60;
			if(txframe(frame, n) < 0)
				print("gmac: adv frame %d failed\n", i);
			sleep(50);
		}
		exits(nil);
	}
	if((forcespeed == 10 || forcespeed == 100) && !noreset){
		/* advertise one speed and renegotiate, like U-Boot does */
		mdio(phy, 0x9, 0, 1);			/* no 1000BASE-T */
		mdio(phy, 0x4, forcespeed == 100 ? 0x0181 : 0x0061, 1);
		mdio(phy, MiiBmcr, 0x1200, 1);		/* AN enable + restart */
		for(i = 0; i < 40; i++){
			int st = mdio(phy, 0x11, 0, 0);

			if(st > 0 && (st & (1<<10)))
				break;
			sleep(100);
		}
		speed = (mdio(phy, 0x11, 0, 0) >> 14) == 1 ? 100 :
			(mdio(phy, 0x11, 0, 0) >> 14) == 2 ? 1000 : 10;
		print("gmac: phy advertised %d Mbps, status %04x\n",
		      forcespeed, mdio(phy, 0x11, 0, 0));
	}
	if(argc > 1 && strcmp(argv[1], "probe") == 0)
		exits(nil);

	setspeed(speed);
	dmainit();
	dmacfg();

	if(argc > 1 && (strcmp(argv[1], "tx") == 0 ||
	                strcmp(argv[1], "txnr") == 0)){
		nf = argc > 2 ? atoi(argv[2]) : 4;
		for(i = 0; i < nf; i++){
			memset(frame, 0, sizeof frame);
			n = 14;
			memset(frame, 0xff, 6);
			frame[6] = 0x02; frame[11] = 0x01;
			frame[12] = 0x88; frame[13] = 0xb5;
			snprint((char *)frame + n, sizeof frame - n,
				"HARVEY GMAC TX %d", i);
			n += strlen((char *)frame + n);
			if(n < 60)
				n = 60;
			if(txframe(frame, n) < 0)
				print("gmac: frame %d failed\n", i);
			else
				print("gmac: frame %d sent (%d bytes)\n", i, n);
			sleep(100);
		}
		exits(nil);
	}
	if(argc > 1 && strcmp(argv[1], "loop") == 0){
		u32 bmcr;
		int k;

		bmcr = mdio(macphy, MiiBmcr, 0, 0);
		mdio(macphy, MiiBmcr, bmcr | (1<<14), 1);	/* loopback */
		sleep(100);
		print("gmac: phy loopback bmcr %04x\n",
		      mdio(macphy, MiiBmcr, 0, 0));
		memset(frame, 0, sizeof frame);
		memset(frame, 0xff, 6);
		frame[6] = 0x02; frame[11] = 0x01;
		frame[12] = 0x88; frame[13] = 0xb5;
		strcpy((char *)frame + 14, "HARVEY GMAC LOOP");
		n = 60;
		if(txframe(frame, n) < 0)
			print("gmac: loop tx failed\n");
		for(k = 0; k < 100; k++){
			n = rxpoll(in, sizeof in);
			if(n > 0){
				print("gmac: loop rx %d bytes type %02x%02x\n",
				      n, in[12], in[13]);
				break;
			}
			sleep(10);
		}
		if(k == 100)
			print("gmac: loopback saw nothing\n");
		mdio(macphy, MiiBmcr, bmcr, 1);
		exits(nil);
	}
	if(argc > 1 && strcmp(argv[1], "arp") == 0){
		uchar me[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
		u32 sip = 0xc0a802fd;		/* 192.168.2.253 */
		u32 tip = 0xc0a80201;		/* 192.168.2.1 */
		int tries;

		tries = argc > 2 ? atoi(argv[2]) : 5;
		if(argc > 3)
			sip = parseip(argv[3]);
		if(argc > 4)
			tip = parseip(argv[4]);
		for(i = 0; i < tries; i++){
			n = arpreq(frame, me, sip, tip);
			if(txframe(frame, n) < 0){
				print("gmac: arp send failed\n");
				exits(nil);
			}
			print("gmac: arp request sent\n");
			for(nf = 0; nf < 100; nf++){
				n = rxpoll(in, sizeof in);
				if(n >= 42 && in[12] == 0x08 && in[13] == 0x06 &&
				   in[20] == 0 && in[21] == 2){
					print("gmac: arp reply %d.%d.%d.%d is %02x:%02x:%02x:%02x:%02x:%02x\n",
					      in[28],in[29],in[30],in[31],
					      in[22],in[23],in[24],in[25],in[26],in[27]);
					exits(nil);
				}
				sleep(10);
			}
			sleep(500);
		}
		print("gmac: no arp reply\n");
		exits(nil);
	}
	if(argc > 1 && strcmp(argv[1], "rx") == 0){
		nf = argc > 2 ? atoi(argv[2]) : 5;
		for(i = 0; i < nf*100; i++){
			n = rxpoll(in, sizeof in);
			if(n > 0)
				print("gmac: rx %d bytes %02x%02x%02x%02x%02x%02x -> %02x%02x%02x%02x%02x%02x type %02x%02x\n",
				      n, in[6],in[7],in[8],in[9],in[10],in[11],
				      in[0],in[1],in[2],in[3],in[4],in[5],
				      in[12],in[13]);
			sleep(10);
		}
		exits(nil);
	}
	exits(nil);
}
