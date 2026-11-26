/* Bounded early device-tree discovery for the ARM64 kernel. */
#include "fdt.h"

enum {
	Fdtmagic = 0xd00dfeed,
	Fdtmin = 40,
	Fdtmax = 16 * 1024 * 1024,
	Fdtbegin = 1,
	Fdtendnode = 2,
	Fdtprop = 3,
	Fdtnop = 4,
	Fdtend = 9,
	Maxdepth = 32,
	Maxpath = 256,
	Maxaliases = 64,
	Maxconsoles = 16,
};

typedef struct Fdtnode Fdtnode;
struct Fdtnode {
	u32 addr, size;
	u32 parentaddr, parentsize;
	u8 *reg;
	u32 reglen;
	u8 *intr;
	u32 intrlen;
	u8 *intrext;
	u32 intrextlen;
	u32 phandle;
	u8 *compat;
	u32 compatlen, pathlen, uarttype, gicversion;
	u32 clas;
	u32 fbwidth, fbheight, fbstride, fbbpp;
	char fbformat[16];
	char group[Maxfdtname];
	int fb;
	int memory, uart, gic, timer, virtio;
	int cpu, psci, psci_std;
	u32 method, cpuon;
	int reservedir, reserve, enabled;
};

typedef struct Fdtalias Fdtalias;
struct Fdtalias {
	u8 *name, *path;
	u32 namelen, pathlen;
};

typedef struct Fdtconsole Fdtconsole;
struct Fdtconsole {
	char path[Maxpath];
	u64 base;
	u32 type;
};

static u32
get4(u8 *p)
{
	return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3];
}

static u64
get8(u8 *p)
{
	return (u64)get4(p)<<32 | get4(p+4);
}

static int
addreserve(Fdtinfo *out, u64 base, u64 size, char *name, u32 namelen)
{
	Fdtreserve *r;
	u32 i;

	if(size == 0)
		return 0;
	if(base+size < base || out->nreserve == Maxfdtreserve)
		return -1;
	r = &out->reserve[out->nreserve];
	r->name[0] = 0;
	if(name != nil){
		if(namelen >= Maxfdtname)
			namelen = Maxfdtname-1;
		for(i = 0; i < namelen; i++)
			r->name[i] = name[i];
		r->name[namelen] = 0;
	}
	r->base = base;
	r->size = size;
	out->nreserve++;
	return 0;
}

static int
same(char *a, char *b)
{
	while(*a != 0 && *a == *b){
		a++;
		b++;
	}
	return *a == *b;
}

static int
samelen(u8 *a, u32 n, char *b)
{
	u32 i;

	for(i = 0; i < n && b[i] != 0; i++)
		if(a[i] != b[i])
			return 0;
	return i == n && b[i] == 0;
}

static int
strnlen0(u8 *p, u32 n)
{
	u32 i;

	for(i = 0; i < n; i++)
		if(p[i] == 0)
			return i;
	return -1;
}

static int
hascompat(u8 *p, u32 n, char *want)
{
	u32 i, j;

	for(i = 0; i < n; i = j+1){
		for(j = i; j < n && p[j] != 0; j++)
			;
		if(j == n)
			return 0;
		if(same((char *)p+i, want))
			return 1;
	}
	return 0;
}

typedef struct Fdtclass Fdtclass;
struct Fdtclass {
	char *compat;
	u32 clas;
};

