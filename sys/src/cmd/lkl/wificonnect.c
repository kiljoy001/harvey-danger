/*
 * wificonnect -- WPA2-PSK (802.11i CCMP) + open Wi-Fi connect and DHCPv4
 * client for the LKL guest's wlan0, exposed over 9P via /mnt/lkl/wifictl
 * and /mnt/lkl/wifistatus.
 *
 * Uses Harvey's libsec (pbkdf2_x, hmac_sha1, AES) and Linux's nl80211 +
 * AF_PACKET directly via lkl_syscall() — no Linux userspace required.
 */
#include <u.h>
#include <libc.h>
#include <libsec.h>
#include <lkl/asm/unistd.h>

extern long	lkl_syscall(long, long*);
extern int	lkl_printf(const char*, ...);
extern int	wifiscan_find(const char*, uchar[6], int*, int*, u32*);

enum {
	AfInet		= 2,
	AfNetlink	= 16,
	AfPacket	= 17,
	NetlinkGeneric	= 16,
	SockStream	= 1,
	SockDgram	= 2,
	SockRaw		= 3,

	NlmRequest	= 0x1,
	NlmAck		= 0x4,
	NlmsgError	= 2,

	GenlIdCtrl	= 0x10,
	CtrlCmdGetfamily = 3,
	CtrlAttrFamilyId = 1,
	CtrlAttrFamilyName = 2,

	Nl80211CmdSetKey	= 10,
	Nl80211CmdNewKey	= 11,
	Nl80211CmdSetStation	= 18,
	Nl80211CmdConnect	= 46,
	Nl80211CmdDisconnect	= 48,
	Nl80211CmdControlPortFrame = 129,

	Nl80211AttrIfindex		= 3,
	Nl80211AttrMac			= 6,
	Nl80211AttrKeyData		= 7,
	Nl80211AttrKeyIdx		= 8,
	Nl80211AttrKeyCipher		= 9,
	Nl80211AttrKeySeq		= 10,
	Nl80211AttrKeyDefault		= 11,
	Nl80211AttrWiphyFreq		= 38,
	Nl80211AttrIe			= 42,
	Nl80211AttrFrame		= 51,
	Nl80211AttrSsid			= 52,
	Nl80211AttrAuthType		= 53,
	Nl80211AttrKeyType		= 55,
	Nl80211AttrStaFlags2		= 67,
	Nl80211AttrControlPort		= 68,
	Nl80211AttrPrivacy		= 70,
	Nl80211AttrCipherSuitesPairwise	= 73,
	Nl80211AttrCipherSuiteGroup	= 74,
	Nl80211AttrWpaVersions		= 75,
	Nl80211AttrAkmSuites		= 76,
	Nl80211AttrControlPortEthertype	= 102,
	Nl80211AttrControlPortNoEncrypt	= 103,

	Nl80211KeytypeGroup	= 0,
	Nl80211KeytypePairwise	= 1,

	CipherCcmp	= 0x000fac04,
	CipherTkip	= 0x000fac02,
	AkmPsk		= 0x000fac02,

	SiocGifflags	= 0x8913,
	SiocSifflags	= 0x8914,
	SiocSifaddr	= 0x8916,
	SiocSifnetmask	= 0x891c,
	SiocGifhwaddr	= 0x8927,
	SiocGifindex	= 0x8933,
	SiocAddrt	= 0x890b,
	IffUp		= 0x1,
	RtfUp		= 0x0001,
	RtfGateway	= 0x0002,

