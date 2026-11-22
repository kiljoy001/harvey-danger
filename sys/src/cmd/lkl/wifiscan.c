/*
 * wifiscan -- drive the guest's cfg80211 from the host side.
 *
 * The UWE5622 driver under LKL registers wlan0 in the guest kernel, but the
 * guest has no userspace and therefore no wpa_supplicant/iw.  The host can
 * still issue guest syscalls with lkl_syscall(), so this file speaks
 * nl80211 (generic netlink) directly: resolve the family, bring the
 * interface up, trigger a scan and dump the BSS list as text.
 *
 * The same netlink plumbing is what a later "join" control needs; keeping it
 * here means the driver's capability surface is text and lives in one place.
 */
#include <u.h>
#include <libc.h>
#include <lkl/asm/unistd.h>

extern long	lkl_syscall(long, long*);
extern int	lkl_printf(const char*, ...);

enum {
	AfNetlink	= 16,
	NetlinkGeneric	= 16,
	SockRaw		= 3,
	SockDgram	= 2,

	NlmRequest	= 0x1,
	NlmDump		= 0x300,
	NlmAck		= 0x4,

	NlmsgError	= 2,
	NlmsgDone	= 3,

	GenlIdCtrl	= 0x10,
	CtrlCmdGetfamily = 3,
	CtrlAttrFamilyId = 1,
	CtrlAttrFamilyName = 2,

	Nl80211CmdGetScan	= 32,
	Nl80211CmdTriggerScan	= 33,
	Nl80211CmdNewScanResults = 34,

	Nl80211AttrIfindex	= 3,
	Nl80211AttrWiphyFreq	= 38,
	Nl80211AttrScanFreqs	= 44,
	Nl80211AttrScanSsids	= 45,
	Nl80211AttrBss		= 47,

	Nl80211BssBssid		= 1,
	Nl80211BssFrequency	= 2,
	Nl80211BssBeaconInterval = 4,
	Nl80211BssCapability	= 5,
	Nl80211BssIes		= 6,
	Nl80211BssSignalMbm	= 7,

	SiocGifindex	= 0x8933,
	SiocGifflags	= 0x8913,
	SiocSifflags	= 0x8914,
	IffUp		= 0x1,
};

typedef struct Nlmsg Nlmsg;
struct Nlmsg {
	u32	len;
	u16	type;
	u16	flags;
	u32	seq;
	u32	pid;
};

typedef struct Nlattr Nlattr;
struct Nlattr {
	u16	len;
	u16	type;
};

typedef struct Genlhdr Genlhdr;
struct Genlhdr {
	u8	cmd;
	u8	version;
	u16	reserved;
};

typedef struct SockaddrNl SockaddrNl;
struct SockaddrNl {
	u16	family;
	u16	pad;
	u32	pid;
	u32	groups;
};

typedef struct Ifreq Ifreq;
struct Ifreq {
	char	name[16];
	union {
		short	flags;
		int	ifindex;
	} u;
};

enum { Nlbuf = 64*1024, Maxbss = 64 };

typedef struct Bss Bss;
struct Bss {
	uchar	bssid[6];
	int	freq;
	int	signal;
	int	beacon;
	int	privacy;
	u32	group_cipher;
	int	ssidlen;
	char	ssid[33];
};

static Nlmsg*
nlstart(uchar *buf, int bufsz, u16 type, u16 flags, u32 seq)
{
	Nlmsg *n;

	memset(buf, 0, bufsz);
	n = (Nlmsg*)buf;
	n->type = type;
	n->flags = flags;
	n->seq = seq;
	n->len = sizeof(Nlmsg);
	return n;
}

static void
nlattr(Nlmsg *n, u16 type, void *data, int len)
{
	Nlattr *a;
	int alen;

	alen = sizeof(Nlattr) + len;
	a = (Nlattr*)((uchar*)n + n->len);
	a->type = type;
	a->len = alen;
	if(len)
		memmove(a+1, data, len);
	n->len += (alen + 3) & ~3;
}

static Nlattr*
nlnext(Nlattr *a, int *left)
{
	int len;

	len = (a->len + 3) & ~3;
	if(len <= 0 || len > *left)
		return nil;
	*left -= len;
	if(*left < (int)sizeof(Nlattr))
		return nil;
	return (Nlattr*)((uchar*)a + len);
}

static void*
nlpayload(Nlattr *a)
{
	return a+1;
}

static int
nlpaylen(Nlattr *a)
{
	return a->len - sizeof(Nlattr);
}