/* Map board peripheral compatible strings to a device class. */
static const Fdtclass classtab[] = {
	{ "snps,dw-apb-uart", Fdtclassuart },
	{ "arm,pl011", Fdtclassuart },
	/* Allwinner H6 (Orange Pi 3) and related sun50i boards. */
	{ "allwinner,sun50i-h6-mmc", Fdtclassmmc },
	{ "allwinner,sun50i-h6-emmc", Fdtclassmmc },
	{ "allwinner,sun50i-a64-mmc", Fdtclassmmc },
	{ "allwinner,sun50i-a64-emmc", Fdtclassmmc },
	{ "allwinner,sun50i-h6-pinctrl", Fdtclassgpio },
	{ "allwinner,sun50i-h6-r-pinctrl", Fdtclassgpio },
	{ "allwinner,sun6i-a31-i2c", Fdtclassi2c },
	{ "allwinner,sun50i-h6-i2c", Fdtclassi2c },
	{ "allwinner,sun50i-h6-pwm", Fdtclasspwm },
	{ "allwinner,sun50i-h6-emac", Fdtclassgmac },
	{ "allwinner,sun8i-h3-emac", Fdtclassgmac },
	{ "rockchip,rk3568-i2c", Fdtclassi2c },
	{ "rockchip,rk3399-i2c", Fdtclassi2c },
	{ "rockchip,rk3568-spi", Fdtclassspi },
	{ "rockchip,rk3399-spi", Fdtclassspi },
	{ "rockchip,gpio-bank", Fdtclassgpio },
	{ "rockchip,rk3568-pwm", Fdtclasspwm },
	{ "rockchip,rk3328-pwm", Fdtclasspwm },
	{ "rockchip,rk3568-gmac", Fdtclassgmac },
	{ "snps,dwmac", Fdtclassgmac },
	{ "rockchip,rk3568-dw-mshc", Fdtclassmmc },
	{ "rockchip,rk3399-dw-mshc", Fdtclassmmc },
	{ "arasan,sdhci-8.9a", Fdtclassmmc },
	{ "rockchip,rk3568-sdhci", Fdtclassmmc },
	{ "rockchip,rk3568-dwcmshc", Fdtclassmmc },
	{ "snps,dwc3", Fdtclassusb },
	{ "qcom,dwc3", Fdtclassusb },
	{ "qcom,dwc-usb3-msm", Fdtclassusb },
	/* QEMU virt's RTC; no driver claims it, so #P's tests use it */
	{ "arm,pl031", Fdtclassrtc },
	{ "rockchip,rk3568-dwc3", Fdtclassusb },
	{ "generic-ehci", Fdtclassusb },
	{ "generic-ohci", Fdtclassusb },
	{ "rockchip,rk3568-pcie", Fdtclasspcie },
	{ "snps,dw-pcie", Fdtclasspcie },
	{ "rockchip,rk3568-saradc", Fdtclasssaradc },
	{ "rockchip,rk3568-tsadc", Fdtclasstsadc },
	{ "rockchip,rk3568-wdt", Fdtclasswdt },
	{ "snps,dw-wdt", Fdtclasswdt },
	{ "rockchip,cryptov2-rng", Fdtclassrng },
	{ "rockchip,rk3568-rng", Fdtclassrng },
	{ "rockchip,trngv1", Fdtclassrng },
	{ "rockchip,rkrng", Fdtclassrng },
	{ "rockchip,cryptov1-rng", Fdtclassrng },
	{ "rockchip,rk3568-i2s", Fdtclassi2s },
	{ "rockchip,rk3568-i2s-tdm", Fdtclassi2s },
	{ "rockchip,rk3568-cru", Fdtclasscru },
	{ "rockchip,rk3568-pmucru", Fdtclasscru },
	{ "rockchip,rk3568-pmu", Fdtclassgrf },
	{ "rockchip,rk3568-grf", Fdtclassgrf },
	{ "rockchip,rk3568-pmugrf", Fdtclassgrf },
	{ "rockchip,rk3568-usb2phy-grf", Fdtclassgrf },
	{ "rockchip,rk3568-pipe-grf", Fdtclassgrf },
	{ "rockchip,rk3568-pipe-phy-grf", Fdtclassgrf },
	{ "rockchip,rk3568-usb2phy", Fdtclassusb },
	{ "rockchip,rk3568-naneng-combphy", Fdtclassusb },
	{ "rockchip,rk3568-rknpu", Fdtclassnpu },
	{ "rockchip,rk3568-gpu", Fdtclassgpu },
	{ "arm,mali-bifrost", Fdtclassgpu },
	{ "rockchip,rk3566-vop", Fdtclassvop },
	{ "rockchip,rk3568-vop", Fdtclassvop },
	{ "rockchip,rk3568-dw-hdmi", Fdtclasshdmi },
	{ "rockchip,rk3568-mipi-dsi", Fdtclassdsi },
	{ "rockchip,rk3568-edp", Fdtclassedp },
	{ "rockchip,sfc", Fdtclasssfc },
	{ "rockchip,rk3568-vpu", Fdtclassvpu },
	{ "rockchip,rk3568-vdec", Fdtclassvpu },
	{ "rockchip,rk3568-vepu", Fdtclassvpu },
	{ "rockchip,rk3568-iommu", Fdtclassiommu },
	{ "rockchip,iommu-v2", Fdtclassiommu },
	{ "rockchip,iommu", Fdtclassiommu },
	{ "rockchip,rk3568-power-controller", Fdtclasspower },
	/* RK3588 uses the same IP blocks with its own compatible strings. */
	{ "rockchip,rk3588-uart", Fdtclassuart },
	{ "rockchip,rk3588-i2c", Fdtclassi2c },
	{ "rockchip,rk3588-spi", Fdtclassspi },
	{ "rockchip,rk3588-gpio-bank", Fdtclassgpio },
	{ "rockchip,rk3588-pwm", Fdtclasspwm },
	{ "rockchip,rk3588-gmac", Fdtclassgmac },
	{ "rockchip,rk3588-dw-mshc", Fdtclassmmc },
	{ "rockchip,rk3588-sdhci", Fdtclassmmc },
	{ "rockchip,rk3588-dwcmshc", Fdtclassmmc },
	{ "rockchip,rk3588-dwc3", Fdtclassusb },
	{ "rockchip,rk3588-usb2phy", Fdtclassusb },
	{ "rockchip,rk3588-naneng-combphy", Fdtclassusb },
	{ "rockchip,rk3588-pcie", Fdtclasspcie },
	{ "rockchip,rk3588-saradc", Fdtclasssaradc },
	{ "rockchip,rk3588-tsadc", Fdtclasstsadc },
	{ "rockchip,rk3588-wdt", Fdtclasswdt },
	{ "rockchip,rk3588-rng", Fdtclassrng },
	{ "rockchip,rk3588-crypto", Fdtclassrng },
	{ "rockchip,rk3588-i2s", Fdtclassi2s },
	{ "rockchip,rk3588-i2s-tdm", Fdtclassi2s },
	{ "rockchip,rk3588-cru", Fdtclasscru },
	{ "rockchip,rk3588-pmucru", Fdtclasscru },
	{ "rockchip,rk3588-pmu", Fdtclassgrf },
	{ "rockchip,rk3588-grf", Fdtclassgrf },
	{ "rockchip,rk3588-pmugrf", Fdtclassgrf },
	{ "rockchip,rk3588-usb2phy-grf", Fdtclassgrf },
	{ "rockchip,rk3588-pipe-grf", Fdtclassgrf },
	{ "rockchip,rk3588-pipe-phy-grf", Fdtclassgrf },
	{ "rockchip,rk3588-rknpu", Fdtclassnpu },
	{ "rockchip,rk3588-mali", Fdtclassgpu },
	{ "arm,mali-valhall-csf", Fdtclassgpu },
	{ "rockchip,rk3588-vop", Fdtclassvop },
	{ "rockchip,rk3588-dw-hdmi", Fdtclasshdmi },
	{ "rockchip,rk3588-mipi-dsi", Fdtclassdsi },
	{ "rockchip,rk3588-edp", Fdtclassedp },
	{ "rockchip,rk3588-sfc", Fdtclasssfc },
	{ "rockchip,rk3588-vpu", Fdtclassvpu },
	{ "rockchip,rk3588-vdec", Fdtclassvpu },
	{ "rockchip,rk3588-vepu", Fdtclassvpu },
	{ "rockchip,rk3588-av1", Fdtclassvpu },
	{ "rockchip,rk3588-iommu", Fdtclassiommu },
	{ "rockchip,rk3588-power-controller", Fdtclasspower },
	/* Qualcomm SM6115 / Bengal (Moto G Power 2021). */
	{ "qcom,bengal-gcc", Fdtclasscru },
	{ "qcom,sm6115-gcc", Fdtclasscru },
	{ "qcom,gcc-sm6115", Fdtclasscru },
	{ "qcom,bengal-dispcc", Fdtclasscru },
	{ "qcom,bengal-gpucc", Fdtclasscru },
	{ "qcom,sm6115-gpucc", Fdtclasscru },
	{ "qcom,kgsl-3d0", Fdtclassgpu },
	{ "qcom,adreno", Fdtclassgpu },
	{ "qcom,kgsl-smmu-v2", Fdtclassiommu },
	{ "qcom,adreno-smmu", Fdtclassiommu },
	{ "qcom,qsmmu-v500", Fdtclassiommu },
	{ "qcom,adreno-gmu-wrapper", Fdtclasspower },
	{ "qcom,wcn3990-wifi", Fdtclassgmac },
	{ "qcom,icnss", Fdtclassgmac },
	{ "qcom,ipa", Fdtclassgmac },
	{ "qcom,cci-v1.2", Fdtclassi2c },
	{ "qcom,cci", Fdtclassi2c },
	{ "qcom,cam-cpas", Fdtclassgpu },
	{ "qcom,csiphy-v2.0.1", Fdtclassgpu },
	{ "qcom,tfe530", Fdtclassgpu },
	{ "qcom,tpgv1", Fdtclassgpu },
	{ "qcom,bengal-pinctrl", Fdtclassgpio },
	{ "qcom,sm6115-pinctrl", Fdtclassgpio },
	{ "qcom,sm6115-tlmm", Fdtclassgpio },
	{ "qcom,spmi-pmic-arb", Fdtclasspower },
	{ "qcom,sdhci-msm-v5", Fdtclassmmc },
	{ "qcom,sm6115-sdhci", Fdtclassmmc },
	{ "qcom,ufshc", Fdtclassmmc },
	{ "qcom,sm6115-ufshc", Fdtclassmmc },
	{ "qcom,qupv3-geni-se", Fdtclassi2c },
	{ "qcom,geni-se-qup", Fdtclassi2c },
	{ "qcom,i2c-geni", Fdtclassi2c },
	{ "qcom,geni-i2c", Fdtclassi2c },
	{ "qcom,spi-geni", Fdtclassspi },
	{ "qcom,geni-spi", Fdtclassspi },
	{ "qcom,msm-rng", Fdtclassrng },
	{ "qcom,prng-ee", Fdtclassrng },
	{ "qcom,tsens24xx", Fdtclasstsadc },
	{ "qcom,sm6115-tsens", Fdtclasstsadc },
	{ "qcom,qusb2phy", Fdtclassusb },
	{ "qcom,sm6115-qusb2-phy", Fdtclassusb },
	{ "qcom,dwc-usb3-msm", Fdtclassusb },
	{ "qcom,sm6115-dwc3", Fdtclassusb },
	{ "qcom,bengal-apcs-hmss-global", Fdtclassgrf },
	{ "qcom,sm6115-apcs-hmss-global", Fdtclassgrf },
	{ "qcom,tcsr-mutex", Fdtclassgrf },
	{ "qcom,rpm-msg-ram", Fdtclassgrf },
	{ "syscon", Fdtclassgrf },
};

