/*
 * In-process Qualcomm QRTR services for Bengal / SM6115 under Harvey LKL:
 *   - tqftpserv (QRTR service 4096, version 1, instance 0): read-only TFTP
 *     serving /fw/modem/image and /fw/dsp (e.g. wlanmdsp.mbn).
 *   - rmtfs (QMI service 14, version 1, instance 0): strictly read-only
 *     RAM-backed storage for modemst1/modemst2/fsg/fsc over qcom_rmtfs_mem1.
 *     Partitions are loaded once from eMMC O_RDONLY into RAM; modem writes
 *     update only the RAM copy and never touch flash.
 */
#include <u.h>
#include <libc.h>

extern long	lkl_syscall(long no, long *params);
extern void	lkl_printf(const char *fmt, ...);
extern int	lklhost_spawn(void (*fn)(void*), void *arg);

enum {
	LklNrMknodat	= 33,
	LklNrOpenat	= 56,
	LklNrClose	= 57,
	LklNrLseek	= 62,
	LklNrRead	= 63,
	LklNrWrite	= 64,
	LklNrPread64	= 67,
	LklNrPwrite64	= 68,
	LklNrSocket	= 198,
	LklNrBind	= 200,
	LklNrGetsockname = 204,
	LklNrSendto	= 206,
	LklNrRecvfrom	= 207,

	LklAtFdcwd	= -100,
	LklOrdonly	= 0,
	LklOrdwr	= 2,
	LklSeekSet	= 0,
	LklSeekEnd	= 2,
	LklSifchr	= 0020000,

	AfQipcrtr	= 42,
	SockDgram	= 2,
	QrtrPortCtrl	= 0xfffffffeU,
	QrtrNewServer	= 4,

	TftpSvc		= 4096,
	TftpVer		= 1,
	RmtfsSvc	= 14,
	RmtfsVer	= 1,

	SectorSz	= 512,
	MaxPartBytes	= 4 * 1024 * 1024,
};

typedef struct SockaddrQrtr SockaddrQrtr;
struct SockaddrQrtr {
	u16	sq_family;
	u16	pad;
	u32	sq_node;
	u32	sq_port;
};

typedef struct QrtrCtrlPkt QrtrCtrlPkt;
struct QrtrCtrlPkt {
	u32	cmd;
	u32	service;
	u32	instance;
	u32	node;
	u32	port;
};

static long
qsys(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
	long p[6] = { a0, a1, a2, a3, a4, a5 };
	return lkl_syscall(nr, p);
}

static int
qrtr_open_and_publish(u32 svc, u32 ver, u32 inst)
{
	SockaddrQrtr me, ctrl;
	QrtrCtrlPkt pkt;
	u32 alen;
	long fd, r;

	fd = qsys(LklNrSocket, AfQipcrtr, SockDgram, 0, 0, 0, 0);
	if(fd < 0){
		lkl_printf("qcom_qrtr: socket(AF_QIPCRTR) -> %ld\n", fd);
		return -1;
	}
	memset(&me, 0, sizeof me);
	alen = sizeof me;
	r = qsys(LklNrGetsockname, fd, (long)&me, (long)&alen, 0, 0, 0);
	if(r < 0){
		lkl_printf("qcom_qrtr: getsockname -> %ld\n", r);
		qsys(LklNrClose, fd, 0, 0, 0, 0, 0);
		return -1;
	}
	r = qsys(LklNrBind, fd, (long)&me, sizeof me, 0, 0, 0);
	if(r < 0){
		lkl_printf("qcom_qrtr: bind -> %ld\n", r);
		qsys(LklNrClose, fd, 0, 0, 0, 0, 0);
		return -1;
	}
	alen = sizeof me;
	r = qsys(LklNrGetsockname, fd, (long)&me, (long)&alen, 0, 0, 0);
	if(r < 0){
		lkl_printf("qcom_qrtr: getsockname -> %ld\n", r);
		qsys(LklNrClose, fd, 0, 0, 0, 0, 0);
		return -1;
	}
	memset(&ctrl, 0, sizeof ctrl);
	ctrl.sq_family = AfQipcrtr;
	ctrl.sq_node = me.sq_node;
	ctrl.sq_port = QrtrPortCtrl;

	memset(&pkt, 0, sizeof pkt);
	pkt.cmd = QrtrNewServer;
	pkt.service = svc;
	pkt.instance = (inst << 8) | ver;
	pkt.node = me.sq_node;
	pkt.port = me.sq_port;

	r = qsys(LklNrSendto, fd, (long)&pkt, sizeof pkt, 0, (long)&ctrl, sizeof ctrl);
	if(r < 0){
		lkl_printf("qcom_qrtr: publish svc=%u -> %ld\n", svc, r);
		qsys(LklNrClose, fd, 0, 0, 0, 0, 0);
		return -1;
	}
	lkl_printf("qcom_qrtr: published svc=%u ver=%u inst=%u on %u:%u\n",
		svc, ver, inst, me.sq_node, me.sq_port);
	return (int)fd;
}