	MsgDontwait	= 0x40,
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

typedef struct SockaddrLl SockaddrLl;
struct SockaddrLl {
	u16	sll_family;
	u16	sll_protocol;
	int	sll_ifindex;
	u16	sll_hatype;
	u8	sll_pkttype;
	u8	sll_halen;
	u8	sll_addr[8];
};

typedef struct SockaddrIn SockaddrIn;
struct SockaddrIn {
	u16	sin_family;
	u16	sin_port;
	u32	sin_addr;
	u8	sin_zero[8];
};

typedef struct Sockaddr Sockaddr;
struct Sockaddr {
	u16	sa_family;
	u8	sa_data[14];
};

typedef struct Ifreq Ifreq;
struct Ifreq {
	char	name[16];
	union {
		Sockaddr	addr;
		short		flags;
		int		ifindex;
		u8		pad[24];
	} u;
};

typedef struct Rtentry Rtentry;
struct Rtentry {
	ulong		rt_pad1;
	Sockaddr	rt_dst;
	Sockaddr	rt_gateway;
	Sockaddr	rt_genmask;
	u16		rt_flags;
	short		rt_pad2;
	ulong		rt_pad3;
	void		*rt_pad4;
	short		rt_metric;
	char		*rt_dev;
	ulong		rt_mtu;
	ulong		rt_window;
	u16		rt_irtt;
};

static struct {
	char	state[32];
	char	ssid[36];
	uchar	bssid[6];
	int	freq;
	uchar	ip[4];
	uchar	mask[4];
	uchar	gw[4];
	uchar	dns[4];
	char	err[128];
} wst = {
	.state = "disconnected",
};

static u16
be16(const uchar *p)
{
	return ((u16)p[0] << 8) | p[1];
}

static void
putbe16(uchar *p, u16 v)
{
	p[0] = v >> 8;
	p[1] = v;
}

static u32
be32(const uchar *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void
putbe32(uchar *p, u32 v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static u16
htons16(u16 v)
{
	return (v >> 8) | (v << 8);
}

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
nlattr(Nlmsg *n, u16 type, const void *data, int len)
{
	Nlattr *a;
	int alen;

	alen = sizeof(Nlattr) + len;
	a = (Nlattr*)((uchar*)n + n->len);
	a->type = type;
	a->len = alen;
	if(len > 0)
		memmove(a + 1, data, len);
	n->len += (alen + 3) & ~3;
}

static void
nlattr_u8(Nlmsg *n, u16 type, u8 v)
{
	nlattr(n, type, &v, 1);
}

static void
nlattr_u16(Nlmsg *n, u16 type, u16 v)
{
	nlattr(n, type, &v, 2);
}

static void
nlattr_u32(Nlmsg *n, u16 type, u32 v)
{
	nlattr(n, type, &v, 4);
}

static int
nltxrx(int fd, Nlmsg *n, uchar *rbuf, int rbufsz)
{
	SockaddrNl sa;
	long r;

	memset(&sa, 0, sizeof sa);
	sa.family = AfNetlink;
	r = lkl_syscall(__lkl__NR_sendto, (long[]){ fd, (long)n, n->len, 0,
		(long)&sa, sizeof sa });
	if(r < 0)
		return (int)r;
	r = lkl_syscall(__lkl__NR_recvfrom, (long[]){ fd, (long)rbuf, rbufsz, 0, 0, 0 });
	if(r < (long)(sizeof(Nlmsg) + 4))
		return -1;
	n = (Nlmsg*)rbuf;
	if(n->type == NlmsgError)
		return *(int*)(n + 1);
	return 0;
}

static int
nl80211_open(int *famout)
{
	uchar buf[1024];
	SockaddrNl sa;
	Nlmsg *n;
	Genlhdr *g;
	Nlattr *a;
	long fd, m;
	int left, fam;

	fd = lkl_syscall(__lkl__NR_socket, (long[]){ AfNetlink, SockRaw, NetlinkGeneric });
	if(fd < 0)
		return -1;
	memset(&sa, 0, sizeof sa);
	sa.family = AfNetlink;
	lkl_syscall(__lkl__NR_bind, (long[]){ fd, (long)&sa, sizeof sa });

	n = nlstart(buf, sizeof buf, GenlIdCtrl, NlmRequest, 1);
	g = (Genlhdr*)(n + 1);
	g->cmd = CtrlCmdGetfamily;
	g->version = 1;
	n->len += sizeof(Genlhdr);
	nlattr(n, CtrlAttrFamilyName, "nl80211", 8);

	memset(&sa, 0, sizeof sa);
	sa.family = AfNetlink;
	if(lkl_syscall(__lkl__NR_sendto, (long[]){ fd, (long)n, n->len, 0,
	    (long)&sa, sizeof sa }) < 0){
		lkl_syscall(__lkl__NR_close, (long[]){ fd });
		return -1;
	}
	m = lkl_syscall(__lkl__NR_recvfrom, (long[]){ fd, (long)buf, sizeof buf, 0, 0, 0 });
	if(m < (long)(sizeof(Nlmsg) + sizeof(Genlhdr))){
		lkl_syscall(__lkl__NR_close, (long[]){ fd });
		return -1;
	}
	left = (int)m - sizeof(Nlmsg) - sizeof(Genlhdr);
	a = (Nlattr*)((uchar*)(n + 1) + sizeof(Genlhdr));
	fam = -1;
	while(left >= (int)sizeof(Nlattr)){
		int step = (a->len + 3) & ~3;
		if(step <= 0 || step > left)
			break;
		if((a->type & 0x3fff) == CtrlAttrFamilyId && a->len >= sizeof(Nlattr) + 2)
			fam = *(u16*)(a + 1);
		left -= step;
		a = (Nlattr*)((uchar*)a + step);
	}
	if(fam < 0){
		lkl_syscall(__lkl__NR_close, (long[]){ fd });
		return -1;
	}
	*famout = fam;
	return (int)fd;
}

static int
wlan_info(int *idxout, uchar mac[6])
{
	Ifreq ifr;
	long fd, r;

	fd = lkl_syscall(__lkl__NR_socket, (long[]){ AfInet, SockDgram, 0 });
	if(fd < 0)
		return -1;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.name, "wlan0", sizeof ifr.name - 1);
	r = lkl_syscall(__lkl__NR_ioctl, (long[]){ fd, SiocGifindex, (long)&ifr });
	if(r < 0){
		lkl_syscall(__lkl__NR_close, (long[]){ fd });
		return -1;
	}
	*idxout = ifr.u.ifindex;

	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.name, "wlan0", sizeof ifr.name - 1);
	r = lkl_syscall(__lkl__NR_ioctl, (long[]){ fd, SiocGifhwaddr, (long)&ifr });
	if(r >= 0)
		memmove(mac, ifr.u.addr.sa_data, 6);

	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.name, "wlan0", sizeof ifr.name - 1);
	if(lkl_syscall(__lkl__NR_ioctl, (long[]){ fd, SiocGifflags, (long)&ifr }) >= 0){
		ifr.u.flags |= IffUp;
		lkl_syscall(__lkl__NR_ioctl, (long[]){ fd, SiocSifflags, (long)&ifr });
	}
	lkl_syscall(__lkl__NR_close, (long[]){ fd });
	return r < 0 ? -1 : 0;
}

/*
 * IEEE 802.11i PRF-384 using HMAC-SHA1: derives 48-byte PTK = KCK[16]||KEK[16]||TK[16].
 */
static void
prf384(const uchar pmk[32], const uchar aa[6], const uchar spa[6],
       const uchar anonce[32], const uchar snonce[32], uchar ptk[48])
{
	static const char label[] = "Pairwise key expansion";
	uchar msg[sizeof(label) + 6 + 6 + 32 + 32 + 1];
	uchar out[60];
	int pos, i;

	pos = 0;
	memmove(msg + pos, label, sizeof(label));	/* includes NUL */
	pos += sizeof(label);
	if(memcmp(aa, spa, 6) < 0){
		memmove(msg + pos, aa, 6); pos += 6;
		memmove(msg + pos, spa, 6); pos += 6;
	}else{
		memmove(msg + pos, spa, 6); pos += 6;
		memmove(msg + pos, aa, 6); pos += 6;
	}
	if(memcmp(anonce, snonce, 32) < 0){
		memmove(msg + pos, anonce, 32); pos += 32;
		memmove(msg + pos, snonce, 32); pos += 32;
	}else{
		memmove(msg + pos, snonce, 32); pos += 32;
		memmove(msg + pos, anonce, 32); pos += 32;
	}
	for(i = 0; i < 3; i++){
		msg[pos] = (uchar)i;
		hmac_sha1(msg, pos + 1, (uchar*)pmk, 32, out + i * SHA1dlen, nil);
	}
	memmove(ptk, out, 48);
}