static int
nlsend(int fd, Nlmsg *n)
{
	SockaddrNl sa;

	memset(&sa, 0, sizeof sa);
	sa.family = AfNetlink;
	return lkl_syscall(__lkl__NR_sendto, (long[]){ fd, (long)n, n->len, 0,
		(long)&sa, sizeof sa });
}

static int
nlfamily(int fd)
{
	uchar buf[512];
	Nlmsg *n;
	Genlhdr *g;
	Nlattr *a;
	int m, left, fam;
	long r;

	n = nlstart(buf, sizeof buf, GenlIdCtrl, NlmRequest, 1);
	g = (Genlhdr*)(n+1);
	g->cmd = CtrlCmdGetfamily;
	g->version = 1;
	n->len += sizeof(Genlhdr);
	nlattr(n, CtrlAttrFamilyName, "nl80211", 8);
	if(nlsend(fd, n) < 0)
		return -1;
	m = lkl_syscall(__lkl__NR_recvfrom, (long[]){ fd, (long)buf, sizeof buf, 0, 0, 0 });
	if(m < (int)sizeof(Nlmsg) + (int)sizeof(Genlhdr))
		return -1;
	left = m - sizeof(Nlmsg) - sizeof(Genlhdr);
	a = (Nlattr*)((uchar*)(n+1) + sizeof(Genlhdr));
	fam = -1;
	for(; left >= (int)sizeof(Nlattr); a = nlnext(a, &left)){
		if(a == nil)
			break;
		if(a->type == CtrlAttrFamilyId && nlpaylen(a) >= 2)
			fam = *(u16*)nlpayload(a);
	}
	r = 0;
	USED(r);
	return fam;
}

static int
ifindex(char *name)
{
	Ifreq ifr;
	int fd;
	long r;

	fd = lkl_syscall(__lkl__NR_socket, (long[]){ 2 /*AF_INET*/, SockDgram, 0 });
	if(fd < 0)
		return -1;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.name, name, sizeof ifr.name - 1);
	r = lkl_syscall(__lkl__NR_ioctl, (long[]){ fd, SiocGifindex, (long)&ifr });
	lkl_syscall(__lkl__NR_close, (long[]){ fd });
	if(r < 0)
		return -1;
	return ifr.u.ifindex;
}

static int
ifup(char *name)
{
	Ifreq ifr;
	int fd;
	long r;

	fd = lkl_syscall(__lkl__NR_socket, (long[]){ 2, SockDgram, 0 });
	if(fd < 0)
		return -1;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.name, name, sizeof ifr.name - 1);
	r = lkl_syscall(__lkl__NR_ioctl, (long[]){ fd, SiocGifflags, (long)&ifr });
	if(r >= 0){
		ifr.u.flags |= IffUp;
		r = lkl_syscall(__lkl__NR_ioctl, (long[]){ fd, SiocSifflags, (long)&ifr });
	}
	lkl_syscall(__lkl__NR_close, (long[]){ fd });
	return r < 0 ? -1 : 0;
}

static void
parseies(uchar *ie, int len, Bss *b)
{
	int id, ilen;
	uchar *p, *e;

	p = ie;
	e = ie + len;
	while(p + 2 <= e){
		id = p[0];
		ilen = p[1];
		if(p + 2 + ilen > e)
			break;
		if(id == 0){			/* SSID */
			if(ilen > 32)
				ilen = 32;
			memmove(b->ssid, p+2, ilen);
			b->ssid[ilen] = 0;
			b->ssidlen = ilen;
		}else if(id == 48){		/* RSN */
			b->privacy = 1;
			if(ilen >= 6)
				b->group_cipher = ((u32)p[4] << 24) | ((u32)p[5] << 16) |
					((u32)p[6] << 8) | (u32)p[7];
		}
		p += 2 + ilen;
	}
}