static long
read_sysfs_str(char *path, char *buf, int bufsz)
{
	long fd, n;

	fd = qsys(LklNrOpenat, LklAtFdcwd, (long)path, LklOrdonly, 0, 0, 0);
	if(fd < 0)
		return fd;
	n = qsys(LklNrRead, fd, (long)buf, bufsz - 1, 0, 0, 0);
	qsys(LklNrClose, fd, 0, 0, 0, 0, 0);
	if(n < 0)
		return n;
	buf[n] = 0;
	while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
		buf[--n] = 0;
	return n;
}

/* ---- tqftpserv (RFC 1350 TFTP over AF_QIPCRTR, service 4096) ---- */

static void
tftp_send_err(int fd, SockaddrQrtr *peer, u16 code, char *msg)
{
	uchar buf[128];
	int n;

	buf[0] = 0;
	buf[1] = 5;	/* ERROR */
	buf[2] = code >> 8;
	buf[3] = code & 0xff;
	n = snprint((char*)buf + 4, sizeof buf - 5, "%s", msg);
	qsys(LklNrSendto, fd, (long)buf, 4 + n + 1, 0, (long)peer, sizeof *peer);
}

static int
tftp_open_local(char *req, char *outpath, int outsz, long *fsz)
{
	static char *prefixes[] = {
		"/readonly/firmware/image/",
		"/firmware/image/",
		"/readonly/",
		"/",
	};
	static char *roots[] = {
		"/fw/modem/image/",
		"/fw/dsp/",
		"/lib/firmware/",
	};
	char *rel;
	int i;
	long fd, sz;

	rel = req;
	for(i = 0; i < (int)nelem(prefixes); i++){
		int plen = strlen(prefixes[i]);
		if(strncmp(rel, prefixes[i], plen) == 0){
			rel += plen;
			break;
		}
	}
	if(strstr(rel, "..") != nil)
		return -1;
	for(i = 0; i < (int)nelem(roots); i++){
		snprint(outpath, outsz, "%s%s", roots[i], rel);
		fd = qsys(LklNrOpenat, LklAtFdcwd, (long)outpath, LklOrdonly, 0, 0, 0);
		if(fd >= 0){
			sz = qsys(LklNrLseek, fd, 0, LklSeekEnd, 0, 0, 0);
			qsys(LklNrLseek, fd, 0, LklSeekSet, 0, 0, 0);
			*fsz = sz > 0 ? sz : 0;
			return (int)fd;
		}
	}
	return -1;
}