/*
 * RFC 3394 AES-128 Key Unwrap.  ctlen must be a multiple of 8 and >= 24.
 * Writes (ctlen - 8) plaintext bytes into pt and returns 0 on IV match.
 */
static int
aes_key_unwrap(const uchar kek[16], const uchar *ct, int ctlen, uchar *pt)
{
	static const uchar iv[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 };
	AESstate aes;
	uchar a[8], b[16];
	int n, i, j;
	u32 t;

	if(ctlen < 24 || (ctlen & 7) != 0)
		return -1;
	n = (ctlen / 8) - 1;
	setupAESstate(&aes, (uchar*)kek, 16, nil);
	memmove(a, ct, 8);
	memmove(pt, ct + 8, n * 8);

	for(j = 5; j >= 0; j--){
		for(i = n; i >= 1; i--){
			t = (u32)(n * j + i);
			a[7] ^= (uchar)(t & 0xff);
			a[6] ^= (uchar)((t >> 8) & 0xff);
			a[5] ^= (uchar)((t >> 16) & 0xff);
			a[4] ^= (uchar)((t >> 24) & 0xff);
			memmove(b, a, 8);
			memmove(b + 8, pt + (i - 1) * 8, 8);
			aes_decrypt(aes.dkey, aes.rounds, b, b);
			memmove(a, b, 8);
			memmove(pt + (i - 1) * 8, b + 8, 8);
		}
	}
	if(memcmp(a, iv, 8) != 0)
		return -1;
	return 0;
}

static int
nl_install_key(int nlfd, int fam, int ifidx, u8 keyidx, u32 cipher,
	       const uchar *key, int keylen, const uchar *mac,
	       const uchar *rsc, int is_pairwise)
{
	uchar buf[512], rbuf[512];
	Nlmsg *n;
	Genlhdr *g;
	int err;

	n = nlstart(buf, sizeof buf, fam, NlmRequest|NlmAck, 10 + keyidx);
	g = (Genlhdr*)(n + 1);
	g->cmd = Nl80211CmdNewKey;
	g->version = 0;
	n->len += sizeof(Genlhdr);
	nlattr_u32(n, Nl80211AttrIfindex, ifidx);
	nlattr_u8(n, Nl80211AttrKeyIdx, keyidx);
	nlattr_u32(n, Nl80211AttrKeyCipher, cipher);
	nlattr(n, Nl80211AttrKeyData, key, keylen);
	nlattr_u32(n, Nl80211AttrKeyType, is_pairwise ? Nl80211KeytypePairwise : Nl80211KeytypeGroup);
	if(mac != nil)
		nlattr(n, Nl80211AttrMac, mac, 6);
	if(rsc != nil)
		nlattr(n, Nl80211AttrKeySeq, rsc, 6);
	err = nltxrx(nlfd, n, rbuf, sizeof rbuf);
	lkl_printf("wificonnect: NEW_KEY idx=%d pairwise=%d cipher=0x%x -> %d\n",
		keyidx, is_pairwise, cipher, err);
	return err;
}

static int
nl_set_default_key(int nlfd, int fam, int ifidx, u8 keyidx)
{
	uchar buf[256], rbuf[256];
	Nlmsg *n;
	Genlhdr *g;

	n = nlstart(buf, sizeof buf, fam, NlmRequest|NlmAck, 20 + keyidx);
	g = (Genlhdr*)(n + 1);
	g->cmd = Nl80211CmdSetKey;
	g->version = 0;
	n->len += sizeof(Genlhdr);
	nlattr_u32(n, Nl80211AttrIfindex, ifidx);
	nlattr_u8(n, Nl80211AttrKeyIdx, keyidx);
	nlattr(n, Nl80211AttrKeyDefault, "", 0);
	return nltxrx(nlfd, n, rbuf, sizeof rbuf);
}

static int
nl_set_station_authorized(int nlfd, int fam, int ifidx, const uchar bssid[6])
{
	uchar buf[256], rbuf[256];
	struct { u32 mask; u32 set; } flags;
	Nlmsg *n;
	Genlhdr *g;
	int err;

	flags.mask = 1U << 1;	/* NL80211_STA_FLAG_AUTHORIZED */
	flags.set  = 1U << 1;
	n = nlstart(buf, sizeof buf, fam, NlmRequest|NlmAck, 30);
	g = (Genlhdr*)(n + 1);
	g->cmd = Nl80211CmdSetStation;
	g->version = 0;
	n->len += sizeof(Genlhdr);
	nlattr_u32(n, Nl80211AttrIfindex, ifidx);
	nlattr(n, Nl80211AttrMac, bssid, 6);
	nlattr(n, Nl80211AttrStaFlags2, &flags, sizeof flags);
	err = nltxrx(nlfd, n, rbuf, sizeof rbuf);
	lkl_printf("wificonnect: SET_STATION authorized -> %d\n", err);
	return err;
}