static u32
classify(u8 *p, u32 n)
{
	u32 i, j, k;

	for(k = 0; k < sizeof(classtab)/sizeof(classtab[0]); k++){
		for(i = 0; i < n; i = j+1){
			for(j = i; j < n && p[j] != 0; j++)
				;
			if(j == n)
				break;
			if(same((char *)p+i, classtab[k].compat))
				return classtab[k].clas;
		}
	}
	return Fdtclassnone;
}

const char *
fdtclassname(u32 clas)
{
	static const char *name[] = {
		"none", "uart", "i2c", "spi", "gpio", "pwm", "gmac", "mmc",
		"usb", "pcie", "saradc", "tsadc", "wdt", "rng", "i2s", "cru", "grf",
		"npu", "gpu", "vop", "hdmi", "dsi", "edp", "sfc", "vpu",
		"iommu", "power", "rtc",
	};

	if(clas >= sizeof(name)/sizeof(name[0]))
		return "unknown";
	return name[clas];
}

const char *
fdtboardname(u32 board)
{
	switch(board){
	case Fdtboardqemuvirt:
		return "qemu-virt";
	case Fdtboardrk3566:
		return "rk3566";
	case Fdtboardrk3568:
		return "rk3568";
	case Fdtboardrk3588:
		return "rk3588";
	case Fdtboardbengal:
		return "bengal";
	case Fdtboardsun50ih6:
		return "sun50i-h6";
	default:
		return "unknown";
	}
}

static int
cells(u8 *p, u32 n, u32 count, u64 *v)
{
	u32 i;

	if(count == 0 || count > 2 || n < count*4)
		return -1;
	*v = 0;
	for(i = 0; i < count; i++)
		*v = (*v<<32) | get4(p+i*4);
	return 0;
}

static int
regpair(Fdtnode *node, u32 index, u64 *base, u64 *size)
{
	u32 stride, off;

	if(node->parentaddr == 0 || node->parentaddr > 2 ||
	   node->parentsize == 0 || node->parentsize > 2)
		return -1;
	stride = (node->parentaddr+node->parentsize)*4;
	if(index >= node->reglen/stride)
		return -1;
	off = index*stride;
	if(cells(node->reg+off, stride, node->parentaddr, base) < 0 ||
	   cells(node->reg+off+node->parentaddr*4, stride-node->parentaddr*4,
	         node->parentsize, size) < 0)
		return -1;
	return 0;
}

