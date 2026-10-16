/*
 * iputil.c - IP address parsing, byte-order and formatting utilities
 * Kernel implementation for Harvey microkernel.
 */

#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#include "ip.h"

u8 v4prefix[IPaddrlen] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0
};

u8 IPnoaddr[IPaddrlen] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

u8 IPv4bcast[IPaddrlen] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

u8 IPv4bcastobs[IPaddrlen] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0
};

u8 IPv4allsys[IPaddrlen] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 224, 0, 0, 1
};

u8 IPv4allrouter[IPaddrlen] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 224, 0, 0, 2
};

u8 IPallbits[IPaddrlen] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

void
hnputv(void *p, u64 v)
{
	u8 *a = p;
	a[0] = v >> 56;
	a[1] = v >> 48;
	a[2] = v >> 40;
	a[3] = v >> 32;
	a[4] = v >> 24;
	a[5] = v >> 16;
	a[6] = v >> 8;
	a[7] = v;
}

void
hnputl(void *p, u32 v)
{
	u8 *a = p;
	a[0] = v >> 24;
	a[1] = v >> 16;
	a[2] = v >> 8;
	a[3] = v;
}

void
hnputs(void *p, u16 v)
{
	u8 *a = p;
	a[0] = v >> 8;
	a[1] = v;
}

u64
nhgetv(void *p)
{
	u8 *a = p;
	return ((u64)a[0] << 56) | ((u64)a[1] << 48) | ((u64)a[2] << 40) | ((u64)a[3] << 32)
		| ((u64)a[4] << 24) | ((u64)a[5] << 16) | ((u64)a[6] << 8) | a[7];
}

u32
nhgetl(void *p)
{
	u8 *a = p;
	return ((u32)a[0] << 24) | ((u32)a[1] << 16) | ((u32)a[2] << 8) | a[3];
}

u16
nhgets(void *p)
{
	u8 *a = p;
	return (a[0] << 8) | a[1];
}

int
isv4(u8 *ip)
{
	return memcmp(ip, v4prefix, IPv4off) == 0;
}

void
v4tov6(u8 *v6, u8 *v4)
{
	memmove(v6, v4prefix, IPv4off);
	memmove(v6 + IPv4off, v4, IPv4addrlen);
}

int
v6tov4(u8 *v4, u8 *v6)
{
	if(memcmp(v6, v4prefix, IPv4off) == 0){
		memmove(v4, v6 + IPv4off, IPv4addrlen);
		return 0;
	}
	memset(v4, 0, IPv4addrlen);
	return -1;
}

void
maskip(u8 *from, u8 *mask, u8 *to)
{
	for(int i = 0; i < IPaddrlen; i++)
		to[i] = from[i] & mask[i];
}

static u8 defmask4_a[IPaddrlen] = {
	0,0,0,0, 0,0,0,0, 0,0,0xff,0xff, 0xff,0,0,0
};
static u8 defmask4_b[IPaddrlen] = {
	0,0,0,0, 0,0,0,0, 0,0,0xff,0xff, 0xff,0xff,0,0
};
static u8 defmask4_c[IPaddrlen] = {
	0,0,0,0, 0,0,0,0, 0,0,0xff,0xff, 0xff,0xff,0xff,0
};

u8*
defmask(u8 *ip)
{
	if(isv4(ip)){
		u8 b = ip[IPv4off];
		if((b & 0x80) == 0)
			return defmask4_a;
		if((b & 0xC0) == 0x80)
			return defmask4_b;
		return defmask4_c;
	}
	return IPallbits;
}

#define CLASS_BYTE(p) ((*(u8*)(p) >> 6) & 3)

char*
v4parseip(u8 *to, char *from)
{
	int i;
	char *p = from;

	for(i = 0; i < 4 && *p; i++){
		to[i] = strtoul(p, &p, 0);
		if(*p == '.')
			p++;
	}
	switch(CLASS_BYTE(to)){
	case 0:
	case 1:
		if(i == 3){
			to[3] = to[2];
			to[2] = to[1];
			to[1] = 0;
		} else if(i == 2){
			to[3] = to[1];
			to[1] = 0;
		}
		break;
	case 2:
		if(i == 3){
			to[3] = to[2];
			to[2] = 0;
		}
		break;
	}
	return p;
}