static int
eapol_send(int nlfd, int fam, int ifidx, const uchar dst[6],
	   const uchar *eapol, int eapol_len)
{
	uchar buf[512], rbuf[512];
	Nlmsg *n;
	Genlhdr *g;
	int err, try;

	/*
	 * EAPOL Msg 1/4 often arrives on AF_PACKET a few ms before cfg80211's
	 * event workqueue finishes setting wdev->connected = true, causing
	 * NL80211_CMD_CONTROL_PORT_FRAME to return -ENOTCONN (-107).  Retry
	 * briefly until wdev->connected flips true.
	 */
	err = -107;
	for(try = 0; try < 25; try++){
		n = nlstart(buf, sizeof buf, fam, NlmRequest|NlmAck, 40 + try);
		g = (Genlhdr*)(n + 1);
		g->cmd = Nl80211CmdControlPortFrame;
		g->version = 0;
		n->len += sizeof(Genlhdr);
		nlattr_u32(n, Nl80211AttrIfindex, ifidx);
		nlattr(n, Nl80211AttrMac, dst, 6);
		nlattr_u16(n, Nl80211AttrControlPortEthertype, 0x888e);
		nlattr(n, Nl80211AttrControlPortNoEncrypt, "", 0);
		nlattr(n, Nl80211AttrFrame, eapol, eapol_len);
		err = nltxrx(nlfd, n, rbuf, sizeof rbuf);
		if(err != -107)
			break;
		sleep(50);
	}
	lkl_printf("wificonnect: CONTROL_PORT_FRAME len=%d -> %d (tries=%d)\n",
		eapol_len, err, try + 1);
	return err;
}

static void
gen_random(uchar *buf, int len)
{
	long r = lkl_syscall(__lkl__NR_getrandom, (long[]){ (long)buf, len, 0 });
	if(r == len)
		return;
	for(int i = 0; i < len; i++)
		buf[i] = (uchar)(i * 37 + 0x5a);
}

/*
 * Run the 4-way EAPOL-Key handshake on pfd (already bound to ETH_P_PAE)
 * and install PTK + GTK via nl80211.
 */
static int
wpa2_handshake(int pfd, int nlfd, int fam, int ifidx,
	       const uchar mac[6], const uchar bssid[6],
	       const uchar pmk[32], const uchar *rsnie, int rsnielen,
	       u32 group_cipher)
{
	uchar rx[1536], tx[256], snonce[32], anonce[32], ptk[48], mic[SHA1dlen];
	uchar unwrapped[256], gtk[32], rsc[6];
	int got1 = 0, got3 = 0, gtklen = 0, gtkidx = 0;
	int tries, m;

	gen_random(snonce, sizeof snonce);

	for(tries = 0; tries < 200 && !got3; tries++){
		m = (int)lkl_syscall(__lkl__NR_recvfrom, (long[]){
			pfd, (long)rx, sizeof rx, MsgDontwait, 0, 0 });
		if(m <= 0){
			sleep(50);
			continue;
		}
		if(m < 14 + 4 + 95)
			continue;
		if(be16(rx + 12) != 0x888e)
			continue;
		if(memcmp(rx + 6, bssid, 6) != 0)
			continue;

		uchar *ep = rx + 14;
		int eplen = m - 14;
		int bodylen = be16(ep + 2);
		if(ep[1] != 3 || bodylen + 4 > eplen || bodylen < 95)
			continue;
		if(ep[4] != 2 && ep[4] != 254)	/* RSN (2) or WPA (254) */
			continue;

		u16 keyinfo = be16(ep + 5);
		int kdatalen = be16(ep + 97);
		if(99 + kdatalen > 4 + bodylen)
			continue;

		/* Msg 1/4: Pairwise(0x0008) + Ack(0x0080), no MIC(0x0100) */
		if((keyinfo & 0x0188) == 0x0088){
			int sr;
			memmove(anonce, ep + 17, 32);
			prf384(pmk, bssid, mac, anonce, snonce, ptk);
			got1 = 1;
			lkl_printf("wificonnect: EAPOL Msg 1/4 received (ver=%d keyinfo=0x%04x)\n",
				ep[0], keyinfo);

			/* Build Msg 2/4 (Key Length = 0 per 802.11i) */
			memset(tx, 0, sizeof tx);
			tx[0] = ep[0];				/* Match AP EAPOL version */
			tx[1] = 3;				/* EAPOL-Key */
			putbe16(tx + 2, 95 + rsnielen);
			tx[4] = ep[4];				/* RSN Key Descriptor (2) */
			putbe16(tx + 5, 0x0108 | (keyinfo & 7)); /* Pairwise + MIC + ver */
			putbe16(tx + 7, 0);			/* Key Length = 0 in Msg 2/4 */
			memmove(tx + 9, ep + 9, 8);		/* Replay Counter */
			memmove(tx + 17, snonce, 32);		/* SNonce */
			putbe16(tx + 97, rsnielen);
			memmove(tx + 99, rsnie, rsnielen);
			hmac_sha1(tx, 99 + rsnielen, ptk, 16, mic, nil);
			memmove(tx + 81, mic, 16);
			sr = eapol_send(nlfd, fam, ifidx, bssid, tx, 99 + rsnielen);
			lkl_printf("wificonnect: EAPOL Msg 2/4 sent -> %d\n", sr);
			snprint(wst.err, sizeof wst.err, "msg1(ki=0x%04x) msg2=%d", keyinfo, sr);
			continue;
		}

		/* Msg 3/4: Pairwise + Install(0x40) + Ack(0x80) + MIC(0x100) + Secure(0x200) + Enc(0x1000) */
		if(got1 && (keyinfo & 0x13c8) == 0x13c8){
			uchar saved_mic[16];

			memmove(saved_mic, ep + 81, 16);
			memset(ep + 81, 0, 16);
			hmac_sha1(ep, 4 + bodylen, ptk, 16, mic, nil);
			if(memcmp(saved_mic, mic, 16) != 0){
				lkl_printf("wificonnect: Msg 3/4 MIC mismatch!\n");
				snprint(wst.err, sizeof wst.err, "wrong passphrase (MIC mismatch)");
				return -1;
			}
			lkl_printf("wificonnect: EAPOL Msg 3/4 MIC ok (kdatalen=%d)\n", kdatalen);

			if(kdatalen >= 24 && kdatalen <= (int)sizeof unwrapped){
				if(aes_key_unwrap(ptk + 16, ep + 99, kdatalen, unwrapped) == 0){
					int ulen = kdatalen - 8;
					int off = 0;
					while(off + 2 <= ulen){
						uchar id = unwrapped[off];
						uchar len = unwrapped[off + 1];
						if(id == 0xdd && len == 0)
							break;
						if(off + 2 + len > ulen)
							break;
						/* GTK KDE: dd <len> 00 0f ac 01 <keyid|tx> 00 <GTK> */
						if(id == 0xdd && len >= 6 &&
						   unwrapped[off + 2] == 0x00 &&
						   unwrapped[off + 3] == 0x0f &&
						   unwrapped[off + 4] == 0xac &&
						   unwrapped[off + 5] == 0x01){
							gtkidx = unwrapped[off + 6] & 0x03;
							gtklen = len - 6;
							if(gtklen > (int)sizeof gtk)
								gtklen = sizeof gtk;
							memmove(gtk, unwrapped + off + 8, gtklen);
							memmove(rsc, ep + 65, 6);
							lkl_printf("wificonnect: unwrapped GTK idx=%d len=%d\n",
								gtkidx, gtklen);
						}
						off += 2 + len;
					}
				}else{
					lkl_printf("wificonnect: AES key unwrap failed\n");
				}
			}

			/* Build & send Msg 4/4 */
			memset(tx, 0, sizeof tx);
			tx[0] = ep[0];
			tx[1] = 3;
			putbe16(tx + 2, 95);
			tx[4] = ep[4];
			putbe16(tx + 5, 0x0308 | (keyinfo & 7)); /* Pairwise + MIC + Secure */
			memmove(tx + 9, ep + 9, 8);		/* Replay Counter */
			hmac_sha1(tx, 99, ptk, 16, mic, nil);
			memmove(tx + 81, mic, 16);
			eapol_send(nlfd, fam, ifidx, bssid, tx, 99);
			lkl_printf("wificonnect: EAPOL Msg 4/4 sent\n");
			got3 = 1;
		}
	}

	if(!got3){
		if(wst.err[0] == 0)
			snprint(wst.err, sizeof wst.err, got1 ? "EAPOL Msg 3/4 timeout" : "EAPOL Msg 1/4 timeout");
		return -1;
	}

	/* Install PTK (pairwise key 0) and GTK (group key) */
	if(nl_install_key(nlfd, fam, ifidx, 0, CipherCcmp, ptk + 32, 16, bssid, nil, 1) < 0){
		snprint(wst.err, sizeof wst.err, "failed to install PTK");
		return -1;
	}
	if(gtklen > 0){
		u32 gc = group_cipher;
		if(gtklen == 16)
			gc = CipherCcmp;
		else if(gtklen == 32)
			gc = CipherTkip;
		nl_install_key(nlfd, fam, ifidx, (u8)gtkidx, gc, gtk, gtklen, nil, rsc, 0);
	}
	nl_set_default_key(nlfd, fam, ifidx, 0);
	nl_set_station_authorized(nlfd, fam, ifidx, bssid);
	return 0;
}