static int
addusable(Fdtinfo *info, u64 base, u64 size)
{
	u64 end, lo, hi;

	if(size == 0)
		return 0;
	end = base + size;
	if(end < base || base > ~(u64)0 - ((1<<21)-1))
		return -1;
	lo = (base + (1<<21)-1) & ~((u64)(1<<21)-1);
	hi = end & ~((u64)(1<<21)-1);
	if(hi <= lo)
		return 0;
	if(info->nusable > 0){
		Fdtmemory *prev = &info->usable[info->nusable-1];
		if(prev->base + prev->size == lo){
			prev->size += hi - lo;
			info->ramtotal += hi - lo;
			return 0;
		}
	}
	if(info->nusable == Maxfdtmemory)
		return -1;
	info->usable[info->nusable].base = lo;
	info->usable[info->nusable].size = hi - lo;
	info->nusable++;
	info->ramtotal += hi - lo;
	return 0;
}

int
fdtselectram(Fdtinfo *info, u64 imagebase, u64 imageend)
{
	u64 end;
	u32 i, img;

	if(info == nil || imageend <= imagebase)
		return -1;
	img = Maxfdtmemory;
	for(i = 0; i < info->nmemory; i++){
		end = info->memory[i].base + info->memory[i].size;
		if(end < info->memory[i].base)
			continue;
		if(imagebase >= info->memory[i].base && imageend <= end){
			img = i;
			break;
		}
	}
	if(img == Maxfdtmemory)
		return -1;
	info->nusable = 0;
	info->ramtotal = 0;
	if(addusable(info, info->memory[img].base, info->memory[img].size) < 0 ||
	   info->nusable == 0)
		return -1;
	if(imagebase < info->usable[0].base ||
	   imageend > info->usable[0].base + info->usable[0].size)
		return -1;
	info->rambase = info->usable[0].base;
	info->ramsize = info->usable[0].size;
	for(i = 0; i < info->nmemory; i++){
		if(i == img)
			continue;
		if(addusable(info, info->memory[i].base, info->memory[i].size) < 0)
			return -1;
	}
	return 0;
}

int
fdtiscompat(Fdtinfo *info, char *want)
{
	char *p;

	if(info == nil || want == nil)
		return 0;
	if(same(info->rootcompat, want))
		return 1;
	for(p = info->rootcompat; *p != 0; p++)
		if(*p == ' ' && same(p+1, want))
			return 1;
	return 0;
}

Fdtdev *
fdtdev(Fdtinfo *info, u32 clas)
{
	u32 i;

	if(info == nil || clas == Fdtclassnone)
		return nil;
	for(i = 0; i < info->ndev; i++)
		if(info->dev[i].clas == clas)
			return &info->dev[i];
	return nil;
}

int
fdtisrockchip(Fdtinfo *info)
{
	if(info == nil)
		return 0;
	return info->board == Fdtboardrk3566 ||
	       info->board == Fdtboardrk3568 ||
	       info->board == Fdtboardrk3588;
}

int
fdtinram(Fdtinfo *info, u64 base, u64 size)
{
	u32 i;
	u64 end;

	if(info == nil || size == 0)
		return 0;
	end = base + size;
	if(end < base)
		return 0;
	for(i = 0; i < info->nusable; i++){
		if(end <= info->usable[i].base + info->usable[i].size &&
		   base >= info->usable[i].base)
			return 1;
	}
	return 0;
}

Fdtdev *
fdtdevcompatn(Fdtinfo *info, char *compat, u32 nth)
{
	u32 i;

	if(info == nil || compat == nil)
		return nil;
	for(i = 0; i < info->ndev; i++)
		if(same(info->dev[i].compat, compat)){
			if(nth == 0)
				return &info->dev[i];
			nth--;
		}
	return nil;
}

Fdtdev *
fdtdevcompat(Fdtinfo *info, char *compat)
{
	return fdtdevcompatn(info, compat, 0);
}

Fdtdev *
fdtdevname(Fdtinfo *info, char *name)
{
	u32 i;

	if(info == nil || name == nil)
		return nil;
	for(i = 0; i < info->ndev; i++)
		if(same(info->dev[i].name, name))
			return &info->dev[i];
	return nil;
}

static int
compatstarts(Fdtinfo *info, char *want)
{
	char *p;
	u32 i, n;

	for(n = 0; want[n] != 0; n++)
		;
	for(p = info->rootcompat; *p != 0; ){
		for(i = 0; i < n && p[i] == want[i]; i++)
			;
		if(i == n)
			return 1;
		while(*p != 0 && *p != ' ')
			p++;
		while(*p == ' ')
			p++;
	}
	return 0;
}

static u32
boardid(Fdtinfo *out)
{
	if(fdtiscompat(out, "linux,dummy-virt") ||
	   compatstarts(out, "qemu,virt"))
		return Fdtboardqemuvirt;
	if(compatstarts(out, "allwinner,sun50i-h6"))
		return Fdtboardsun50ih6;
	if(compatstarts(out, "rockchip,rk3566"))
		return Fdtboardrk3566;
	if(compatstarts(out, "rockchip,rk3568"))
		return Fdtboardrk3568;
	if(compatstarts(out, "rockchip,rk3588"))
		return Fdtboardrk3588;
	if(compatstarts(out, "qcom,bengal") ||
	   compatstarts(out, "qcom,sm6115"))
		return Fdtboardbengal;
	return Fdtboardunknown;
}