static void
tftp_handle_rrq(SockaddrQrtr *peer, uchar *req, int reqlen)
{
	char *p, *end, *fname, *k, *v;
	char local[256];
	int ffd, tfd, blksize = 512, want_oack = 0, want_tsize = 0, want_wsize = 0, want_blk = 0;
	long fsz = 0, off = 0, n, r;
	uchar *pkt;
	uchar ack[64];
	SockaddrQrtr from;
	u32 flen;
	u16 blk, op;

	p = (char*)req + 2;
	end = (char*)req + reqlen;
	fname = p;
	while(p < end && *p != 0)
		p++;
	if(p >= end)
		return;
	p++;
	/* skip mode */
	while(p < end && *p != 0)
		p++;
	if(p < end)
		p++;

	while(p < end){
		k = p;
		while(p < end && *p != 0)
			p++;
		if(p >= end)
			break;
		p++;
		v = p;
		while(p < end && *p != 0)
			p++;
		if(p < end)
			p++;
		if(cistrcmp(k, "blksize") == 0){
			int b = atoi(v);
			if(b >= 8 && b <= 16384)
				blksize = b;
			want_blk = want_oack = 1;
		}else if(cistrcmp(k, "tsize") == 0){
			want_tsize = want_oack = 1;
		}else if(cistrcmp(k, "wsize") == 0){
			want_wsize = want_oack = 1;
		}
	}

	tfd = (int)qsys(LklNrSocket, AfQipcrtr, SockDgram, 0, 0, 0, 0);
	if(tfd < 0)
		return;

	ffd = tftp_open_local(fname, local, sizeof local, &fsz);
	if(ffd < 0){
		lkl_printf("tqftpserv: RRQ %s -> ENOENT\n", fname);
		tftp_send_err(tfd, peer, 1, "file not found");
		qsys(LklNrClose, tfd, 0, 0, 0, 0, 0);
		return;
	}
	lkl_printf("tqftpserv: RRQ %s -> %s (%ld bytes, blk=%d)\n",
		fname, local, fsz, blksize);

	if(want_oack){
		uchar obuf[256];
		int olen = 2;

		obuf[0] = 0;
		obuf[1] = 6;	/* OACK */
		if(want_blk)
			olen += snprint((char*)obuf + olen, sizeof obuf - olen,
				"blksize%c%d%c", 0, blksize, 0);
		if(want_tsize)
			olen += snprint((char*)obuf + olen, sizeof obuf - olen,
				"tsize%c%ld%c", 0, fsz, 0);
		if(want_wsize)
			olen += snprint((char*)obuf + olen, sizeof obuf - olen,
				"wsize%c1%c", 0, 0);
		qsys(LklNrSendto, tfd, (long)obuf, olen, 0, (long)peer, sizeof *peer);
		flen = sizeof from;
		r = qsys(LklNrRecvfrom, tfd, (long)ack, sizeof ack, 0, (long)&from, (long)&flen);
		if(r < 4 || ((ack[0] << 8) | ack[1]) != 4){
			qsys(LklNrClose, ffd, 0, 0, 0, 0, 0);
			qsys(LklNrClose, tfd, 0, 0, 0, 0, 0);
			return;
		}
	}

	pkt = malloc(4 + blksize);
	if(pkt == nil){
		qsys(LklNrClose, ffd, 0, 0, 0, 0, 0);
		qsys(LklNrClose, tfd, 0, 0, 0, 0, 0);
		return;
	}

	blk = 1;
	for(;;){
		n = qsys(LklNrPread64, ffd, (long)(pkt + 4), blksize, off, 0, 0);
		if(n < 0)
			n = 0;
		pkt[0] = 0;
		pkt[1] = 3;	/* DATA */
		pkt[2] = blk >> 8;
		pkt[3] = blk & 0xff;
		r = qsys(LklNrSendto, tfd, (long)pkt, 4 + n, 0, (long)peer, sizeof *peer);
		if(r < 0)
			break;
		flen = sizeof from;
		r = qsys(LklNrRecvfrom, tfd, (long)ack, sizeof ack, 0, (long)&from, (long)&flen);
		if(r < 4)
			break;
		op = (ack[0] << 8) | ack[1];
		if(op != 4)
			break;
		off += n;
		blk++;
		if(n < blksize)
			break;
	}

	free(pkt);
	qsys(LklNrClose, ffd, 0, 0, 0, 0, 0);
	qsys(LklNrClose, tfd, 0, 0, 0, 0, 0);
}