static u16
ipcsum(const uchar *p, int len)
{
	u32 sum = 0;

	while(len > 1){
		sum += ((u32)p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}
	if(len)
		sum += (u32)p[0] << 8;
	while(sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (u16)~sum;
}

static int
dhcp_send(int pfd, int ifidx, const uchar mac[6], u32 xid, u8 msgtype,
	  const uchar *reqip, const uchar *srvid)
{
	uchar f[600];
	SockaddrLl sll;
	int off, iplen, udplen;

	memset(f, 0, sizeof f);
	/* Ethernet header */
	memset(f, 0xff, 6);
	memmove(f + 6, mac, 6);
	f[12] = 0x08;
	f[13] = 0x00;

	/* DHCP/BOOTP starts at 14 + 20 + 8 = 42 */
	uchar *d = f + 42;
	d[0] = 1;		/* BOOTREQUEST */
	d[1] = 1;		/* Ethernet */
	d[2] = 6;		/* hlen */
	putbe32(d + 4, xid);
	putbe16(d + 10, 0x8000); /* broadcast flag */
	memmove(d + 28, mac, 6);
	/* Magic cookie */
	d[236] = 99; d[237] = 130; d[238] = 83; d[239] = 99;
	off = 240;
	/* Option 53: DHCP Message Type */
	d[off++] = 53; d[off++] = 1; d[off++] = msgtype;
	if(reqip != nil){
		d[off++] = 50; d[off++] = 4;
		memmove(d + off, reqip, 4); off += 4;
	}
	if(srvid != nil){
		d[off++] = 54; d[off++] = 4;
		memmove(d + off, srvid, 4); off += 4;
	}
	/* Option 55: Parameter Request List (subnet, router, DNS) */
	d[off++] = 55; d[off++] = 3; d[off++] = 1; d[off++] = 3; d[off++] = 6;
	d[off++] = 255;
	if(off < 300)
		off = 300;

	udplen = 8 + off;
	iplen = 20 + udplen;

	/* IPv4 header at 14 */
	uchar *ip = f + 14;
	ip[0] = 0x45;
	putbe16(ip + 2, iplen);
	putbe16(ip + 4, 0x1234);
	ip[8] = 64;
	ip[9] = 17;		/* UDP */
	memset(ip + 12, 0, 4);	/* 0.0.0.0 */
	memset(ip + 16, 0xff, 4); /* 255.255.255.255 */
	putbe16(ip + 10, ipcsum(ip, 20));

	/* UDP header at 34 */
	uchar *udp = f + 34;
	putbe16(udp + 0, 68);
	putbe16(udp + 2, 67);
	putbe16(udp + 4, udplen);
	putbe16(udp + 6, 0);

	memset(&sll, 0, sizeof sll);
	sll.sll_family = AfPacket;
	sll.sll_protocol = htons16(0x0800);
	sll.sll_ifindex = ifidx;
	sll.sll_halen = 6;
	memset(sll.sll_addr, 0xff, 6);
	return (int)lkl_syscall(__lkl__NR_sendto, (long[]){
		pfd, (long)f, 14 + iplen, 0, (long)&sll, sizeof sll });
}

static int
dhcp_configure_iface(const uchar ip[4], const uchar mask[4], const uchar gw[4])
{
	Ifreq ifr;
	Rtentry rt;
	SockaddrIn *sin;
	long fd, r;

	fd = lkl_syscall(__lkl__NR_socket, (long[]){ AfInet, SockDgram, 0 });
	if(fd < 0)
		return -1;

	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.name, "wlan0", sizeof ifr.name - 1);
	sin = (SockaddrIn*)&ifr.u.addr;
	sin->sin_family = AfInet;
	memmove(&sin->sin_addr, ip, 4);
	r = lkl_syscall(__lkl__NR_ioctl, (long[]){ fd, SiocSifaddr, (long)&ifr });
	lkl_printf("wificonnect: SIOCSIFADDR %d.%d.%d.%d -> %ld\n",
		ip[0], ip[1], ip[2], ip[3], r);

	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.name, "wlan0", sizeof ifr.name - 1);
	sin = (SockaddrIn*)&ifr.u.addr;
	sin->sin_family = AfInet;
	memmove(&sin->sin_addr, mask, 4);
	r = lkl_syscall(__lkl__NR_ioctl, (long[]){ fd, SiocSifnetmask, (long)&ifr });
	lkl_printf("wificonnect: SIOCSIFNETMASK %d.%d.%d.%d -> %ld\n",
		mask[0], mask[1], mask[2], mask[3], r);

	if(gw[0] || gw[1] || gw[2] || gw[3]){
		memset(&rt, 0, sizeof rt);
		rt.rt_dst.sa_family = AfInet;
		rt.rt_genmask.sa_family = AfInet;
		sin = (SockaddrIn*)&rt.rt_gateway;
		sin->sin_family = AfInet;
		memmove(&sin->sin_addr, gw, 4);
		rt.rt_flags = RtfUp | RtfGateway;
		rt.rt_dev = "wlan0";
		r = lkl_syscall(__lkl__NR_ioctl, (long[]){ fd, SiocAddrt, (long)&rt });
		lkl_printf("wificonnect: SIOCADDRT gw %d.%d.%d.%d -> %ld\n",
			gw[0], gw[1], gw[2], gw[3], r);
	}
	lkl_syscall(__lkl__NR_close, (long[]){ fd });
	return 0;
}

