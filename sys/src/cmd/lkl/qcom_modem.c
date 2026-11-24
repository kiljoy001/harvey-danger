/*
 * In-process Qualcomm QMI cellular modem client over AF_QIPCRTR for Bengal / SM6115.
 *
 * Exposes live MPSS cellular status and control over 9P (/mnt/lkl/modemstatus
 * and /mnt/lkl/modemctl):
 *   - QRTR service discovery (WDS, DMS, NAS, WMS, VOICE, UIM, etc.)
 *   - DMS: firmware revision, IMEI, operating mode (ONLINE / LPM)
 *   - UIM: SIM slot presence and card state
 *   - NAS: registration state, signal info (LTE/WCDMA/GSM), and live RF network scan
 */
#include <u.h>
#include <libc.h>

extern long	lkl_syscall(long no, long *params);
extern int	lkl_printf(const char *fmt, ...);

enum {
	LklNrClose	= 57,
	LklNrSocket	= 198,
	LklNrBind	= 200,
	LklNrGetsockname = 204,
	LklNrSendto	= 206,
	LklNrRecvfrom	= 207,
	LklNrSetsockopt	= 208,

	AfQipcrtr	= 42,
	SockDgram	= 2,
	SolSocket	= 1,
	SoRcvtimeo	= 20,

	QrtrPortCtrl	= 0xfffffffeU,
	QrtrNewServer	= 4,
	QrtrNewLookup	= 10,

	/* Standard QMI service IDs */
	QmiSvcWds	= 0x01,
	QmiSvcDms	= 0x02,
	QmiSvcNas	= 0x03,
	QmiSvcWms	= 0x05,
	QmiSvcVoice	= 0x09,
	QmiSvcUim	= 0x0b,

	/* QMI message types */
	QmiTypeReq	= 0,
	QmiTypeResp	= 2,
	QmiTypeInd	= 4,

	/* DMS message IDs */
	DmsGetRevId	= 0x0023,
	DmsGetSerials	= 0x0025,
	DmsGetOpMode	= 0x002d,
	DmsSetOpMode	= 0x002e,

	/* UIM message IDs */
	UimGetCardStat	= 0x002f,

	/* NAS message IDs */
	NasPerformScan	= 0x0021,
	NasGetServSys	= 0x0024,
	NasGetSigInfo	= 0x004f,

	/* WMS (SMS) message IDs */
	WmsRawSend	= 0x0020,
	WmsListMsgs	= 0x0031,

	/* VOICE message IDs */
	VoiceDialCall	= 0x0020,
	VoiceEndCall	= 0x0021,
	VoiceAnswerCall	= 0x0022,
	VoiceGetAllCalls = 0x002f,

	/* WDS (Mobile Data) message IDs */
	WdsStartNet	= 0x0020,
	WdsStopNet	= 0x0021,
	WdsGetPktStatus	= 0x0022,
	WdsGetSettings	= 0x002d,
	WdsBindMuxPort	= 0x00a2,

	MaxQrtrSvcs	= 64,
	MaxScanNets	= 24,
	MaxVoiceCalls	= 8,
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

typedef struct QrtrSvc QrtrSvc;
struct QrtrSvc {
	u32	service;
	u32	instance;
	u32	node;
	u32	port;
};

typedef struct ScanNet ScanNet;
struct ScanNet {
	u16	mcc;
	u16	mnc;
	u8	status;
	u8	rat;
	char	desc[48];
};

static struct {
	int	probed;
	int	nsvcs;
	QrtrSvc	svcs[MaxQrtrSvcs];

	char	rev[96];
	char	imei[32];
	int	opmode;		/* -1 unknown, 0 online, 1 lpm, ... */
	u16	offline_reason;
	int	hw_restricted;
	int	simslots;
	int	simstate[2];	/* 0 absent, 1 present, 2 error, -1 unknown */

	int	regstate;	/* -1 unknown, 0 not_reg, 1 reg, 2 searching, 3 denied */
	int	rat;		/* radio interface */
	int	has_lte_sig;
	signed char	lte_rssi;
	signed char	lte_rsrq;
	short	lte_rsrp;
	short	lte_snr;