static void
tftp_handle_wrq(SockaddrQrtr *peer, uchar *req, int reqlen)
{
	char *p, *end, *fname, *k, *v;
	int tfd, blksize = 512, want_oack = 0, want_blk = 0, want_wsize = 0;
	uchar *dbuf;
	uchar ack[4];
	SockaddrQrtr from;
	u32 flen;
	long r, total = 0;

	p = (char*)req + 2;
	end = (char*)req + reqlen;
	fname = p;
	while(p < end && *p != 0)
		p++;
	if(p >= end)
		return;
	p++;
	while(p < end && *p != 0)
		p++;
	if(p < end)
		p++;
	while(p < end){
		k = p;
		while(p < end && *p != 0)
			p++;
		if(p >= end)
			break;
		p++;
		v = p;
		while(p < end && *p != 0)
			p++;
		if(p < end)
			p++;
		if(cistrcmp(k, "blksize") == 0){
			int b = atoi(v);
			if(b >= 8 && b <= 16384)
				blksize = b;
			want_blk = want_oack = 1;
		}else if(cistrcmp(k, "wsize") == 0){
			want_wsize = want_oack = 1;
		}
	}

	tfd = (int)qsys(LklNrSocket, AfQipcrtr, SockDgram, 0, 0, 0, 0);
	if(tfd < 0)
		return;
	dbuf = malloc(4 + blksize);
	if(dbuf == nil){
		qsys(LklNrClose, tfd, 0, 0, 0, 0, 0);
		return;
	}

	if(want_oack){
		uchar obuf[128];
		int olen = 2;

		obuf[0] = 0;
		obuf[1] = 6;	/* OACK */
		if(want_blk)
			olen += snprint((char*)obuf + olen, sizeof obuf - olen,
				"blksize%c%d%c", 0, blksize, 0);
		if(want_wsize)
			olen += snprint((char*)obuf + olen, sizeof obuf - olen,
				"wsize%c1%c", 0, 0);
		qsys(LklNrSendto, tfd, (long)obuf, olen, 0, (long)peer, sizeof *peer);
	}else{
		ack[0] = 0;
		ack[1] = 4;	/* ACK(0) */
		ack[2] = 0;
		ack[3] = 0;
		qsys(LklNrSendto, tfd, (long)ack, 4, 0, (long)peer, sizeof *peer);
	}

	for(;;){
		flen = sizeof from;
		r = qsys(LklNrRecvfrom, tfd, (long)dbuf, 4 + blksize, 0, (long)&from, (long)&flen);
		if(r < 4)
			break;
		if(((dbuf[0] << 8) | dbuf[1]) != 3)	/* DATA */
			break;
		total += r - 4;
		ack[0] = 0;
		ack[1] = 4;	/* ACK(block) */
		ack[2] = dbuf[2];
		ack[3] = dbuf[3];
		qsys(LklNrSendto, tfd, (long)ack, 4, 0, (long)peer, sizeof *peer);
		if(r - 4 < blksize)
			break;
	}
	lkl_printf("tqftpserv: WRQ %s -> %ld bytes (RAM sink)\n", fname, total);
	free(dbuf);
	qsys(LklNrClose, tfd, 0, 0, 0, 0, 0);
}

static void
tqftp_thread(void *arg)
{
	int fd;
	uchar buf[1024];
	SockaddrQrtr peer;
	u32 plen;
	long n;
	u16 op;

	USED(arg);
	fd = qrtr_open_and_publish(TftpSvc, TftpVer, 0);
	if(fd < 0)
		return;
	for(;;){
		plen = sizeof peer;
		n = qsys(LklNrRecvfrom, fd, (long)buf, sizeof buf, 0, (long)&peer, (long)&plen);
		if(n < 2)
			continue;
		if(peer.sq_port == QrtrPortCtrl)
			continue;
		op = (buf[0] << 8) | buf[1];
		if(op == 1)
			tftp_handle_rrq(&peer, buf, (int)n);
		else if(op == 2)
			tftp_handle_wrq(&peer, buf, (int)n);
	}
}

/* ---- Read-only RAM-backed rmtfs (QMI service 14, version 1, instance 0) ---- */

typedef struct RmtPart RmtPart;
struct RmtPart {
	char	*qmipath;
	char	*partname;
	uchar	*data;
	u32	size;
};