static int
dhcp_run(int ifidx, const uchar mac[6])
{
	uchar rx[1536], yiaddr[4], srvid[4], mask[4], gw[4], dns[4];
	SockaddrLl sll;
	u32 xid;
	long pfd;
	int state = 0;	/* 0 = waiting OFFER, 1 = waiting ACK */
	int tries, m;

	pfd = lkl_syscall(__lkl__NR_socket, (long[]){ AfPacket, SockRaw, htons16(0x0800) });
	if(pfd < 0){
		snprint(wst.err, sizeof wst.err, "dhcp packet socket failed (%ld)", pfd);
		return -1;
	}
	memset(&sll, 0, sizeof sll);
	sll.sll_family = AfPacket;
	sll.sll_protocol = htons16(0x0800);
	sll.sll_ifindex = ifidx;
	lkl_syscall(__lkl__NR_bind, (long[]){ pfd, (long)&sll, sizeof sll });

	gen_random((uchar*)&xid, 4);
	memset(mask, 0, 4); mask[0] = 255; mask[1] = 255; mask[2] = 255;
	memset(gw, 0, 4);
	memset(dns, 0, 4);
	memset(srvid, 0, 4);

	dhcp_send((int)pfd, ifidx, mac, xid, 1, nil, nil);
	lkl_printf("wificonnect: DHCPDISCOVER sent (xid=0x%08x)\n", xid);

	for(tries = 0; tries < 200; tries++){
		if(tries == 60 && state == 0)
			dhcp_send((int)pfd, ifidx, mac, xid, 1, nil, nil);
		if(tries == 120 && state == 1)
			dhcp_send((int)pfd, ifidx, mac, xid, 3, yiaddr, srvid);

		m = (int)lkl_syscall(__lkl__NR_recvfrom, (long[]){
			pfd, (long)rx, sizeof rx, MsgDontwait, 0, 0 });
		if(m <= 0){
			sleep(50);
			continue;
		}
		if(m < 14 + 20 + 8 + 240)
			continue;
		if(be16(rx + 12) != 0x0800)
			continue;
		int ihl = (rx[14] & 0x0f) * 4;
		if(rx[14 + 9] != 17 || 14 + ihl + 8 + 240 > m)
			continue;
		uchar *udp = rx + 14 + ihl;
		if(be16(udp + 2) != 68)
			continue;
		uchar *d = udp + 8;
		int dlen = m - (14 + ihl + 8);
		if(d[0] != 2 || be32(d + 4) != xid)
			continue;
		if(d[236] != 99 || d[237] != 130 || d[238] != 83 || d[239] != 99)
			continue;

		u8 mtype = 0;
		int off = 240;
		while(off < dlen){
			u8 opt = d[off++];
			if(opt == 0) continue;
			if(opt == 255) break;
			if(off >= dlen) break;
			u8 olen = d[off++];
			if(off + olen > dlen) break;
			if(opt == 53 && olen >= 1) mtype = d[off];
			else if(opt == 1 && olen >= 4) memmove(mask, d + off, 4);
			else if(opt == 3 && olen >= 4) memmove(gw, d + off, 4);
			else if(opt == 6 && olen >= 4) memmove(dns, d + off, 4);
			else if(opt == 54 && olen >= 4) memmove(srvid, d + off, 4);
			off += olen;
		}
		if(state == 0 && mtype == 2){		/* DHCPOFFER */
			memmove(yiaddr, d + 16, 4);
			lkl_printf("wificonnect: DHCPOFFER %d.%d.%d.%d\n",
				yiaddr[0], yiaddr[1], yiaddr[2], yiaddr[3]);
			state = 1;
			dhcp_send((int)pfd, ifidx, mac, xid, 3, yiaddr, srvid);
			continue;
		}
		if(state == 1 && mtype == 5){		/* DHCPACK */
			memmove(yiaddr, d + 16, 4);
			memmove(wst.ip, yiaddr, 4);
			memmove(wst.mask, mask, 4);
			memmove(wst.gw, gw, 4);
			memmove(wst.dns, dns, 4);
			lkl_printf("wificonnect: DHCPACK ip=%d.%d.%d.%d gw=%d.%d.%d.%d dns=%d.%d.%d.%d\n",
				yiaddr[0], yiaddr[1], yiaddr[2], yiaddr[3],
				gw[0], gw[1], gw[2], gw[3],
				dns[0], dns[1], dns[2], dns[3]);
			lkl_syscall(__lkl__NR_close, (long[]){ pfd });
			dhcp_configure_iface(yiaddr, mask, gw);
			return 0;
		}
	}
	lkl_syscall(__lkl__NR_close, (long[]){ pfd });
	snprint(wst.err, sizeof wst.err, "DHCP timeout");
	return -1;
}

