/*
 * gmac: RK3566 GMAC (snps,dwmac-4.20a) bring-up.
 *
 * Runs against the #P grant for ethernet@fe010000.  Phase 1 is the
 * register path: map the MAC, check that its clocks are alive, program
 * the GRF for RGMII, reset the PHY through gpiofs and read its identity
 * and link state over MDIO.
 *
 *   gmac probe            do all of the above
 *   gmac regs             dump MAC and GRF registers
 *   gmac mdio <phy> <reg> read one MDIO register
 */
#include <u.h>
#include <libc.h>

enum {
	/* dwmac4 MAC registers */
	GmacConfig	= 0x000,
	GmacFeature0	= 0x11c,
	GmacFeature1	= 0x120,
	GmacFeature2	= 0x124,
	GmacFeature3	= 0x128,
	MdioAddr	= 0x200,
	MdioData	= 0x204,

	/* GRF syscon@fdc60000 */
	Gmac1Con0	= 0x388,
	Gmac1Con1	= 0x38c,

	MiiBusy		= 1<<0,
	MiiRead		= 3<<2,
	MiiWrite	= 1<<2,

	/* standard PHY registers */
	MiiBmcr		= 0,
	MiiBmsr		= 1,
	MiiPhysid1	= 2,
	MiiPhysid2	= 3,
};

static u8 *mmio, *grf;
static int csr = 1;		/* MDC clock range: 100-150 MHz CSR clock */

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

/* Clause 22 MDIO through the dwmac4 MAC_MDIO_ADDRESS/DATA pair. */
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
	for(i = 0; i < 100000; i++){
		if((rd32(MdioAddr) & MiiBusy) == 0)
			return rd32(MdioData) & 0xffff;
	}
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
	print("gmac: mdio addr %08ux data %08ux\n",
	      rd32(MdioAddr), rd32(MdioData));
}

/*
 * RK3568 GMAC1 RGMII: CON1 selects the PHY interface (bit 4 set,
 * 5/6 clear).  The board runs phy-mode "rgmii-id", so the PHY adds the
 * delays and the MAC delay bits stay clear.
 */
static void
setrgmii(void)
{
	u32 v;

	v = (1<<4) | (1<<20);		/* GRF_BIT(4): select RGMII */
	v |= (1<<21) | (1<<22);		/* GRF_CLR_BIT(5), GRF_CLR_BIT(6) */
	gwr(Gmac1Con1, v);
	print("gmac: grf gmac1 con1 -> %08ux\n", grd(Gmac1Con1));
}

static int
scanphy(void)
{
	int p, id1, id2, bmsr, bmcr, found, spec, i;

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
	mdio(found, MiiBmcr, bmcr | (1<<9), 1);		/* restart autoneg */
	for(i = 0; i < 60; i++){
		bmsr = mdio(found, MiiBmsr, 0, 0);
		bmsr = mdio(found, MiiBmsr, 0, 0);	/* link is latching */
		if((bmsr & (1<<5)) && (bmsr & (1<<2)))
			break;
		sleep(100);
	}
	spec = mdio(found, 0x11, 0, 0);
	print("gmac: phy %d bmcr %04ux bmsr %04ux link %d aneg %d\n",
	      found, bmcr, bmsr, (bmsr>>2)&1, (bmsr>>5)&1);
	if(spec >= 0)
		print("gmac: phy status %04ux link %d speed %d duplex %d\n",
		      spec, (spec>>10)&1,
		      (spec>>14) == 0 ? 10 : (spec>>14) == 1 ? 100 : 1000,
		      (spec>>13)&1);
	return found;
}

void
main(int argc, char **argv)
{
	int phy;

	mmio = mapnode("ethernet@fe010000", 0xfe010000);
	grf = mapnode("syscon@fdc60000", 0xfdc60000);
	devregs();
	if(argc > 1 && strcmp(argv[1], "regs") == 0)
		exits(nil);
	if(argc > 2 && strcmp(argv[1], "mdio") == 0){
		int reg = argc > 3 ? atoi(argv[3]) : 0;

		print("gmac: mdio %d %d = %04x\n", atoi(argv[2]), reg,
		      mdio(atoi(argv[2]), reg, 0, 0));
		exits(nil);
	}
	setrgmii();
	phyreset();
	phy = scanphy();
	if(phy < 0)
		print("gmac: no phy found\n");
	exits(nil);
}