	int	nscan;
	ScanNet	scan[MaxScanNets];
	int	last_call_id;
	int	ncalls;
	struct {
		u8	id;
		u8	state;
		u8	type;
		u8	dir;
	} calls[MaxVoiceCalls];
	int	sms_stored;
	u32	wds_handle;
	int	wds_status;	/* 1=disconnected, 2=connected, 3=suspended */
	u32	wds_ip;
	u32	wds_gw;
	u32	wds_mask;
	u32	wds_dns1;
	u32	wds_mtu;
	char	info[128];
} mst = {
	.opmode = -1,
	.simstate = { -1, -1 },
	.regstate = -1,
	.sms_stored = -1,
};

static u16 qmi_txnid = 1;

static long
msys(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
	long p[6] = { a0, a1, a2, a3, a4, a5 };
	return lkl_syscall(nr, p);
}

static u16
le16(const uchar *p)
{
	return (u16)p[0] | ((u16)p[1] << 8);
}

static void
putle16(uchar *p, u16 v)
{
	p[0] = (uchar)v;
	p[1] = (uchar)(v >> 8);
}

static int
set_rcvtimeo(int fd, long sec)
{
	struct { long tv_sec; long tv_usec; } tv;
	tv.tv_sec = sec;
	tv.tv_usec = 0;
	return (int)msys(LklNrSetsockopt, fd, SolSocket, SoRcvtimeo,
		(long)&tv, sizeof tv, 0);
}

static int
qrtr_open_client(SockaddrQrtr *meout, long timeout_sec)
{
	SockaddrQrtr me;
	u32 alen;
	long fd;

	fd = msys(LklNrSocket, AfQipcrtr, SockDgram, 0, 0, 0, 0);
	if(fd < 0)
		return -1;
	memset(&me, 0, sizeof me);
	alen = sizeof me;
	if(msys(LklNrGetsockname, fd, (long)&me, (long)&alen, 0, 0, 0) < 0){
		msys(LklNrClose, fd, 0, 0, 0, 0, 0);
		return -1;
	}
	if(msys(LklNrBind, fd, (long)&me, sizeof me, 0, 0, 0) < 0){
		msys(LklNrClose, fd, 0, 0, 0, 0, 0);
		return -1;
	}
	alen = sizeof me;
	if(msys(LklNrGetsockname, fd, (long)&me, (long)&alen, 0, 0, 0) < 0){
		msys(LklNrClose, fd, 0, 0, 0, 0, 0);
		return -1;
	}
	set_rcvtimeo((int)fd, timeout_sec);
	if(meout != nil)
		*meout = me;
	return (int)fd;
}

static int
qrtr_discover(void)
{
	SockaddrQrtr me, ctrl, from;
	QrtrCtrlPkt req, resp;
	u32 alen;
	int fd;
	long m;

	fd = qrtr_open_client(&me, 2);
	if(fd < 0)
		return -1;

	memset(&ctrl, 0, sizeof ctrl);
	ctrl.sq_family = AfQipcrtr;
	ctrl.sq_node = me.sq_node;
	ctrl.sq_port = QrtrPortCtrl;

	memset(&req, 0, sizeof req);
	req.cmd = QrtrNewLookup;
	req.service = 0;
	req.instance = 0;

	if(msys(LklNrSendto, fd, (long)&req, sizeof req, 0, (long)&ctrl, sizeof ctrl) < 0){
		msys(LklNrClose, fd, 0, 0, 0, 0, 0);
		return -1;
	}

	mst.nsvcs = 0;
	for(;;){
		alen = sizeof from;
		m = msys(LklNrRecvfrom, fd, (long)&resp, sizeof resp, 0,
			(long)&from, (long)&alen);
		if(m < (long)sizeof resp)
			break;
		if(resp.cmd != QrtrNewServer)
			continue;
		if(resp.service == 0 && resp.instance == 0 &&
		   resp.node == 0 && resp.port == 0)
			break;
		if(mst.nsvcs < MaxQrtrSvcs){
			mst.svcs[mst.nsvcs].service = resp.service;
			mst.svcs[mst.nsvcs].instance = resp.instance;
			mst.svcs[mst.nsvcs].node = resp.node;
			mst.svcs[mst.nsvcs].port = resp.port;
			mst.nsvcs++;
		}
	}
	msys(LklNrClose, fd, 0, 0, 0, 0, 0);
	lkl_printf("qcom_modem: discovered %d QRTR services\n", mst.nsvcs);
	return mst.nsvcs;
}

static QrtrSvc*
find_svc(u32 svc)
{
	int i;

	for(i = 0; i < mst.nsvcs; i++)
		if(mst.svcs[i].service == svc)
			return &mst.svcs[i];
	return nil;
}

/*
 * Send a QMI request with optional TLVs to a discovered QRTR service and
 * receive the matching QMI response.  Returns response TLV payload length
 * (>= 0) into resp_tlv, or negative error code (or -1000 - qmi_err).
 */
static int
qmi_txn(u32 svc_id, u16 msg_id, const uchar *req_tlv, int req_len,
	uchar *resp_tlv, int resp_max, long timeout_sec)
{
	QrtrSvc *s;
	SockaddrQrtr dst, from;
	uchar tx[512], rx[4096];
	u32 alen;
	u16 tid;
	int fd, tries;
	long m;

	s = find_svc(svc_id);
	if(s == nil)
		return -2;
	if(7 + req_len > (int)sizeof tx)
		return -3;

	fd = qrtr_open_client(nil, timeout_sec);
	if(fd < 0)
		return -4;

	tid = qmi_txnid++;
	if(qmi_txnid == 0)
		qmi_txnid = 1;

	tx[0] = QmiTypeReq;
	putle16(tx + 1, tid);
	putle16(tx + 3, msg_id);
	putle16(tx + 5, (u16)req_len);
	if(req_len > 0)
		memmove(tx + 7, req_tlv, req_len);

	memset(&dst, 0, sizeof dst);
	dst.sq_family = AfQipcrtr;
	dst.sq_node = s->node;
	dst.sq_port = s->port;

	if(msys(LklNrSendto, fd, (long)tx, 7 + req_len, 0, (long)&dst, sizeof dst) < 0){
		msys(LklNrClose, fd, 0, 0, 0, 0, 0);
		return -5;
	}

	for(tries = 0; tries < 16; tries++){
		alen = sizeof from;
		m = msys(LklNrRecvfrom, fd, (long)rx, sizeof rx, 0,
			(long)&from, (long)&alen);
		if(m < 7){
			msys(LklNrClose, fd, 0, 0, 0, 0, 0);
			return -6;
		}
		if(rx[0] != QmiTypeResp)
			continue;
		if(le16(rx + 1) != tid || le16(rx + 3) != msg_id)
			continue;
		int tlen = le16(rx + 5);
		if(7 + tlen > (int)m)
			tlen = (int)m - 7;
		if(tlen > resp_max)
			tlen = resp_max;
		if(resp_tlv != nil && tlen > 0)
			memmove(resp_tlv, rx + 7, tlen);
		msys(LklNrClose, fd, 0, 0, 0, 0, 0);
		return tlen;
	}
	msys(LklNrClose, fd, 0, 0, 0, 0, 0);
	return -7;
}

static const uchar*
qmi_find_tlv(const uchar *buf, int len, u8 type, int *vlen)
{
	int pos = 0;

	while(pos + 3 <= len){
		u8 t = buf[pos];
		int l = le16(buf + pos + 1);
		if(pos + 3 + l > len)
			break;
		if(t == type){
			if(vlen != nil)
				*vlen = l;
			return buf + pos + 3;
		}
		pos += 3 + l;
	}
	return nil;
}

static int
qmi_result_err(const uchar *buf, int len)
{
	const uchar *v;
	int vl;

	v = qmi_find_tlv(buf, len, 0x02, &vl);
	if(v == nil || vl < 4)
		return 0;
	if(le16(v) == 0)
		return 0;
	return (int)le16(v + 2);
}

static void
query_dms(void)
{
	uchar r[512];
	const uchar *v;
	int n, vl;

	n = qmi_txn(QmiSvcDms, DmsGetRevId, nil, 0, r, sizeof r, 3);
	if(n >= 0 && qmi_result_err(r, n) == 0){
		v = qmi_find_tlv(r, n, 0x01, &vl);
		if(v != nil && vl > 0){
			if(vl >= (int)sizeof mst.rev)
				vl = sizeof(mst.rev) - 1;
			memmove(mst.rev, v, vl);
			mst.rev[vl] = 0;
		}
	}

	n = qmi_txn(QmiSvcDms, DmsGetSerials, nil, 0, r, sizeof r, 3);
	if(n >= 0 && qmi_result_err(r, n) == 0){
		v = qmi_find_tlv(r, n, 0x11, &vl);
		if(v != nil && vl > 0){
			if(vl >= (int)sizeof mst.imei)
				vl = sizeof(mst.imei) - 1;
			memmove(mst.imei, v, vl);
			mst.imei[vl] = 0;
		}
	}

	n = qmi_txn(QmiSvcDms, DmsGetOpMode, nil, 0, r, sizeof r, 3);
	if(n >= 0 && qmi_result_err(r, n) == 0){
		v = qmi_find_tlv(r, n, 0x01, &vl);
		if(v != nil && vl >= 1)
			mst.opmode = v[0];
		v = qmi_find_tlv(r, n, 0x10, &vl);
		mst.offline_reason = (v != nil && vl >= 2) ? le16(v) : 0;
		v = qmi_find_tlv(r, n, 0x11, &vl);
		mst.hw_restricted = (v != nil && vl >= 1) ? v[0] : -1;
	}
}

static void
query_uim(void)
{
	uchar r[512];
	const uchar *v;
	int n, vl, i, pos;

	n = qmi_txn(QmiSvcUim, UimGetCardStat, nil, 0, r, sizeof r, 3);
	if(n < 0 || qmi_result_err(r, n) != 0)
		return;
	/*
	 * TLV 0x10 (Card Status):
	 *   u16 index_gw_pri, u16 index_1x_pri, u16 index_gw_sec, u16 index_1x_sec,
	 *   u8 num_slots,
	 *   slot[]: u8 card_state, u8 upin_state, u8 upin_retries, u8 upuk_retries,
	 *           u8 error_code, u8 num_app, app[]...
	 */
	v = qmi_find_tlv(r, n, 0x10, &vl);
	if(v == nil || vl < 9)
		return;
	mst.simslots = v[8];
	pos = 9;
	for(i = 0; i < mst.simslots && i < 2 && pos + 6 <= vl; i++){
		int napp;
		mst.simstate[i] = v[pos];
		napp = v[pos + 5];
		pos += 6;
		/* Skip application descriptors to reach next slot */
		while(napp-- > 0 && pos + 2 <= vl){
			int aidlen;
			if(pos + 3 > vl)
				break;
			aidlen = v[pos + 2];
			pos += 3 + aidlen;
			if(pos + 1 > vl)
				break;
			pos += 1 + v[pos];	/* label */
			pos += 1 + 4 + 4;	/* upin_replaced + pin1 + pin2 */
		}
	}
}

static void
query_nas(void)
{
	uchar r[512];
	const uchar *v;
	int n, vl;

	n = qmi_txn(QmiSvcNas, NasGetServSys, nil, 0, r, sizeof r, 3);
	if(n >= 0 && qmi_result_err(r, n) == 0){
		v = qmi_find_tlv(r, n, 0x01, &vl);
		if(v != nil && vl >= 6){
			mst.regstate = v[0];
			if(v[5] > 0 && vl >= 7)
				mst.rat = v[6];
		}
	}

	mst.has_lte_sig = 0;
	n = qmi_txn(QmiSvcNas, NasGetSigInfo, nil, 0, r, sizeof r, 3);
	if(n >= 0 && qmi_result_err(r, n) == 0){
		v = qmi_find_tlv(r, n, 0x14, &vl);	/* LTE signal strength */
		if(v != nil && vl >= 6){
			mst.has_lte_sig = 1;
			mst.lte_rssi = (signed char)v[0];
			mst.lte_rsrq = (signed char)v[1];
			mst.lte_rsrp = (short)le16(v + 2);
			mst.lte_snr  = (short)le16(v + 4);
		}
	}
}

int
modem_probe(void)
{
	int n;

	n = qrtr_discover();
	if(n <= 0){
		snprint(mst.info, sizeof mst.info, "no QRTR services (mpss not ready)");
		return -1;
	}
	mst.probed = 1;
	query_dms();
	query_uim();
	query_nas();
	snprint(mst.info, sizeof mst.info, "probe ok (%d qrtr svcs)", n);
	return 0;
}

int
modem_set_opmode(int mode)
{
	uchar req[4], resp[128];
	int n, err;

	if(!mst.probed)
		modem_probe();
	req[0] = 0x01;
	putle16(req + 1, 1);
	req[3] = (uchar)mode;
	n = qmi_txn(QmiSvcDms, DmsSetOpMode, req, 4, resp, sizeof resp, 5);
	if(n < 0){
		snprint(mst.info, sizeof mst.info, "set_opmode(%d) txn err %d", mode, n);
		return -1;
	}
	err = qmi_result_err(resp, n);
	if(err != 0){
		snprint(mst.info, sizeof mst.info, "set_opmode(%d) qmi err %d", mode, err);
		return -1;
	}
	query_dms();
	query_nas();
	snprint(mst.info, sizeof mst.info, "opmode set to %d", mst.opmode);
	return 0;
}

int
modem_scan(void)
{
	uchar req[4], resp[2048];
	const uchar *v, *vr;
	int n, vl, vrl, err, cnt, pos, i, rcnt, rpos;

	if(!mst.probed)
		modem_probe();

	/* TLV 0x10 = network_type bitmask: 0x07 = GSM | UMTS | LTE */
	req[0] = 0x10;
	putle16(req + 1, 1);
	req[3] = 0x07;

	snprint(mst.info, sizeof mst.info, "scanning...");
	n = qmi_txn(QmiSvcNas, NasPerformScan, req, 4, resp, sizeof resp, 45);
	if(n < 0){
		snprint(mst.info, sizeof mst.info, "scan txn err %d", n);
		return -1;
	}
	err = qmi_result_err(resp, n);
	if(err != 0){
		snprint(mst.info, sizeof mst.info, "scan qmi err %d", err);
		return -1;
	}

	mst.nscan = 0;
	v = qmi_find_tlv(resp, n, 0x10, &vl);
	if(v != nil && vl >= 2){
		cnt = le16(v);
		pos = 2;
		for(i = 0; i < cnt && mst.nscan < MaxScanNets && pos + 6 <= vl; i++){
			ScanNet *sn = &mst.scan[mst.nscan];
			int dlen;
			sn->mcc = le16(v + pos);
			sn->mnc = le16(v + pos + 2);
			sn->status = v[pos + 4];
			dlen = v[pos + 5];
			pos += 6;
			if(pos + dlen > vl)
				break;
			int cpy = dlen < (int)sizeof(sn->desc) - 1 ? dlen : (int)sizeof(sn->desc) - 1;
			memmove(sn->desc, v + pos, cpy);
			sn->desc[cpy] = 0;
			sn->rat = 0;
			pos += dlen;
			mst.nscan++;
		}
	}
	/* Match RAT info from TLV 0x12 if present */
	vr = qmi_find_tlv(resp, n, 0x12, &vrl);
	if(vr != nil && vrl >= 2){
		rcnt = le16(vr);
		rpos = 2;
		for(i = 0; i < rcnt && rpos + 5 <= vrl; i++){
			u16 mcc = le16(vr + rpos);
			u16 mnc = le16(vr + rpos + 2);
			u8 rat = vr[rpos + 4];
			rpos += 5;
			if(i < mst.nscan && mst.scan[i].mcc == mcc && mst.scan[i].mnc == mnc)
				mst.scan[i].rat = rat;
		}
	}
	query_nas();
	snprint(mst.info, sizeof mst.info, "scan ok (%d networks)", mst.nscan);
	return mst.nscan;
}

/* ---- Stage 2: QMI VOICE (0x09) & WMS SMS (0x05) ---- */

int
modem_voice_dial(const char *number)
{
	uchar req[96], resp[128];
	const uchar *v;
	int n, vl, err, nlen, pos = 0;

	if(!mst.probed)
		modem_probe();
	if(number == nil || number[0] == 0){
		snprint(mst.info, sizeof mst.info, "dial: missing number");
		return -1;
	}
	nlen = strlen(number);
	if(nlen > 64)
		nlen = 64;
	/* TLV 0x01: calling_number (ASCII string) */
	req[pos++] = 0x01;
	putle16(req + pos, nlen); pos += 2;
	memmove(req + pos, number, nlen); pos += nlen;
	/* TLV 0x10: call_type (0x00 = voice) */
	req[pos++] = 0x10;
	putle16(req + pos, 1); pos += 2;
	req[pos++] = 0x00;

	n = qmi_txn(QmiSvcVoice, VoiceDialCall, req, pos, resp, sizeof resp, 10);
	if(n < 0){
		snprint(mst.info, sizeof mst.info, "dial(%s) txn err %d", number, n);
		return -1;
	}
	err = qmi_result_err(resp, n);
	if(err != 0){
		snprint(mst.info, sizeof mst.info, "dial(%s) qmi err %d", number, err);
		return -1;
	}
	v = qmi_find_tlv(resp, n, 0x10, &vl);
	if(v != nil && vl >= 1)
		mst.last_call_id = v[0];
	snprint(mst.info, sizeof mst.info, "dial(%s) ok call_id=%d", number, mst.last_call_id);
	return mst.last_call_id;
}

int
modem_voice_answer(int call_id)
{
	uchar req[8], resp[128];
	int n, err;

	if(!mst.probed)
		modem_probe();
	if(call_id <= 0)
		call_id = mst.last_call_id > 0 ? mst.last_call_id : 1;
	req[0] = 0x01;
	putle16(req + 1, 1);
	req[3] = (uchar)call_id;
	n = qmi_txn(QmiSvcVoice, VoiceAnswerCall, req, 4, resp, sizeof resp, 5);
	if(n < 0){
		snprint(mst.info, sizeof mst.info, "answer(%d) txn err %d", call_id, n);
		return -1;
	}
	err = qmi_result_err(resp, n);
	if(err != 0){
		snprint(mst.info, sizeof mst.info, "answer(%d) qmi err %d", call_id, err);
		return -1;
	}
	snprint(mst.info, sizeof mst.info, "answer(%d) ok", call_id);
	return 0;
}

int
modem_voice_hangup(int call_id)
{
	uchar req[8], resp[128];
	int n, err;

	if(!mst.probed)
		modem_probe();
	if(call_id <= 0)
		call_id = mst.last_call_id > 0 ? mst.last_call_id : 1;
	req[0] = 0x01;
	putle16(req + 1, 1);
	req[3] = (uchar)call_id;
	n = qmi_txn(QmiSvcVoice, VoiceEndCall, req, 4, resp, sizeof resp, 5);
	if(n < 0){
		snprint(mst.info, sizeof mst.info, "hangup(%d) txn err %d", call_id, n);
		return -1;
	}
	err = qmi_result_err(resp, n);
	if(err != 0){
		snprint(mst.info, sizeof mst.info, "hangup(%d) qmi err %d", call_id, err);
		return -1;
	}
	snprint(mst.info, sizeof mst.info, "hangup(%d) ok", call_id);
	return 0;
}

int
modem_voice_calls(void)
{
	uchar resp[512];
	const uchar *v;
	int n, vl, err, cnt, i;

	if(!mst.probed)
		modem_probe();
	n = qmi_txn(QmiSvcVoice, VoiceGetAllCalls, nil, 0, resp, sizeof resp, 5);
	if(n < 0){
		snprint(mst.info, sizeof mst.info, "calls txn err %d", n);
		return -1;
	}
	err = qmi_result_err(resp, n);
	if(err != 0){
		snprint(mst.info, sizeof mst.info, "calls qmi err %d", err);
		return -1;
	}
	mst.ncalls = 0;
	/* TLV 0x10: Call Information (u8 count, entries of 9 bytes each) */
	v = qmi_find_tlv(resp, n, 0x10, &vl);
	if(v != nil && vl >= 1){
		cnt = v[0];
		for(i = 0; i < cnt && i < MaxVoiceCalls && 1 + (i + 1) * 9 <= vl; i++){
			const uchar *e = v + 1 + i * 9;
			mst.calls[mst.ncalls].id = e[0];
			mst.calls[mst.ncalls].state = e[1];
			mst.calls[mst.ncalls].type = e[2];
			mst.calls[mst.ncalls].dir = e[3];
			mst.ncalls++;
		}
	}
	snprint(mst.info, sizeof mst.info, "calls ok (%d active)", mst.ncalls);
	return mst.ncalls;
}

int
modem_wms_list(void)
{
	uchar req[12], resp[512];
	const uchar *v;
	int n, vl, err, pos = 0;

	if(!mst.probed)
		modem_probe();
	/* TLV 0x01: storage_type (1 = NV), TLV 0x11: message_mode (1 = GW/3GPP) */
	req[pos++] = 0x01; putle16(req + pos, 1); pos += 2; req[pos++] = 0x01;
	req[pos++] = 0x11; putle16(req + pos, 1); pos += 2; req[pos++] = 0x01;
	n = qmi_txn(QmiSvcWms, WmsListMsgs, req, pos, resp, sizeof resp, 5);
	if(n < 0){
		snprint(mst.info, sizeof mst.info, "smslist txn err %d", n);
		return -1;
	}
	err = qmi_result_err(resp, n);
	if(err != 0){
		snprint(mst.info, sizeof mst.info, "smslist qmi err %d", err);
		return -1;
	}
	v = qmi_find_tlv(resp, n, 0x01, &vl);
	mst.sms_stored = (v != nil && vl >= 4) ?
		(int)((u32)v[0] | ((u32)v[1] << 8) | ((u32)v[2] << 16) | ((u32)v[3] << 24)) : 0;
	snprint(mst.info, sizeof mst.info, "smslist ok (%d stored)", mst.sms_stored);
	return mst.sms_stored;
}

/* Encode 3GPP SMS-SUBMIT PDU (GSM 7-bit default alphabet) and send via QMI WMS */
int
modem_wms_send(const char *dest, const char *text)
{
	uchar pdu[256], req[280], resp[128];
	char digits[32];
	int nd = 0, toa = 0x81, p = 0, tlen, i, n, err;

	if(!mst.probed)
		modem_probe();
	if(dest == nil || text == nil || dest[0] == 0 || text[0] == 0){
		snprint(mst.info, sizeof mst.info, "sms: usage sms <number> <text>");
		return -1;
	}
	if(dest[0] == '+'){
		toa = 0x91;
		dest++;
	}
	for(i = 0; dest[i] != 0 && nd < (int)sizeof(digits) - 1; i++)
		if(dest[i] >= '0' && dest[i] <= '9')
			digits[nd++] = dest[i] - '0';
	if(nd == 0){
		snprint(mst.info, sizeof mst.info, "sms: invalid number");
		return -1;
	}

	pdu[p++] = 0x00;		/* SMSC length = 0 (use modem default) */
	pdu[p++] = 0x11;		/* SMS-SUBMIT, VPF=relative */
	pdu[p++] = 0x00;		/* TP-MR */
	pdu[p++] = (uchar)nd;		/* TP-DA digit count */
	pdu[p++] = (uchar)toa;		/* TP-DA type-of-address */
	for(i = 0; i < nd; i += 2){
		uchar lo = digits[i];
		uchar hi = (i + 1 < nd) ? digits[i + 1] : 0x0f;
		pdu[p++] = lo | (hi << 4);
	}
	pdu[p++] = 0x00;		/* TP-PID */
	pdu[p++] = 0x00;		/* TP-DCS (GSM 7-bit) */
	pdu[p++] = 0xa7;		/* TP-VP (24 hours) */

	tlen = strlen(text);
	if(tlen > 160)
		tlen = 160;
	pdu[p++] = (uchar)tlen;		/* TP-UDL (septets) */
	/* Pack 7-bit septets into octets */
	{
		int bitpos = 0;
		uchar cur = 0;
		for(i = 0; i < tlen && p < (int)sizeof(pdu) - 1; i++){
			uchar c = (uchar)text[i] & 0x7f;
			cur |= (uchar)(c << bitpos);
			bitpos += 7;
			if(bitpos >= 8){
				pdu[p++] = cur;
				bitpos -= 8;
				cur = (uchar)(c >> (7 - bitpos));
			}
		}
		if(bitpos > 0 && p < (int)sizeof(pdu))
			pdu[p++] = cur;
	}

	/* TLV 0x01: raw_message_data { u8 format(0x06=GW_PP); u16 len; u8 pdu[]; } */
	req[0] = 0x01;
	putle16(req + 1, 3 + p);
	req[3] = 0x06;
	putle16(req + 4, p);
	memmove(req + 6, pdu, p);

	n = qmi_txn(QmiSvcWms, WmsRawSend, req, 6 + p, resp, sizeof resp, 15);
	if(n < 0){
		snprint(mst.info, sizeof mst.info, "sms txn err %d", n);
		return -1;
	}
	err = qmi_result_err(resp, n);
	if(err != 0){
		snprint(mst.info, sizeof mst.info, "sms qmi err %d", err);
		return -1;
	}
	snprint(mst.info, sizeof mst.info, "sms ok (%dB PDU)", p);
	return 0;
}

/* ---- Stage 3 Control: QMI WDS (0x01) Mobile Data over rmnet_ipa0 ---- */

static u32
wds_le32(const uchar *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void
putle32(uchar *p, u32 v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

int
modem_wds_status(void)
{
	uchar req[8], resp[512];
	const uchar *v;
	int n, vl, err;

	if(!mst.probed)
		modem_probe();
	n = qmi_txn(QmiSvcWds, WdsGetPktStatus, nil, 0, resp, sizeof resp, 5);
	if(n < 0){
		snprint(mst.info, sizeof mst.info, "datastat txn err %d", n);
		return -1;
	}
	err = qmi_result_err(resp, n);
	if(err != 0){
		snprint(mst.info, sizeof mst.info, "datastat qmi err %d", err);
		return -1;
	}
	v = qmi_find_tlv(resp, n, 0x01, &vl);
	mst.wds_status = (v != nil && vl >= 1) ? v[0] : 0;

	if(mst.wds_status == 2){
		/* Request IPv4 addr | GW | subnet | DNS | MTU */
		req[0] = 0x10; putle16(req + 1, 4); putle32(req + 3, 0x00003370);
		n = qmi_txn(QmiSvcWds, WdsGetSettings, req, 7, resp, sizeof resp, 5);
		if(n >= 0 && qmi_result_err(resp, n) == 0){
			if((v = qmi_find_tlv(resp, n, 0x1e, &vl)) != nil && vl >= 4)
				mst.wds_ip = wds_le32(v);
			if((v = qmi_find_tlv(resp, n, 0x20, &vl)) != nil && vl >= 4)
				mst.wds_gw = wds_le32(v);
			if((v = qmi_find_tlv(resp, n, 0x21, &vl)) != nil && vl >= 4)
				mst.wds_mask = wds_le32(v);
			if((v = qmi_find_tlv(resp, n, 0x15, &vl)) != nil && vl >= 4)
				mst.wds_dns1 = wds_le32(v);
			if((v = qmi_find_tlv(resp, n, 0x29, &vl)) != nil && vl >= 4)
				mst.wds_mtu = wds_le32(v);
		}
	}
	snprint(mst.info, sizeof mst.info, "datastat ok (state=%d)", mst.wds_status);
	return mst.wds_status;
}

int
modem_wds_start(const char *apn)
{
	uchar req[128], resp[256];
	const uchar *v;
	int n, vl, err, pos = 0, alen;

	if(!mst.probed)
		modem_probe();
	if(apn == nil || apn[0] == 0){
		snprint(mst.info, sizeof mst.info, "dataup: usage dataup <apn>");
		return -1;
	}
	/* Bind embedded IPA endpoint (ep_type=5 EMBEDDED, iface=1, mux_id=1) */
	req[pos++] = 0x10; putle16(req + pos, 8); pos += 2;
	putle32(req + pos, 5); pos += 4;
	putle32(req + pos, 1); pos += 4;
	req[pos++] = 0x11; putle16(req + pos, 1); pos += 2;
	req[pos++] = 1;
	qmi_txn(QmiSvcWds, WdsBindMuxPort, req, pos, resp, sizeof resp, 5);

	/* Start Network Interface: TLV 0x14=APN, TLV 0x19=IPv4(4) */
	pos = 0;
	alen = strlen(apn);
	if(alen > 64)
		alen = 64;
	req[pos++] = 0x14; putle16(req + pos, alen); pos += 2;
	memmove(req + pos, apn, alen); pos += alen;
	req[pos++] = 0x19; putle16(req + pos, 1); pos += 2;
	req[pos++] = 4;

	n = qmi_txn(QmiSvcWds, WdsStartNet, req, pos, resp, sizeof resp, 20);
	if(n < 0){
		snprint(mst.info, sizeof mst.info, "dataup(%s) txn err %d", apn, n);
		return -1;
	}
	err = qmi_result_err(resp, n);
	if(err != 0){
		snprint(mst.info, sizeof mst.info, "dataup(%s) qmi err %d", apn, err);
		return -1;
	}
	v = qmi_find_tlv(resp, n, 0x01, &vl);
	if(v != nil && vl >= 4)
		mst.wds_handle = wds_le32(v);
	modem_wds_status();
	snprint(mst.info, sizeof mst.info, "dataup(%s) ok handle=0x%x", apn, mst.wds_handle);
	return 0;
}

int
modem_wds_stop(void)
{
	uchar req[16], resp[128];
	int n, err;

	if(!mst.probed)
		modem_probe();
	req[0] = 0x01; putle16(req + 1, 4); putle32(req + 3, mst.wds_handle);
	n = qmi_txn(QmiSvcWds, WdsStopNet, req, 7, resp, sizeof resp, 10);
	if(n < 0){
		snprint(mst.info, sizeof mst.info, "datadown txn err %d", n);
		return -1;
	}
	err = qmi_result_err(resp, n);
	if(err != 0){
		snprint(mst.info, sizeof mst.info, "datadown qmi err %d", err);
		return -1;
	}
	mst.wds_handle = 0;
	mst.wds_status = 1;
	snprint(mst.info, sizeof mst.info, "datadown ok");
	return 0;
}

static const char*
opmode_str(int m)
{
	switch(m){
	case 0: return "online";
	case 1: return "low-power";
	case 2: return "ftm";
	case 3: return "offline";
	case 4: return "resetting";
	case 5: return "shutting-down";
	case 6: return "persistent-lpm";
	default: return "unknown";
	}
}

static const char*
sim_str(int s)
{
	switch(s){
	case 0: return "absent";
	case 1: return "present";
	case 2: return "error";
	default: return "unknown";
	}
}

static const char*
reg_str(int r)
{
	switch(r){
	case 0: return "not-registered";
	case 1: return "registered";
	case 2: return "searching";
	case 3: return "denied";
	default: return "unknown";
	}
}

static const char*
rat_str(int r)
{
	switch(r){
	case 4: return "GSM";
	case 5: return "UMTS";
	case 8: return "LTE";
	case 12: return "5G-NR";
	default: return "?";
	}
}

void
modemstatus_buf(char *out, int outsz)
{
	int pos = 0, i;

	if(!mst.probed)
		modem_probe();
	else{
		query_dms();
		query_nas();
	}

	pos += snprint(out + pos, outsz - pos, "qrtr_svcs %d", mst.nsvcs);
	if(find_svc(QmiSvcWds))   pos += snprint(out + pos, outsz - pos, " WDS");
	if(find_svc(QmiSvcDms))   pos += snprint(out + pos, outsz - pos, " DMS");
	if(find_svc(QmiSvcNas))   pos += snprint(out + pos, outsz - pos, " NAS");
	if(find_svc(QmiSvcWms))   pos += snprint(out + pos, outsz - pos, " WMS");
	if(find_svc(QmiSvcVoice)) pos += snprint(out + pos, outsz - pos, " VOICE");
	if(find_svc(QmiSvcUim))   pos += snprint(out + pos, outsz - pos, " UIM");
	pos += snprint(out + pos, outsz - pos, "\n");

	pos += snprint(out + pos, outsz - pos, "rev %s\n", mst.rev[0] ? mst.rev : "unknown");
	pos += snprint(out + pos, outsz - pos, "imei %s\n", mst.imei[0] ? mst.imei : "unknown");
	pos += snprint(out + pos, outsz - pos, "opmode %s (%d) reason=0x%04x hw_restr=%d\n",
		opmode_str(mst.opmode), mst.opmode, mst.offline_reason, mst.hw_restricted);
	pos += snprint(out + pos, outsz - pos, "sim slots=%d slot0=%s slot1=%s\n",
		mst.simslots, sim_str(mst.simstate[0]), sim_str(mst.simstate[1]));
	pos += snprint(out + pos, outsz - pos, "nas reg=%s rat=%s\n",
		reg_str(mst.regstate), rat_str(mst.rat));
	if(mst.has_lte_sig)
		pos += snprint(out + pos, outsz - pos, "lte rssi=%d rsrp=%d rsrq=%d snr=%d\n",
			mst.lte_rssi, mst.lte_rsrp, mst.lte_rsrq, mst.lte_snr);
	pos += snprint(out + pos, outsz - pos, "info %s\n", mst.info);
	if(mst.wds_status > 0){
		pos += snprint(out + pos, outsz - pos, "wds state=%d handle=0x%x",
			mst.wds_status, mst.wds_handle);
		if(mst.wds_ip != 0)
			pos += snprint(out + pos, outsz - pos, " ip=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u mtu=%u",
				(mst.wds_ip>>24)&0xff, (mst.wds_ip>>16)&0xff, (mst.wds_ip>>8)&0xff, mst.wds_ip&0xff,
				(mst.wds_gw>>24)&0xff, (mst.wds_gw>>16)&0xff, (mst.wds_gw>>8)&0xff, mst.wds_gw&0xff,
				(mst.wds_dns1>>24)&0xff, (mst.wds_dns1>>16)&0xff, (mst.wds_dns1>>8)&0xff, mst.wds_dns1&0xff,
				mst.wds_mtu);
		pos += snprint(out + pos, outsz - pos, "\n");
	}
	if(mst.sms_stored >= 0)
		pos += snprint(out + pos, outsz - pos, "sms stored=%d\n", mst.sms_stored);
	for(i = 0; i < mst.ncalls && pos < outsz - 64; i++)
		pos += snprint(out + pos, outsz - pos, "call id=%u state=%u type=%u dir=%u\n",
			mst.calls[i].id, mst.calls[i].state, mst.calls[i].type, mst.calls[i].dir);
	{
		extern int rmtfs_loaded_mask, rmtfs_opens, rmtfs_allocs, rmtfs_alloc_sz;
		extern int rmtfs_reads, rmtfs_writes, rmtfs_rw_skips;
		extern char rmtfs_last_open[], rmtfs_last_skip[];
		pos += snprint(out + pos, outsz - pos,
			"rmtfs loaded=0x%x open=%d alloc=%d(0x%x) rd=%d wr=%d skip=%d last=%s\n",
			rmtfs_loaded_mask, rmtfs_opens, rmtfs_allocs, rmtfs_alloc_sz,
			rmtfs_reads, rmtfs_writes, rmtfs_rw_skips,
			rmtfs_last_open[0] ? rmtfs_last_open : "none");
		if(rmtfs_last_skip[0])
			pos += snprint(out + pos, outsz - pos, "rmtfs_skip %s\n", rmtfs_last_skip);
	}
	{
		extern int tftp_rrqs, tftp_enoents, tftp_wrqs, tftp_nmisses, tftp_nhits, tftp_nwrq_log;
		extern char tftp_last_miss[], tftp_misses[][64], tftp_hits[][64], tftp_wrq_log[][320];
		int k;
		pos += snprint(out + pos, outsz - pos,
			"tftp rrq=%d miss=%d wrq=%d last_miss=%s\n",
			tftp_rrqs, tftp_enoents, tftp_wrqs,
			tftp_last_miss[0] ? tftp_last_miss : "none");
		for(k = 0; k < tftp_nhits && pos < outsz - 80; k++)
			pos += snprint(out + pos, outsz - pos, "tftp_hit %s\n", tftp_hits[k]);
		for(k = 0; k < tftp_nmisses && pos < outsz - 80; k++)
			pos += snprint(out + pos, outsz - pos, "tftp_miss %s\n", tftp_misses[k]);
		for(k = 0; k < tftp_nwrq_log && pos < outsz - 340; k++)
			pos += snprint(out + pos, outsz - pos, "tftp_wrq %s\n", tftp_wrq_log[k]);
	}
	for(i = 0; i < mst.nscan && pos < outsz - 80; i++){
		pos += snprint(out + pos, outsz - pos, "cell %03d-%02d rat=%s status=0x%02x %s\n",
			mst.scan[i].mcc, mst.scan[i].mnc, rat_str(mst.scan[i].rat),
			mst.scan[i].status, mst.scan[i].desc);
	}
}