int
wifidisconnect(void)
{
	uchar buf[256], rbuf[256], mac[6];
	Nlmsg *n;
	Genlhdr *g;
	int nlfd, fam, ifidx;

	if(wlan_info(&ifidx, mac) < 0)
		return -1;
	nlfd = nl80211_open(&fam);
	if(nlfd < 0)
		return -1;
	n = nlstart(buf, sizeof buf, fam, NlmRequest|NlmAck, 99);
	g = (Genlhdr*)(n + 1);
	g->cmd = Nl80211CmdDisconnect;
	g->version = 0;
	n->len += sizeof(Genlhdr);
	nlattr_u32(n, Nl80211AttrIfindex, ifidx);
	nltxrx(nlfd, n, rbuf, sizeof rbuf);
	lkl_syscall(__lkl__NR_close, (long[]){ nlfd });

	snprint(wst.state, sizeof wst.state, "disconnected");
	wst.ssid[0] = 0;
	wst.err[0] = 0;
	memset(wst.ip, 0, 4);
	memset(wst.gw, 0, 4);
	return 0;
}

int
wificonnect(const char *ssid, const char *psk)
{
	uchar bssid[6], mac[6], pmk[32], rsnie[22], buf[512], rbuf[512];
	SockaddrLl sll;
	Nlmsg *n;
	Genlhdr *g;
	int freq, privacy, ifidx, nlfd, fam, err;
	u32 group_cipher;
	long pfd = -1;

	snprint(wst.state, sizeof wst.state, "connecting");
	snprint(wst.ssid, sizeof wst.ssid, "%s", ssid);
	wst.err[0] = 0;
	memset(wst.ip, 0, 4);
	memset(wst.gw, 0, 4);

	if(wlan_info(&ifidx, mac) < 0){
		snprint(wst.state, sizeof wst.state, "error");
		snprint(wst.err, sizeof wst.err, "wlan0 not ready");
		return -1;
	}
	if(wifiscan_find(ssid, bssid, &freq, &privacy, &group_cipher) < 0){
		snprint(wst.state, sizeof wst.state, "error");
		snprint(wst.err, sizeof wst.err, "ssid %s not found in scan", ssid);
		return -1;
	}
	memmove(wst.bssid, bssid, 6);
	wst.freq = freq;
	lkl_printf("wificonnect: target %s bssid %02x:%02x:%02x:%02x:%02x:%02x freq %d privacy %d gc 0x%08x\n",
		ssid, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
		freq, privacy, group_cipher);

	if(privacy && (psk == nil || psk[0] == 0)){
		snprint(wst.state, sizeof wst.state, "error");
		snprint(wst.err, sizeof wst.err, "passphrase required for %s", ssid);
		return -1;
	}

	/* Build 22-byte RSN IE matching AP's group cipher + CCMP pairwise + PSK */
	rsnie[0] = 0x30; rsnie[1] = 20;
	rsnie[2] = 0x01; rsnie[3] = 0x00;			/* RSN v1 */
	putbe32(rsnie + 4, group_cipher);			/* Group cipher */
	rsnie[8] = 0x01; rsnie[9] = 0x00;			/* 1 pairwise cipher */
	putbe32(rsnie + 10, CipherCcmp);			/* CCMP-128 */
	rsnie[14] = 0x01; rsnie[15] = 0x00;			/* 1 AKM suite */
	putbe32(rsnie + 16, AkmPsk);				/* PSK */
	rsnie[20] = 0x00; rsnie[21] = 0x00;			/* RSN caps */

	if(privacy){
		pbkdf2_x((uchar*)psk, (u32)strlen(psk), (uchar*)ssid, (u32)strlen(ssid),
			4096, pmk, 32, hmac_sha1, SHA1dlen);
		/* Bind EAPOL socket BEFORE NL80211_CMD_CONNECT so we never miss Msg 1/4 */
		pfd = lkl_syscall(__lkl__NR_socket, (long[]){ AfPacket, SockRaw, htons16(0x888e) });
		if(pfd < 0){
			snprint(wst.state, sizeof wst.state, "error");
			snprint(wst.err, sizeof wst.err, "AF_PACKET(EAPOL) failed (%ld)", pfd);
			return -1;
		}
		memset(&sll, 0, sizeof sll);
		sll.sll_family = AfPacket;
		sll.sll_protocol = htons16(0x888e);
		sll.sll_ifindex = ifidx;
		lkl_syscall(__lkl__NR_bind, (long[]){ pfd, (long)&sll, sizeof sll });
	}

	nlfd = nl80211_open(&fam);
	if(nlfd < 0){
		if(pfd >= 0) lkl_syscall(__lkl__NR_close, (long[]){ pfd });
		snprint(wst.state, sizeof wst.state, "error");
		snprint(wst.err, sizeof wst.err, "nl80211 open failed");
		return -1;
	}

	n = nlstart(buf, sizeof buf, fam, NlmRequest|NlmAck, 5);
	g = (Genlhdr*)(n + 1);
	g->cmd = Nl80211CmdConnect;
	g->version = 0;
	n->len += sizeof(Genlhdr);
	nlattr_u32(n, Nl80211AttrIfindex, ifidx);
	nlattr(n, Nl80211AttrSsid, ssid, (int)strlen(ssid));
	nlattr(n, Nl80211AttrMac, bssid, 6);
	nlattr_u32(n, Nl80211AttrWiphyFreq, (u32)freq);
	nlattr_u32(n, Nl80211AttrAuthType, 0);			/* OPEN_SYSTEM */
	if(privacy){
		nlattr(n, Nl80211AttrPrivacy, "", 0);
		nlattr_u32(n, Nl80211AttrWpaVersions, 2);	/* WPA2 */
		nlattr_u32(n, Nl80211AttrCipherSuitesPairwise, CipherCcmp);
		nlattr_u32(n, Nl80211AttrCipherSuiteGroup, group_cipher);
		nlattr_u32(n, Nl80211AttrAkmSuites, AkmPsk);
		nlattr(n, Nl80211AttrIe, rsnie, sizeof rsnie);
		nlattr(n, Nl80211AttrControlPort, "", 0);
		nlattr_u16(n, Nl80211AttrControlPortEthertype, 0x888e);
		nlattr(n, Nl80211AttrControlPortNoEncrypt, "", 0);
	}
	err = nltxrx(nlfd, n, rbuf, sizeof rbuf);
	lkl_printf("wificonnect: NL80211_CMD_CONNECT -> %d\n", err);
	if(err != 0){
		if(pfd >= 0) lkl_syscall(__lkl__NR_close, (long[]){ pfd });
		lkl_syscall(__lkl__NR_close, (long[]){ nlfd });
		snprint(wst.state, sizeof wst.state, "error");
		snprint(wst.err, sizeof wst.err, "NL80211_CMD_CONNECT err %d", err);
		return -1;
	}

	if(privacy){
		snprint(wst.state, sizeof wst.state, "handshaking");
		err = wpa2_handshake((int)pfd, nlfd, fam, ifidx, mac, bssid,
			pmk, rsnie, sizeof rsnie, group_cipher);
		lkl_syscall(__lkl__NR_close, (long[]){ pfd });
		if(err < 0){
			lkl_syscall(__lkl__NR_close, (long[]){ nlfd });
			snprint(wst.state, sizeof wst.state, "error");
			return -1;
		}
	}
	lkl_syscall(__lkl__NR_close, (long[]){ nlfd });

	snprint(wst.state, sizeof wst.state, "dhcp");
	if(dhcp_run(ifidx, mac) < 0){
		snprint(wst.state, sizeof wst.state, "associated_no_ip");
		return -1;
	}
	snprint(wst.state, sizeof wst.state, "connected");
	return 0;
}