static int
parsebss(Nlmsg *n, Bss *b)
{
	Nlattr *a, *inner;
	uchar *p;
	int left, inleft;

	memset(b, 0, sizeof *b);
	b->freq = 0;
	b->signal = -128;
	b->group_cipher = 0x000fac04;	/* default CCMP-128 */
	b->ssidlen = -1;
	if(n->len < sizeof(Nlmsg) + sizeof(Genlhdr))
		return -1;
	left = n->len - sizeof(Nlmsg) - sizeof(Genlhdr);
	a = (Nlattr*)((uchar*)(n+1) + sizeof(Genlhdr));
	for(; left >= (int)sizeof(Nlattr); a = nlnext(a, &left)){
		if(a == nil)
			break;
		if((a->type & 0x3fff) != Nl80211AttrBss)
			continue;
		inleft = nlpaylen(a);
		inner = (Nlattr*)nlpayload(a);
		for(; inleft >= (int)sizeof(Nlattr); inner = nlnext(inner, &inleft)){
			if(inner == nil)
				break;
			switch(inner->type & 0x3fff){
			case Nl80211BssBssid:
				if(nlpaylen(inner) >= 6)
					memmove(b->bssid, nlpayload(inner), 6);
				break;
			case Nl80211BssFrequency:
				if(nlpaylen(inner) >= 4)
					b->freq = *(u32*)nlpayload(inner);
				break;
			case Nl80211BssSignalMbm:
				if(nlpaylen(inner) >= 4)
					b->signal = *(int*)nlpayload(inner) / 100;
				break;
			case Nl80211BssBeaconInterval:
				if(nlpaylen(inner) >= 2)
					b->beacon = *(u16*)nlpayload(inner);
				break;
			case Nl80211BssCapability:
				if(nlpaylen(inner) >= 2 &&
				   (*(u16*)nlpayload(inner) & 0x10))
					b->privacy = 1;
				break;
			case Nl80211BssIes:
				p = nlpayload(inner);
				parseies(p, nlpaylen(inner), b);
				break;
			}
		}
		return 0;
	}
	return -1;
}

static Bss lastbss[Maxbss];
static int nlastbss;

/*
 * Scan wlan0 via nl80211 and format one line per BSS into out[outsz].
 * Returns the number of networks found (>= 0), or -1 on error (with a
 * short diagnostic written to out when out != nil).
 */