static int
ipcharok(int c)
{
	if(c == '.' || c == ':')
		return 1;
	if((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
		return 1;
	return 0;
}

static int
delimchar(int c)
{
	if(c == '\0')
		return 1;
	if(c == '.' || c == ':')
		return 0;
	if((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
		return 0;
	return 1;
}

i64
parseip(u8 *to, char *from)
{
	int i, elipsis = 0, v4 = 1;
	u32 x;
	char *p, *op;

	memset(to, 0, IPaddrlen);
	p = from;
	for(i = 0; i < IPaddrlen && ipcharok(*p); i += 2){
		op = p;
		x = strtoul(p, &p, 16);
		if((*p == '.' && i <= IPaddrlen - 4) || (*p == 0 && i == 0)){
			p = v4parseip(to + i, op);
			i += 4;
			break;
		}
		if(x > 0xFFFFU || (*p != ':' && !delimchar(*p))){
			memset(to, 0, IPaddrlen);
			return -1;
		}
		to[i] = x >> 8;
		to[i+1] = x;
		if(*p == ':'){
			v4 = 0;
			if(*++p == ':'){
				if(elipsis){
					memset(to, 0, IPaddrlen);
					return -1;
				}
				elipsis = i + 2;
				p++;
			}
		} else if(p == op)
			break;
	}
	if(p == from || !delimchar(*p)){
		memset(to, 0, IPaddrlen);
		return -1;
	}
	if(i < IPaddrlen){
		memmove(&to[elipsis + IPaddrlen - i], &to[elipsis], i - elipsis);
		memset(&to[elipsis], 0, IPaddrlen - i);
	}
	if(v4){
		to[10] = to[11] = 0xff;
		return nhgetl(to + IPv4off);
	}
	return 6;
}

i64
parseipmask(u8 *to, char *from)
{
	int i, w;
	i64 x;
	u8 *p;

	if(*from == '/'){
		i = strtoul(from + 1, nil, 10);
		if(i < 0) i = 0;
		if(i > 128) i = 128;
		w = i;
		memset(to, 0, IPaddrlen);
		for(p = to; i >= 8; i -= 8)
			*p++ = 0xff;
		if(i > 0)
			*p = ~((1 << (8 - i)) - 1);
		x = nhgetl(to + IPv4off);
		if(w < 8 * (IPaddrlen - IPv4addrlen))
			return 6;
	} else {
		x = parseip(to, from);
		if(x != -1)
			x = (u32)nhgetl(to + IPv4off);
		if(memcmp(to, v4prefix, IPv4off) == 0)
			memset(to, 0xff, IPv4off);
	}
	return x;
}

int
myetheraddr(u8 *to, char *from)
{
	USED(to);
	USED(from);
	return -1;
}

int
eipfmt(Fmt *f)
{
	char buf[64];
	u8 *p;

	switch(f->r){
	case 'E':
		p = va_arg(f->args, u8*);
		snprint(buf, sizeof(buf), "%.2ux%.2ux%.2ux%.2ux%.2ux%.2ux",
			p[0], p[1], p[2], p[3], p[4], p[5]);
		return fmtstrcpy(f, buf);
	case 'I':
	case 'i':
		p = va_arg(f->args, u8*);
		if(isv4(p)){
			snprint(buf, sizeof(buf), "%d.%d.%d.%d",
				p[12], p[13], p[14], p[15]);
			return fmtstrcpy(f, buf);
		}
		snprint(buf, sizeof(buf), "%.2ux%.2ux:%.2ux%.2ux:%.2ux%.2ux:%.2ux%.2ux:%.2ux%.2ux:%.2ux%.2ux:%.2ux%.2ux:%.2ux%.2ux",
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
			p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
		return fmtstrcpy(f, buf);
	case 'V':
	case 'M':
		p = va_arg(f->args, u8*);
		snprint(buf, sizeof(buf), "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
		return fmtstrcpy(f, buf);
	}
	return fmtstrcpy(f, "???");
}
