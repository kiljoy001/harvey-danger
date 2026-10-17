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
};

typedef struct Fdtnode Fdtnode;
struct Fdtnode {
	u32 addr, size;
	u32 parentaddr, parentsize;
	u8 *reg;
	u32 reglen;
	u8 *intr;
	u32 intrlen;
	int memory, uart, gic, timer, virtio;
	int reservedir, reserve;
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
fdtparse(void *blob, usize bytes, Fdtinfo *out)
{
	u8 *p, *s, *str, *value;
	u32 total, soff, ssize, stroff, strsize, rsv, pos, token, namelen, len, depth;
	u32 i, off, stride;
	u64 base, size;
	Fdtnode nodes[Maxdepth], *node;
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
	out->gicdist = out->giccpu = 0;
	out->timerirq = 0;
	out->nreserve = 0;
	out->nvirtio = 0;
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
			node->parentaddr = depth == 0 ? 2 : nodes[depth-1].addr;
			node->parentsize = depth == 0 ? 1 : nodes[depth-1].size;
			node->addr = node->parentaddr;
			node->size = node->parentsize;
			node->reg = nil;
			node->reglen = 0;
			node->intr = nil;
			node->intrlen = 0;
			node->memory = depth == 1 && i-pos >= 7 &&
			               s[pos] == 'm' && s[pos+1] == 'e' &&
			               s[pos+2] == 'm' && s[pos+3] == 'o' &&
			               s[pos+4] == 'r' && s[pos+5] == 'y' &&
			               (s[pos+6] == '@' || s[pos+6] == 0);
			node->uart = node->gic = node->timer = node->virtio = 0;
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
			else if(same((char *)str+off, "reg")){
				node->reg = value;
				node->reglen = len;
			}else if(same((char *)str+off, "device_type") && len >= 7 &&
			         value[6] == 0 && same((char *)value, "memory"))
				node->memory = 1;
			else if(same((char *)str+off, "compatible")){
				node->uart = hascompat(value, len, "arm,pl011");
				node->gic = hascompat(value, len, "arm,cortex-a15-gic");
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
			if(node->timer && node->intrlen >= 36 &&
			   get4(node->intr+24) == 1)
				out->timerirq = get4(node->intr+28)+16;
			if(node->reg != nil && regpair(node, 0, &base, &size) == 0){
				if(node->memory && out->ramsize == 0){
					out->rambase = base;
					out->ramsize = size;
				}
				if(node->uart && out->uartbase == 0)
					out->uartbase = base;
				if(node->gic && out->gicdist == 0 &&
				   regpair(node, 1, &out->giccpu, &size) == 0)
					out->gicdist = base;
				if(node->virtio){
					if(out->nvirtio == Maxfdtvirtio || size < 0x200 ||
					   base+size < base)
						return -1;
					out->virtio[out->nvirtio].base = base;
					out->virtio[out->nvirtio].size = size;
					out->nvirtio++;
				}
			}
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
	return ended && out->ramsize != 0 && out->uartbase != 0 ? 0 : -1;
}