static RmtPart rmtparts[] = {
	{ "/boot/modem_fs1", "modemst1", nil, 0 },
	{ "/boot/modem_fs2", "modemst2", nil, 0 },
	{ "/boot/modem_fsg", "fsg",      nil, 0 },
	{ "/boot/modem_fsc", "fsc",      nil, 0 },
};

static int	rmtmemfd = -1;
static u64	rmtmempa;
static u64	rmtmemsz;

static void
rmtfs_load_parts(void)
{
	int p, i;
	char path[96], ubuf[512], devpath[64];

	for(p = 1; p <= 77; p++){
		snprint(path, sizeof path, "/sys/block/mmcblk0/mmcblk0p%d/uevent", p);
		if(read_sysfs_str(path, ubuf, sizeof ubuf) <= 0)
			continue;
		for(i = 0; i < (int)nelem(rmtparts); i++){
			char needle[48];
			long fd, sz, off, n;

			if(rmtparts[i].data != nil)
				continue;
			snprint(needle, sizeof needle, "PARTNAME=%s", rmtparts[i].partname);
			if(strstr(ubuf, needle) == nil)
				continue;
			snprint(devpath, sizeof devpath, "/dev/mmcblk0p%d", p);
			fd = qsys(LklNrOpenat, LklAtFdcwd, (long)devpath, LklOrdonly, 0, 0, 0);
			if(fd < 0)
				continue;
			sz = qsys(LklNrLseek, fd, 0, LklSeekEnd, 0, 0, 0);
			qsys(LklNrLseek, fd, 0, LklSeekSet, 0, 0, 0);
			if(sz <= 0 || sz > MaxPartBytes)
				sz = MaxPartBytes;
			rmtparts[i].data = mallocz(sz, 1);
			if(rmtparts[i].data != nil){
				off = 0;
				while(off < sz){
					n = qsys(LklNrPread64, fd,
						(long)(rmtparts[i].data + off),
						sz - off > 65536 ? 65536 : sz - off,
						off, 0, 0);
					if(n <= 0)
						break;
					off += n;
				}
				rmtparts[i].size = (u32)off;
				lkl_printf("rmtfs: loaded %s (%s) %ud bytes RO into RAM\n",
					rmtparts[i].partname, devpath, rmtparts[i].size);
			}
			qsys(LklNrClose, fd, 0, 0, 0, 0, 0);
		}
	}
}

static int
rmtfs_open_mem(void)
{
	char buf[64], *colon;
	unsigned int maj, min;
	long r;

	if(read_sysfs_str("/sys/class/rmtfs/qcom_rmtfs_mem1/phys_addr", buf, sizeof buf) <= 0)
		return -1;
	rmtmempa = strtoull(buf, nil, 0);
	if(read_sysfs_str("/sys/class/rmtfs/qcom_rmtfs_mem1/size", buf, sizeof buf) <= 0)
		return -1;
	rmtmemsz = strtoull(buf, nil, 0);
	if(read_sysfs_str("/sys/class/rmtfs/qcom_rmtfs_mem1/dev", buf, sizeof buf) <= 0)
		return -1;
	colon = strchr(buf, ':');
	if(colon == nil)
		return -1;
	maj = strtoul(buf, nil, 10);
	min = strtoul(colon + 1, nil, 10);
	qsys(LklNrMknodat, LklAtFdcwd, (long)"/dev/qcom_rmtfs_mem1",
		LklSifchr | 0600,
		(min & 0xff) | ((maj & 0xfff) << 8) | ((min & ~0xff) << 12), 0, 0);
	r = qsys(LklNrOpenat, LklAtFdcwd, (long)"/dev/qcom_rmtfs_mem1", LklOrdwr, 0, 0, 0);
	if(r < 0)
		return -1;
	rmtmemfd = (int)r;
	lkl_printf("rmtfs: qcom_rmtfs_mem1 pa=%#llx size=%#llx fd=%d\n",
		rmtmempa, rmtmemsz, rmtmemfd);
	return 0;
}

static u16
le16(uchar *p)
{
	return (u16)p[0] | ((u16)p[1] << 8);
}