int
fdtparse(void *blob, usize bytes, Fdtinfo *out)
{
	u8 *p, *s, *str, *value;
	u32 total, soff, ssize, stroff, strsize, rsv, pos, token, namelen, len, depth;
	u32 i, k, n, off, stride, oldlen, naliases, nconsoles, wantlen;
	u64 base, size, wb, ws;
	Fdtnode nodes[Maxdepth], *node;
	Fdtalias aliases[Maxaliases];
	Fdtconsole consoles[Maxconsoles];
	u8 *stdoutpath, *want;
	u32 stdoutlen, gicphandle;
	char path[Maxpath];
	int ended;

	if(blob == nil || out == nil || bytes < Fdtmin)
		return -1;
	p = blob;
	total = get4(p+4);
	if(get4(p) != Fdtmagic || total < Fdtmin || total > Fdtmax || total > bytes)
		return -1;
	soff = get4(p+8);
	stroff = get4(p+12);
	rsv = get4(p+16);
	strsize = get4(p+32);
	ssize = get4(p+36);
	if(get4(p+20) < 17 || get4(p+24) > 17 ||
	   soff > total || ssize > total-soff || stroff > total ||
	   strsize > total-stroff || (soff&3) != 0 ||
	   rsv < Fdtmin || rsv > soff || (rsv&7) != 0)
		return -1;
	s = p+soff;
	str = p+stroff;
	out->rambase = out->ramsize = out->uartbase = 0;
	out->lklarena = 0;
	out->board = Fdtboardunknown;
	out->model[0] = 0;
	out->rootcompat[0] = 0;
	out->bootarchbase = out->bootarchsize = 0;
	out->teeshmbase = out->teeshmsize = 0;
	out->scmbase = out->scmsize = 0;
	out->fixrambase = out->fixramsize = 0;
	out->fbbase = out->fbsize = 0;
	out->fbwidth = out->fbheight = out->fbstride = out->fbbpp = 0;
	out->uarttype = out->gicversion = 0;
	out->gicdist = out->giccpu = 0;
	out->giccpusize = 0;
	gicphandle = 0;
	out->timerirq = 0;
	out->ncpu = 0;
	out->psci_method = out->psci_cpuon = 0;
	out->nmemory = out->nreserve = out->nvirtio = 0;
	out->nusable = 0;
	out->ramtotal = 0;
	out->ndev = 0;
	for(; rsv <= soff && soff-rsv >= 16; rsv += 16){
		base = get8(p+rsv);
		size = get8(p+rsv+8);
		if(base == 0 && size == 0)
			break;
		if(addreserve(out, base, size, nil, 0) < 0)
			return -1;
	}
	if(rsv > soff || soff-rsv < 16)
		return -1;
	pos = depth = ended = 0;
	stdoutpath = nil;
	stdoutlen = naliases = nconsoles = 0;
	path[0] = 0;
	while(pos <= ssize && ssize-pos >= 4){
		token = get4(s+pos);
		pos += 4;
		switch(token){
		case Fdtbegin:
			if(depth == Maxdepth)
				return -1;
			for(i = pos; i < ssize && s[i] != 0; i++)
				;
			if(i == ssize)
				return -1;
			node = &nodes[depth];
			oldlen = depth == 0 ? 0 : nodes[depth-1].pathlen;
			if(depth == 0){
				if(i != pos)
					return -1;
				node->pathlen = 0;
			}else if(oldlen < Maxpath && i-pos < Maxpath-oldlen-1){
				path[oldlen] = '/';
				for(off = pos; off < i; off++)
					path[oldlen+1+off-pos] = s[off];
				node->pathlen = oldlen+1+i-pos;
				path[node->pathlen] = 0;
			}else
				node->pathlen = Maxpath;
			node->parentaddr = depth == 0 ? 2 : nodes[depth-1].addr;
			node->parentsize = depth == 0 ? 1 : nodes[depth-1].size;
			node->addr = node->parentaddr;
			node->size = node->parentsize;
			node->reg = nil;
			node->reglen = 0;
			node->intr = nil;
			node->intrlen = 0;
			node->compat = nil;
			node->compatlen = 0;
			node->group[0] = 0;
			node->clas = Fdtclassnone;
			node->memory = depth == 1 && i-pos >= 6 &&
			               s[pos] == 'm' && s[pos+1] == 'e' &&
			               s[pos+2] == 'm' && s[pos+3] == 'o' &&
			               s[pos+4] == 'r' && s[pos+5] == 'y' &&
			               (s[pos+6] == '@' || s[pos+6] == 0);
			node->uart = node->gic = node->timer = node->virtio = 0;
			node->cpu = node->psci = 0;
			node->method = node->cpuon = 0;
			node->uarttype = node->gicversion = 0;
			node->fb = 0;
			node->fbwidth = node->fbheight = node->fbstride = node->fbbpp = 0;
			node->fbformat[0] = 0;
			node->intrext = nil;
			node->intrextlen = 0;
			node->phandle = 0;
			node->enabled = depth == 0 || nodes[depth-1].enabled;
			node->reservedir = depth == 1 && same((char *)s+pos, "reserved-memory");
			node->reserve = depth != 0 && nodes[depth-1].reservedir;
			pos = (i+4)&~3U;
			if(pos > ssize)
				return -1;
			depth++;
			break;
		case Fdtprop:
			if(depth == 0 || ssize-pos < 8)
				return -1;
			len = get4(s+pos);
			off = get4(s+pos+4);
			pos += 8;
			if(off >= strsize || len > ssize-pos ||
			   ((len+3)&~3U) > ssize-pos)
				return -1;
			for(namelen = off; namelen < strsize && str[namelen] != 0; namelen++)
				;
			if(namelen == strsize)
				return -1;
			value = s+pos;
			pos += (len+3)&~3U;
			node = &nodes[depth-1];
			if(same((char *)str+off, "#address-cells") && len == 4)
				node->addr = get4(value);
			else if(same((char *)str+off, "#size-cells") && len == 4)
				node->size = get4(value);
			else if(same((char *)str+off, "status")){
				if(strnlen0(value, len) < 0)
					return -1;
				if(!same((char *)value, "okay") &&
				   !same((char *)value, "ok"))
					node->enabled = 0;
			}else if(depth == 2 && node->pathlen < Maxpath &&
			         same(path, "/chosen") &&
			         (same((char *)str+off, "stdout-path") ||
			          (stdoutpath == nil && same((char *)str+off, "linux,stdout-path")))){
				if(len == 0 || strnlen0(value, len) < 0)
					return -1;
				stdoutpath = value;
				stdoutlen = strnlen0(value, len);
			}else if(depth == 2 && node->pathlen < Maxpath &&
			         same(path, "/chosen") &&
			         same((char *)str+off, "harvey,lkl-arena-size")){
				/* Total DMA arena the LKL instances in this
				 * image need; stamped by the pack tool from
				 * the family table, not a board constant. */
				if(len == 8)
					out->lklarena = get8(value);
			}else if(depth == 2 && node->pathlen < Maxpath &&
			         same(path, "/chosen") &&
			         same((char *)str+off, "harvey,bootarchive") && len == 16){
				out->bootarchbase = get8(value);
				out->bootarchsize = get8(value+8);
			}else if(depth == 2 && node->pathlen < Maxpath &&
			         same(path, "/chosen") &&
			         same((char *)str+off, "harvey,teeshm") && len == 16){
				out->teeshmbase = get8(value);
				out->teeshmsize = get8(value+8);
			}else if(depth == 2 && node->pathlen < Maxpath &&
			         same(path, "/chosen") &&
			         same((char *)str+off, "harvey,scm") && len == 16){
				out->scmbase = get8(value);
				out->scmsize = get8(value+8);
			}else if(depth == 2 && node->pathlen < Maxpath &&
			         same(path, "/chosen") &&
			         same((char *)str+off, "harvey,ram") && len == 16){
				out->fixrambase = get8(value);
				out->fixramsize = get8(value+8);
			}else if(depth == 2 && node->pathlen < Maxpath &&
			         same(path, "/aliases")){
				if(len == 0 || strnlen0(value, len) < 0)
					return -1;
				if(naliases < Maxaliases){
					aliases[naliases].name = str+off;
					aliases[naliases].namelen = namelen-off;
					aliases[naliases].path = value;
					aliases[naliases].pathlen = strnlen0(value, len);
					naliases++;
				}
			}else if(same((char *)str+off, "harvey,group") &&
			         len > 0 && len < Maxfdtname){
				u32 k;

				for(k = 0; k < len && value[k] != 0; k++)
					node->group[k] = value[k];
				node->group[k] = 0;
			}else if(same((char *)str+off, "reg")){
				node->reg = value;
				node->reglen = len;
			}else if(same((char *)str+off, "width") && len == 4){
				node->fbwidth = get4(value);
			}else if(same((char *)str+off, "height") && len == 4){
				node->fbheight = get4(value);
			}else if(same((char *)str+off, "stride") && len == 4){
				node->fbstride = get4(value);
			}else if(same((char *)str+off, "format") && len >= 2){
				u32 k;

				for(k = 0; k < len && k < sizeof(node->fbformat)-1 &&
				    value[k] != 0; k++)
					node->fbformat[k] = value[k];
				node->fbformat[k] = 0;
				node->fbbpp = 32;
				if(same(node->fbformat, "a8b8g8r8") ||
				   same(node->fbformat, "x8r8g8b8") ||
				   same(node->fbformat, "x8b8g8r8") ||
				   same(node->fbformat, "a8r8g8b8"))
					node->fbbpp = 32;
				else if(same(node->fbformat, "r5g6b5"))
					node->fbbpp = 16;
				else if(same(node->fbformat, "r8g8b8"))
					node->fbbpp = 24;
			}else if(same((char *)str+off, "device_type") && len >= 7 &&
			         value[6] == 0 && same((char *)value, "memory"))
				node->memory = 1;
			else if(same((char *)str+off, "device_type") && len >= 4 &&
			         value[3] == 0 && same((char *)value, "cpu"))
				node->cpu = 1;
			else if(same((char *)str+off, "method") && len >= 4){
				if(same((char *)value, "hvc"))
					node->method = 1;
				else if(same((char *)value, "smc"))
					node->method = 2;
			}else if(same((char *)str+off, "cpu_on") && len == 4)
				node->cpuon = get4(value);
			else if(depth == 1 && same((char *)str+off, "model")){
				u32 k;

				for(k = 0; k < len && value[k] != 0 &&
				    k < sizeof(out->model)-1; k++)
					out->model[k] = value[k];
				out->model[k] = 0;
			}else if(same((char *)str+off, "compatible")){
				node->compat = value;
				node->compatlen = len;
				node->clas = classify(value, len);
				if(depth == 1){
					u32 k, o;

					o = 0;
					for(k = 0; k < len && o < sizeof(out->rootcompat)-1; k++)
						out->rootcompat[o++] = value[k] == 0 ? ' ' : value[k];
					while(o > 0 && out->rootcompat[o-1] == ' ')
						o--;
					out->rootcompat[o] = 0;
				}
				if(hascompat(value, len, "simple-framebuffer") ||
				   hascompat(value, len, "rpi,simple-framebuffer") ||
				   hascompat(value, len, "allwinner,simple-framebuffer"))
					node->fb = 1;
				if(hascompat(value, len, "arm,pl011"))
					node->uarttype = Fdtpl011;
				else if(hascompat(value, len, "snps,dw-apb-uart") ||
				        hascompat(value, len, "rockchip,rk3568-uart") ||
				        hascompat(value, len, "rockchip,rk3588-uart"))
					node->uarttype = Fdtdwuart;
				node->uart = node->uarttype != 0;
				if(hascompat(value, len, "arm,gic-v3"))
					node->gicversion = 3;
				else if(hascompat(value, len, "arm,cortex-a15-gic") ||
				        hascompat(value, len, "arm,gic-400"))
					node->gicversion = 2;
				node->gic = hascompat(value, len, "arm,cortex-a15-gic") ||
				            hascompat(value, len, "arm,gic-400") ||
				            hascompat(value, len, "arm,gic-v3");
				node->timer = hascompat(value, len, "arm,armv8-timer");
				node->virtio = hascompat(value, len, "virtio,mmio");
				node->psci = hascompat(value, len, "arm,psci-1.0") ||
				             hascompat(value, len, "arm,psci-0.2") ||
				             hascompat(value, len, "arm,psci");
				/* PSCI 0.2 and later define CPU_ON in the
				 * specification, so the node need not carry
				 * the legacy cpu_on property. */
				node->psci_std = hascompat(value, len, "arm,psci-1.0") ||
				                 hascompat(value, len, "arm,psci-0.2");
			}else if(same((char *)str+off, "interrupts")){
				node->intr = value;
				node->intrlen = len;
			}else if(same((char *)str+off, "interrupts-extended")){
				node->intrext = value;
				node->intrextlen = len;
			}else if(same((char *)str+off, "phandle") && len == 4){
				node->phandle = get4(value);
			}
			break;
		case Fdtendnode:
			if(depth == 0)
				return -1;
			node = &nodes[--depth];
			if(node->cpu && node->enabled && out->ncpu < Fdtmaxcpu){
				if(node->reg != nil && node->reglen >= 8)
					out->cpumpidr[out->ncpu] = get8(node->reg);
				else if(node->reg != nil && node->reglen >= 4)
					out->cpumpidr[out->ncpu] = get4(node->reg);
				else
					out->cpumpidr[out->ncpu] = out->ncpu;
				out->ncpu++;
			}
			if(node->psci){
				if(node->method != 0)
					out->psci_method = node->method;
				if(node->cpuon != 0)
					out->psci_cpuon = node->cpuon;
				if(node->psci_std)
					out->psci_std = 1;
			}
			if(node->reserve && node->reg != nil){
				u32 b, n, baselen;

				if(node->parentaddr == 0 || node->parentaddr > 2 ||
				   node->parentsize == 0 || node->parentsize > 2)
					return -1;
				stride = (node->parentaddr+node->parentsize)*4;
				if(node->reglen % stride != 0)
					return -1;
				b = 0;
				for(n = 0; n < node->pathlen; n++)
					if(path[n] == '/')
						b = n+1;
				baselen = node->pathlen - b;
				for(i = 0; i < node->reglen/stride; i++){
					if(regpair(node, i, &base, &size) < 0 ||
					   addreserve(out, base, size, path+b, baselen) < 0)
						return -1;
				}
			}
			if(node->enabled && node->timer && node->intrlen >= 36 &&
			   get4(node->intr+24) == 1)
				out->timerirq = get4(node->intr+28)+16;
			if(node->enabled && node->fb && node->reg != nil &&
			   regpair(node, 0, &base, &size) == 0){
				out->fbbase = base;
				out->fbsize = size;
				out->fbwidth = node->fbwidth;
				out->fbheight = node->fbheight;
				out->fbstride = node->fbstride;
				out->fbbpp = node->fbbpp;
			}
			if(node->enabled && node->reg != nil &&
			   regpair(node, 0, &base, &size) == 0){
				if(node->memory){
					if(node->parentaddr == 0 || node->parentaddr > 2 ||
					   node->parentsize == 0 || node->parentsize > 2)
						return -1;
					stride = (node->parentaddr+node->parentsize)*4;
					if(node->reglen % stride != 0)
						return -1;
					for(i = 0; i < node->reglen/stride; i++){
						if(regpair(node, i, &base, &size) < 0 ||
						   base+size < base || out->nmemory == Maxfdtmemory)
							return -1;
						if(size == 0)
							continue;
						out->memory[out->nmemory].base = base;
						out->memory[out->nmemory].size = size;
						out->nmemory++;
						if(out->ramsize == 0){
							out->rambase = base;
							out->ramsize = size;
						}
					}
				}
				if(node->uart && node->pathlen < Maxpath){
					if(nconsoles == Maxconsoles)
						return -1;
					for(i = 0; i <= node->pathlen; i++)
						consoles[nconsoles].path[i] = path[i];
					consoles[nconsoles].base = base;
					consoles[nconsoles].type = node->uarttype;
					nconsoles++;
				}
				if(node->gic && out->gicdist == 0 &&
				   regpair(node, 1, &out->giccpu, &size) == 0){
					out->gicdist = base;
					out->gicversion = node->gicversion;
					out->giccpusize = size;
					gicphandle = node->phandle;
				}
				if(node->virtio){
					if(out->nvirtio == Maxfdtvirtio || size < 0x200 ||
					   base+size < base)
						return -1;
					out->virtio[out->nvirtio].base = base;
					out->virtio[out->nvirtio].size = size;
					out->nvirtio++;
				}
				if(node->clas != Fdtclassnone &&
				   out->ndev < Maxfdtdev && size != 0){
					Fdtdev *dev;
					u32 b, n, baselen;

					dev = &out->dev[out->ndev];
					b = 0;
					for(n = 0; n < node->pathlen; n++)
						if(path[n] == '/')
							b = n+1;
					baselen = node->pathlen - b;
					if(baselen >= Maxfdtname)
						baselen = Maxfdtname-1;
					for(n = 0; n < baselen; n++)
						dev->name[n] = path[b+n];
					dev->name[baselen] = 0;
					for(n = 0; n < node->compatlen &&
					    n < Maxfdtcompat-1 && node->compat[n] != 0; n++)
						dev->compat[n] = node->compat[n];
					dev->compat[n] = 0;
					dev->base = base;
					dev->size = size;
					if(same(dev->compat, "qcom,ipa") && dev->size < 0x80000)
						dev->size = 0x80000;
					/*
					 * Every reg range: a compound node can put
					 * its functions in separate windows (a
					 * display controller and its IOMMU, a
					 * controller and a second block).
					 */
					dev->nwin = 1;
					for(k = 1; k < Maxfdtwin; k++){
						if(regpair(node, k, &wb, &ws) < 0 ||
						   ws == 0)
							break;
						dev->wbase[k-1] = wb;
						dev->wsize[k-1] = ws;
						dev->nwin++;
					}
					if(node->clas == Fdtclasstsadc &&
					   regpair(node, 1, &base, &size) == 0 &&
					   base >= dev->base &&
					   base + size - dev->base <= 64*1024){
						dev->size = (base + size) - dev->base;
						dev->nwin = 1;	/* merged into window 0 */
					}
					if(node->clas == Fdtclasspower){
						u64 rbase, rsize, minb, maxe;
						u32 ridx;

						minb = dev->base;
						maxe = dev->base + dev->size;
						for(ridx = 1; ridx < 8 && regpair(node, ridx, &rbase, &rsize) == 0; ridx++){
							if(rbase < minb)
								minb = rbase;
							if(rbase + rsize > maxe)
								maxe = rbase + rsize;
						}
						dev->base = minb;
						dev->size = maxe - minb;
						dev->nwin = 1;	/* merged into window 0 */
					}
					dev->irq = 0;
					dev->irqflags = 0;
					dev->irq2 = 0;
					if(node->intrlen >= 12){
						u32 type, number;

						type = get4(node->intr);
						number = get4(node->intr+4);
						dev->irqflags = get4(node->intr+8);
						dev->irq = type == 0 ? number+32 :
						           type == 1 ? number+16 : 0;
					}
					if(node->intrlen >= 24){
						u32 type, number;

						type = get4(node->intr+12);
						number = get4(node->intr+16);
						dev->irq2 = type == 0 ? number+32 :
						            type == 1 ? number+16 : 0;
					}
					if(dev->irq == 0 && gicphandle != 0 &&
					   node->intrext != nil){
						u32 eoff;

						for(eoff = 0; eoff + 16 <= node->intrextlen; eoff += 16){
							u32 type, number;

							if(get4(node->intrext+eoff) != gicphandle)
								break;
							type = get4(node->intrext+eoff+4);
							number = get4(node->intrext+eoff+8);
							if(dev->irq == 0){
								dev->irqflags = get4(node->intrext+eoff+12);
								dev->irq = type == 0 ? number+32 :
								           type == 1 ? number+16 : 0;
							}else if(dev->irq2 == 0){
								dev->irq2 = type == 0 ? number+32 :
								            type == 1 ? number+16 : 0;
								break;
							}
						}
					}
					for(b = 0; b < sizeof dev->group - 1 &&
					    node->group[b] != 0; b++)
						dev->group[b] = node->group[b];
					dev->group[b] = 0;
					dev->clas = node->clas;
					dev->depth = depth;
					dev->parent = ~0u;
					out->ndev++;
				}
			}
			if(depth != 0 && nodes[depth-1].pathlen < Maxpath)
				path[nodes[depth-1].pathlen] = 0;
			else
				path[0] = 0;
			break;
		case Fdtnop:
			break;
		case Fdtend:
			if(depth != 0)
				return -1;
			ended = 1;
			goto done;
		default:
			return -1;
		}
	}
done:
	if(!ended)
		return -1;
	/*
	 * Parents, now that every device is known.  Devices are appended in
	 * post-order, so after a device the first one shallower than it is
	 * its nearest enclosing device: everything in between is inside that
	 * node's subtree.
	 */
	for(i = 0; i < out->ndev; i++){
		for(n = i + 1; n < out->ndev; n++){
			if(out->dev[n].depth < out->dev[i].depth){
				out->dev[i].parent = n;
				break;
			}
		}
	}
	if(stdoutpath != nil){
		want = stdoutpath;
		for(wantlen = 0; wantlen < stdoutlen && want[wantlen] != ':'; wantlen++)
			;
		if(wantlen != 0){
			if(want[0] != '/'){
				for(i = 0; i < naliases; i++)
					if(samelen(want, wantlen, (char *)aliases[i].name)){
						want = aliases[i].path;
						wantlen = aliases[i].pathlen;
						break;
					}
			}
			for(i = 0; i < nconsoles; i++)
				if(samelen(want, wantlen, consoles[i].path)){
					out->uartbase = consoles[i].base;
					out->uarttype = consoles[i].type;
					break;
				}
		}
	}else if(nconsoles == 1){
		out->uartbase = consoles[0].base;
		out->uarttype = consoles[0].type;
	}
	/*
	 * Some firmware passes a board DTB with no /memory node (U-Boot
	 * normally adds it).  Fall back to /chosen/harvey,ram, which the
	 * image packer can stamp.
	 */
	if(out->nmemory == 0 && out->fixramsize != 0 &&
	   out->fixrambase + out->fixramsize > out->fixrambase){
		out->memory[0].base = out->fixrambase;
		out->memory[0].size = out->fixramsize;
		out->nmemory = 1;
		out->rambase = out->fixrambase;
		out->ramsize = out->fixramsize;
	}
	out->board = boardid(out);
	return out->nmemory != 0 && out->ramsize != 0 &&
	       out->gicdist != 0 &&
	       out->gicversion != 0 && out->timerirq != 0 ? 0 : -1;
}

