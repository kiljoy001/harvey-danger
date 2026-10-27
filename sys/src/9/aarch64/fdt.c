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
	u8 *compat;
	u32 compatlen, pathlen, uarttype, gicversion;
	u32 clas;
	int memory, uart, gic, timer, virtio;
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
addreserve(Fdtinfo *out, u64 base, u64 size)
{
	if(size == 0)
		return 0;
	if(base+size < base || out->nreserve == Maxfdtreserve)
		return -1;
	out->reserve[out->nreserve].base = base;
	out->reserve[out->nreserve].size = size;
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
	{ "snps,dwc3", Fdtclassusb },
	{ "rockchip,rk3568-dwc3", Fdtclassusb },
	{ "generic-ehci", Fdtclassusb },
	{ "generic-ohci", Fdtclassusb },
	{ "rockchip,rk3568-pcie", Fdtclasspcie },
	{ "snps,dw-pcie", Fdtclasspcie },
	{ "rockchip,rk3568-saradc", Fdtclasssaradc },
	{ "rockchip,rk3568-tsadc", Fdtclasstsadc },
	{ "rockchip,rk3568-wdt", Fdtclasswdt },
	{ "snps,dw-wdt", Fdtclasswdt },
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
		"usb", "pcie", "saradc", "tsadc", "wdt", "i2s", "cru", "grf",
		"npu", "gpu", "vop", "hdmi", "dsi", "edp", "sfc", "vpu",
		"iommu", "power",
	};

	if(clas >= sizeof(name)/sizeof(name[0]))
		return "unknown";
	return name[clas];
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

int
fdtselectram(Fdtinfo *info, u64 imagebase, u64 imageend)
{
	u64 base, end, stop;
	u32 i;

	if(info == nil || imageend <= imagebase)
		return -1;
	for(i = 0; i < info->nmemory; i++){
		end = info->memory[i].base + info->memory[i].size;
		if(end < info->memory[i].base ||
		   imagebase < info->memory[i].base || imageend > end)
			continue;
		if(info->memory[i].base > ~(u64)0 - ((1<<21)-1))
			continue;
		base = (info->memory[i].base + (1<<21)-1) & ~((u64)(1<<21)-1);
		if(base > imagebase)
			continue;
		stop = end;
		stop &= ~((u64)(1<<21)-1);
		if(stop < imageend || stop-base < (1<<21))
			continue;
		info->rambase = base;
		info->ramsize = stop-base;
		return 0;
	}
	return -1;
}

int
fdtparse(void *blob, usize bytes, Fdtinfo *out)
{
	u8 *p, *s, *str, *value;
	u32 total, soff, ssize, stroff, strsize, rsv, pos, token, namelen, len, depth;
	u32 i, off, stride, oldlen, naliases, nconsoles, wantlen;
	u64 base, size;
	Fdtnode nodes[Maxdepth], *node;
	Fdtalias aliases[Maxaliases];
	Fdtconsole consoles[Maxconsoles];
	u8 *stdoutpath, *want;
	u32 stdoutlen;
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
	out->uarttype = out->gicversion = 0;
	out->gicdist = out->giccpu = 0;
	out->timerirq = 0;
	out->nmemory = out->nreserve = out->nvirtio = 0;
	out->ndev = 0;
	for(; rsv <= soff && soff-rsv >= 16; rsv += 16){
		base = get8(p+rsv);
		size = get8(p+rsv+8);
		if(base == 0 && size == 0)
			break;
		if(addreserve(out, base, size) < 0)
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
			node->clas = Fdtclassnone;
			node->memory = depth == 1 && i-pos >= 6 &&
			               s[pos] == 'm' && s[pos+1] == 'e' &&
			               s[pos+2] == 'm' && s[pos+3] == 'o' &&
			               s[pos+4] == 'r' && s[pos+5] == 'y' &&
			               (s[pos+6] == '@' || s[pos+6] == 0);
			node->uart = node->gic = node->timer = node->virtio = 0;
			node->uarttype = node->gicversion = 0;
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
			}else if(same((char *)str+off, "reg")){
				node->reg = value;
				node->reglen = len;
			}else if(same((char *)str+off, "device_type") && len >= 7 &&
			         value[6] == 0 && same((char *)value, "memory"))
				node->memory = 1;
			else if(same((char *)str+off, "compatible")){
				node->compat = value;
				node->compatlen = len;
				node->clas = classify(value, len);
				if(hascompat(value, len, "arm,pl011"))
					node->uarttype = Fdtpl011;
				else if(hascompat(value, len, "snps,dw-apb-uart") ||
				        hascompat(value, len, "rockchip,rk3568-uart"))
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
			}else if(same((char *)str+off, "interrupts")){
				node->intr = value;
				node->intrlen = len;
			}
			break;
		case Fdtendnode:
			if(depth == 0)
				return -1;
			node = &nodes[--depth];
			if(node->reserve && node->reg != nil){
				if(node->parentaddr == 0 || node->parentaddr > 2 ||
				   node->parentsize == 0 || node->parentsize > 2)
					return -1;
				stride = (node->parentaddr+node->parentsize)*4;
				if(node->reglen % stride != 0)
					return -1;
				for(i = 0; i < node->reglen/stride; i++){
					if(regpair(node, i, &base, &size) < 0 ||
					   addreserve(out, base, size) < 0)
						return -1;
				}
			}
			if(node->enabled && node->timer && node->intrlen >= 36 &&
			   get4(node->intr+24) == 1)
				out->timerirq = get4(node->intr+28)+16;
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
					dev->base2 = 0;
					dev->size2 = 0;
					if(node->clas == Fdtclassiommu &&
					   regpair(node, 1, &base, &size) == 0){
						dev->base2 = base;
						dev->size2 = size;
					}
					dev->irq = 0;
					dev->irqflags = 0;
					if(node->intrlen >= 12){
						u32 type, number;

						type = get4(node->intr);
						number = get4(node->intr+4);
						dev->irqflags = get4(node->intr+8);
						dev->irq = type == 0 ? number+32 :
						           type == 1 ? number+16 : 0;
					}
					dev->clas = node->clas;
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
	if(stdoutpath != nil){
		want = stdoutpath;
		for(wantlen = 0; wantlen < stdoutlen && want[wantlen] != ':'; wantlen++)
			;
		if(wantlen == 0)
			return -1;
		if(want[0] != '/'){
			for(i = 0; i < naliases; i++)
				if(samelen(want, wantlen, (char *)aliases[i].name)){
					want = aliases[i].path;
					wantlen = aliases[i].pathlen;
					break;
				}
			if(i == naliases)
				return -1;
		}
		for(i = 0; i < nconsoles; i++)
			if(samelen(want, wantlen, consoles[i].path)){
				out->uartbase = consoles[i].base;
				out->uarttype = consoles[i].type;
				break;
			}
		if(i == nconsoles)
			return -1;
	}else if(nconsoles == 1){
		out->uartbase = consoles[0].base;
		out->uarttype = consoles[0].type;
	}
	return out->nmemory != 0 && out->ramsize != 0 &&
	       out->uartbase != 0 && out->gicdist != 0 &&
	       out->gicversion != 0 && out->timerirq != 0 ? 0 : -1;
}