int
wifiscan_buf(char *out, int outsz)
{
	uchar *buf;
	Nlmsg *n;
	Genlhdr *g;
	Bss *list, b;
	int fd, fam, idx, i, m, found, left, pos;
	Nlattr wildcard;

	if(out != nil && outsz > 0)
		out[0] = 0;

	idx = ifindex("wlan0");
	if(idx < 0){
		if(out != nil && outsz > 0)
			snprint(out, outsz, "error: wlan0 not present\n");
		lkl_printf("wifiscan: wlan0 not present\n");
		return -1;
	}
	if(ifup("wlan0") < 0){
		if(out != nil && outsz > 0)
			snprint(out, outsz, "error: ifup wlan0 failed\n");
		lkl_printf("wifiscan: ifup wlan0 failed\n");
		return -1;
	}
	sleep(500);

	buf = malloc(Nlbuf);
	list = malloc(Maxbss * sizeof(Bss));
	if(buf == nil || list == nil){
		free(buf);
		free(list);
		if(out != nil && outsz > 0)
			snprint(out, outsz, "error: out of memory\n");
		return -1;
	}

	fd = lkl_syscall(__lkl__NR_socket, (long[]){ AfNetlink, SockRaw, NetlinkGeneric });
	if(fd < 0){
		free(buf);
		free(list);
		if(out != nil && outsz > 0)
			snprint(out, outsz, "error: netlink socket %d\n", fd);
		lkl_printf("wifiscan: netlink socket: %d\n", fd);
		return -1;
	}
	{
		SockaddrNl sa;

		memset(&sa, 0, sizeof sa);
		sa.family = AfNetlink;
		lkl_syscall(__lkl__NR_bind, (long[]){ fd, (long)&sa, sizeof sa });
	}
	fam = nlfamily(fd);
	if(fam < 0){
		lkl_syscall(__lkl__NR_close, (long[]){ fd });
		free(buf);
		free(list);
		if(out != nil && outsz > 0)
			snprint(out, outsz, "error: no nl80211 family\n");
		lkl_printf("wifiscan: no nl80211 family\n");
		return -1;
	}

	/* Trigger an active broadcast scan (nested attr 0 = empty SSID). */
	n = nlstart(buf, Nlbuf, fam, NlmRequest|NlmAck, 2);
	g = (Genlhdr*)(n+1);
	g->cmd = Nl80211CmdTriggerScan;
	g->version = 0;
	n->len += sizeof(Genlhdr);
	nlattr(n, Nl80211AttrIfindex, &idx, 4);
	wildcard.len = sizeof(Nlattr);
	wildcard.type = 0;
	nlattr(n, Nl80211AttrScanSsids | 0x8000, &wildcard, sizeof wildcard);
	if(nlsend(fd, n) >= 0){
		/* Drain the NLMSG_ERROR ACK (or error code) so GetScan sees a clean socket. */
		m = lkl_syscall(__lkl__NR_recvfrom, (long[]){ fd, (long)buf, Nlbuf, 0, 0, 0 });
		if(m >= (int)(sizeof(Nlmsg) + 4)){
			n = (Nlmsg*)buf;
			if(n->type == NlmsgError){
				int err = *(int*)(n+1);
				if(err != 0){
					lkl_printf("wifiscan: trigger scan err %d\n", err);
					if(err != -16 && out != nil && outsz > 0)
						snprint(out, outsz, "error: trigger scan err %d\n", err);
				}
			}
		}
	}
	sleep(4000);

	/* Dump the result set. */
	n = nlstart(buf, Nlbuf, fam, NlmRequest|NlmDump, 3);
	g = (Genlhdr*)(n+1);
	g->cmd = Nl80211CmdGetScan;
	g->version = 0;
	n->len += sizeof(Genlhdr);
	nlattr(n, Nl80211AttrIfindex, &idx, 4);
	if(nlsend(fd, n) < 0){
		lkl_syscall(__lkl__NR_close, (long[]){ fd });
		free(buf);
		free(list);
		if(out != nil && outsz > 0)
			snprint(out, outsz, "error: get scan send failed\n");
		return -1;
	}

	found = 0;
	for(;;){
		m = lkl_syscall(__lkl__NR_recvfrom, (long[]){ fd, (long)buf, Nlbuf, 0, 0, 0 });
		if(m <= 0)
			break;
		left = m;
		n = (Nlmsg*)buf;
		while(left >= (int)sizeof(Nlmsg) && n->len >= sizeof(Nlmsg) &&
		      n->len <= left){
			if(n->type == NlmsgDone)
				goto done;
			if(n->type == NlmsgError)
				goto done;
			if(found < Maxbss && parsebss(n, &b) == 0 && b.ssidlen >= 0){
				list[found++] = b;
			}
			left -= (n->len + 3) & ~3;
			n = (Nlmsg*)((uchar*)n + ((n->len + 3) & ~3));
		}
	}
done:
	lkl_syscall(__lkl__NR_close, (long[]){ fd });
	pos = 0;
	for(i = 0; i < found; i++){
		lkl_printf("wifi bss %02x:%02x:%02x:%02x:%02x:%02x freq %d "
			   "signal %d beacon %d %s ssid %s\n",
			   list[i].bssid[0], list[i].bssid[1], list[i].bssid[2],
			   list[i].bssid[3], list[i].bssid[4], list[i].bssid[5],
			   list[i].freq, list[i].signal, list[i].beacon,
			   list[i].privacy ? "secure" : "open", list[i].ssid);
		if(out != nil && pos < outsz - 1){
			pos += snprint(out + pos, outsz - pos,
				"%02x:%02x:%02x:%02x:%02x:%02x freq %d signal %d %s ssid %s\n",
				list[i].bssid[0], list[i].bssid[1], list[i].bssid[2],
				list[i].bssid[3], list[i].bssid[4], list[i].bssid[5],
				list[i].freq, list[i].signal,
				list[i].privacy ? "secure" : "open", list[i].ssid);
		}
	}
	lkl_printf("wifiscan: %d networks\n", found);
	if(found > 0){
		memmove(lastbss, list, found * sizeof(Bss));
		nlastbss = found;
	}
	free(buf);
	free(list);
	return found;
}

int
wifiscan_find(const char *ssid, uchar bssid[6], int *freq, int *privacy, u32 *group_cipher)
{
	int i, best, try;

	for(try = 0; try < 2; try++){
		best = -1;
		for(i = 0; i < nlastbss; i++){
			if(strcmp(lastbss[i].ssid, ssid) != 0)
				continue;
			if(best < 0 || lastbss[i].signal > lastbss[best].signal)
				best = i;
		}
		if(best >= 0){
			memmove(bssid, lastbss[best].bssid, 6);
			*freq = lastbss[best].freq;
			*privacy = lastbss[best].privacy;
			*group_cipher = lastbss[best].group_cipher;
			return 0;
		}
		if(try == 0)
			wifiscan_buf(nil, 0);
	}
	return -1;
}

/*
 * Scan wlan0 and print one line per BSS to the LKL log.  Returns the number
 * of networks found, or -1.
 */
int
wifiscan(void)
{
	int i;

	lkl_printf("wifiscan: start\n");
	for(i = 0; i < 40; i++){
		if(ifindex("wlan0") >= 0)
			break;
		sleep(500);
	}
	if(i == 40){
		lkl_printf("wifiscan: wlan0 did not appear\n");
		return -1;
	}
	return wifiscan_buf(nil, 0);
}