enum {
	Winalign = 2 * 1024 * 1024,	/* a physseg covers whole big pages */
	Winspan = 64 * 1024 * 1024,	/* beyond this, map window 0 only */
};

/*
 * The region a claim maps for a device: the union of its windows, big-
 * page aligned so a physseg can cover it, and the number of windows it
 * covers.  A device whose windows are spread further apart than Winspan
 * maps as its first window alone; its windows file still lists them all.
 */
u32
fdtdevmap(Fdtdev *d, u64 *base, u64 *size)
{
	u64 b, e, wb, ws;
	u32 i;

	if(d->nwin == 0){
		*base = 0;
		*size = 0;
		return 0;
	}
	b = d->base;
	e = d->base + d->size;
	for(i = 1; i < d->nwin; i++){
		fdtdevwin(d, i, &wb, &ws);
		if(wb < b)
			b = wb;
		if(wb + ws > e)
			e = wb + ws;
	}
	b &= ~(u64)(Winalign - 1);
	e = (e + Winalign - 1) & ~(u64)(Winalign - 1);
	if(e - b > Winspan){
		*base = d->base & ~(u64)(Winalign - 1);
		*size = ((d->base + d->size + Winalign - 1) &
			 ~(u64)(Winalign - 1)) - *base;
		return 1;
	}
	*base = b;
	*size = e - b;
	return i;
}