static u32
le32(uchar *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int
qmi_put_tlv(uchar *dst, u8 type, void *val, u16 len)
{
	dst[0] = type;
	dst[1] = len & 0xff;
	dst[2] = len >> 8;
	if(len > 0)
		memmove(dst + 3, val, len);
	return 3 + len;
}

static int
qmi_find_tlv(uchar *p, int len, u8 want, uchar **val, u16 *vlen)
{
	int off = 0;

	while(off + 3 <= len){
		u8 t = p[off];
		u16 l = le16(p + off + 1);
		if(off + 3 + l > len)
			break;
		if(t == want){
			*val = p + off + 3;
			*vlen = l;
			return 0;
		}
		off += 3 + l;
	}
	return -1;
}

static void
rmtfs_send_resp(int fd, SockaddrQrtr *peer, u16 txn, u16 msgid, uchar *tlvs, int tlvlen)
{
	uchar buf[128];

	if(7 + tlvlen > (int)sizeof buf)
		return;
	buf[0] = 0x02;	/* QMI_RESPONSE */
	buf[1] = txn & 0xff;
	buf[2] = txn >> 8;
	buf[3] = msgid & 0xff;
	buf[4] = msgid >> 8;
	buf[5] = tlvlen & 0xff;
	buf[6] = tlvlen >> 8;
	if(tlvlen > 0)
		memmove(buf + 7, tlvs, tlvlen);
	qsys(LklNrSendto, fd, (long)buf, 7 + tlvlen, 0, (long)peer, sizeof *peer);
}

static void
rmtfs_thread(void *arg)
{
	int fd, i;
	uchar buf[1024], resp[96];
	SockaddrQrtr peer;
	u32 plen;
	long n;

	USED(arg);
	rmtfs_load_parts();
	if(rmtfs_open_mem() < 0){
		lkl_printf("rmtfs: qcom_rmtfs_mem1 unavailable\n");
		return;
	}
	fd = qrtr_open_and_publish(RmtfsSvc, RmtfsVer, 0);
	if(fd < 0)
		return;

	for(;;){
		u16 txn, msgid, mlen;
		uchar *body;
		int rlen = 0;
		u16 res[2] = { 0, 0 };

		plen = sizeof peer;
		n = qsys(LklNrRecvfrom, fd, (long)buf, sizeof buf, 0, (long)&peer, (long)&plen);
		if(n < 7 || peer.sq_port == QrtrPortCtrl)
			continue;
		if(buf[0] != 0x00)	/* QMI_REQUEST */
			continue;
		txn = le16(buf + 1);
		msgid = le16(buf + 3);
		mlen = le16(buf + 5);
		if(7 + mlen > n)
			continue;
		body = buf + 7;

		switch(msgid){
		case 0x01: {	/* RMTFS_OPEN */
			uchar *pv;
			u16 pl;
			char qpath[64];
			u32 cid = 0;

			if(qmi_find_tlv(body, mlen, 0x01, &pv, &pl) == 0 && pl < sizeof qpath){
				memmove(qpath, pv, pl);
				qpath[pl] = 0;
				for(i = 0; i < (int)nelem(rmtparts); i++)
					if(strcmp(qpath, rmtparts[i].qmipath) == 0){
						cid = i + 1;
						break;
					}
				lkl_printf("rmtfs: OPEN %s -> cid=%u\n", qpath, cid);
			}
			if(cid == 0){
				res[0] = 1;
				res[1] = 0x0001;
			}
			rlen += qmi_put_tlv(resp + rlen, 0x02, res, sizeof res);
			if(cid != 0)
				rlen += qmi_put_tlv(resp + rlen, 0x10, &cid, sizeof cid);
			rmtfs_send_resp(fd, &peer, txn, msgid, resp, rlen);
			break;
		}
		case 0x02:	/* RMTFS_CLOSE */
		case 0x06:	/* RMTFS_FORCE_SYNC (RAM-only no-op) */
			rlen += qmi_put_tlv(resp + rlen, 0x02, res, sizeof res);
			rmtfs_send_resp(fd, &peer, txn, msgid, resp, rlen);
			break;
		case 0x03: {	/* RMTFS_RW_IOVEC */
			uchar *cv, *dv, *iv;
			u16 cl, dl, il;
			u32 cid, dir, cnt, k;
			RmtPart *rp;

			if(qmi_find_tlv(body, mlen, 0x01, &cv, &cl) < 0 || cl < 4 ||
			   qmi_find_tlv(body, mlen, 0x02, &dv, &dl) < 0 || dl < 1 ||
			   qmi_find_tlv(body, mlen, 0x03, &iv, &il) < 0 || il < 1){
				res[0] = 1;
				res[1] = 0x0003;
				rlen += qmi_put_tlv(resp + rlen, 0x02, res, sizeof res);
				rmtfs_send_resp(fd, &peer, txn, msgid, resp, rlen);
				break;
			}
			cid = le32(cv);
			dir = dv[0];
			cnt = iv[0];
			if(cid < 1 || cid > nelem(rmtparts) || rmtparts[cid-1].data == nil){
				res[0] = 1;
				res[1] = 0x0001;
				rlen += qmi_put_tlv(resp + rlen, 0x02, res, sizeof res);
				rmtfs_send_resp(fd, &peer, txn, msgid, resp, rlen);
				break;
			}
			rp = &rmtparts[cid - 1];
			lkl_printf("rmtfs: RW %s dir=%u cnt=%u\n", rp->partname, dir, cnt);
			for(k = 0; k < cnt && 1 + (k + 1) * 12 <= il; k++){
				u32 sec = le32(iv + 1 + k * 12);
				u32 moff = le32(iv + 1 + k * 12 + 4);
				u32 nsec = le32(iv + 1 + k * 12 + 8);
				u64 poff = (u64)sec * SectorSz;
				u64 bytes = (u64)nsec * SectorSz;

				if(poff + bytes > rp->size || (u64)moff + bytes > rmtmemsz)
					continue;
				if(dir == 0)	/* READ: RAM -> shared mem */
					qsys(LklNrPwrite64, rmtmemfd,
						(long)(rp->data + poff), bytes, moff, 0, 0);
				else		/* WRITE: shared mem -> RAM ONLY (never flash) */
					qsys(LklNrPread64, rmtmemfd,
						(long)(rp->data + poff), bytes, moff, 0, 0);
			}
			rlen += qmi_put_tlv(resp + rlen, 0x02, res, sizeof res);
			rmtfs_send_resp(fd, &peer, txn, msgid, resp, rlen);
			break;
		}
		case 0x04:	/* RMTFS_ALLOC_BUFF */
			rlen += qmi_put_tlv(resp + rlen, 0x02, res, sizeof res);
			rlen += qmi_put_tlv(resp + rlen, 0x10, &rmtmempa, sizeof rmtmempa);
			lkl_printf("rmtfs: ALLOC_BUFF -> pa=%#llx\n", rmtmempa);
			rmtfs_send_resp(fd, &peer, txn, msgid, resp, rlen);
			break;
		case 0x05: {	/* RMTFS_GET_DEV_ERROR */
			u32 st = 0;
			rlen += qmi_put_tlv(resp + rlen, 0x02, res, sizeof res);
			rlen += qmi_put_tlv(resp + rlen, 0x10, &st, sizeof st);
			rmtfs_send_resp(fd, &peer, txn, msgid, resp, rlen);
			break;
		}
		default:
			res[0] = 1;
			res[1] = 0x0001;
			rlen += qmi_put_tlv(resp + rlen, 0x02, res, sizeof res);
			rmtfs_send_resp(fd, &peer, txn, msgid, resp, rlen);
			break;
		}
	}
}

void
qcom_qrtr_start(void)
{
	if(lklhost_spawn(tqftp_thread, nil) < 0)
		lkl_printf("qcom_qrtr: spawn tqftpserv failed\n");
	if(lklhost_spawn(rmtfs_thread, nil) < 0)
		lkl_printf("qcom_qrtr: spawn rmtfs failed\n");
}