int
wifitcpcheck(const char *ipstr, int port)
{
	SockaddrIn sin;
	uchar ip[4];
	const char *p = ipstr;
	long fd, r;

	for(int i = 0; i < 4; i++){
		ip[i] = (uchar)strtoul(p, (char**)&p, 10);
		if(i < 3 && *p == '.')
			p++;
	}
	fd = lkl_syscall(__lkl__NR_socket, (long[]){ AfInet, SockStream, 0 });
	if(fd < 0)
		return -1;
	memset(&sin, 0, sizeof sin);
	sin.sin_family = AfInet;
	sin.sin_port = htons16((u16)port);
	memmove(&sin.sin_addr, ip, 4);
	r = lkl_syscall(__lkl__NR_connect, (long[]){ fd, (long)&sin, sizeof sin });
	lkl_syscall(__lkl__NR_close, (long[]){ fd });
	lkl_printf("wificonnect: tcpcheck %s:%d -> %ld\n", ipstr, port, r);
	if(r == 0)
		snprint(wst.err, sizeof wst.err, "tcp %s:%d ok", ipstr, port);
	else
		snprint(wst.err, sizeof wst.err, "tcp %s:%d err %ld", ipstr, port, r);
	return (int)r;
}

void
wifistatus_buf(char *out, int outsz)
{
	snprint(out, outsz,
		"state %s\n"
		"ssid %s\n"
		"bssid %02x:%02x:%02x:%02x:%02x:%02x\n"
		"freq %d\n"
		"ip %d.%d.%d.%d\n"
		"mask %d.%d.%d.%d\n"
		"gw %d.%d.%d.%d\n"
		"dns %d.%d.%d.%d\n"
		"info %s\n",
		wst.state, wst.ssid,
		wst.bssid[0], wst.bssid[1], wst.bssid[2],
		wst.bssid[3], wst.bssid[4], wst.bssid[5],
		wst.freq,
		wst.ip[0], wst.ip[1], wst.ip[2], wst.ip[3],
		wst.mask[0], wst.mask[1], wst.mask[2], wst.mask[3],
		wst.gw[0], wst.gw[1], wst.gw[2], wst.gw[3],
		wst.dns[0], wst.dns[1], wst.dns[2], wst.dns[3],
		wst.err);
}
