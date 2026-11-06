/* pp - see pp.h. Transliterated from the pp.o
 * disassembly (re/inventory/b37). */
#include "pp.h"

#include <string.h>

/* ---- blob-owned externs (same set pp.o references) ---- */
extern char g_ic;
extern char g_wifi_menuconfig;
extern char lmacConfMib;
extern uint32_t app_optimize_and_debug_mac;
extern uint8_t PendFreeBcnEb;
extern uint8_t TmpSTAAPCloseAP;
/* Defined here in the blob pp.o (COMMON in the
 * original); other blob objects bind to these. */
/* COMMON on purpose: ieee80211_api.o carries the
 * strong definition; the blob pp.o's common merged
 * into it the same way. */
void *wifi_sta_rx_probe_req;
uint8_t g_cnxMgr[1224] = { 0 };
extern void *promiscuous_cb;
extern uint32_t _g_esp_wifi_ppt_task_stk_size;

#define GICB(off) (((uint8_t *)&g_ic)[(off)])
#define GICW(off) (*(uint32_t *)((uint8_t *)&g_ic + (off)))
#define MIBB(off) (((uint8_t *)&lmacConfMib)[(off)])
#define MIBW(off) (*(uint32_t *)((uint8_t *)&lmacConfMib + (off)))
#ifdef PP_HOST_TEST
extern uint32_t pp_host_wdev_count;
#define WDEV_COUNT pp_host_wdev_count
#define MEMW() ((void)0)
#else
#define WDEV_COUNT (*(volatile uint32_t *)0x3ff20c00u)
#define MEMW() __asm__ volatile ("memw" ::: "memory")
#endif

void esf_buf_recycle(void *node, uint32_t cls);
int esp_wifi_get_pm_recv_multi_data(void);
void esf_buf_setup(void);
void esp_log_write(int level, const char *tag, const char *fmt, ...);
int ets_printf(const char *fmt, ...);
int printf(const char *fmt, ...);
void ets_timer_proc(void);
uint32_t GetAccess(uint32_t ac);
void *_heap_caps_free(void *p, const char *file, int line);
void hostap_input(uint32_t ic, PpNode *node, int len, int flag);
uint32_t ic_get_gtk_alg(uint32_t ifidx);
uint32_t ic_get_ptk_alg(uint32_t ifidx);
int ic_is_pure_sta(uint32_t ifidx);
void ieee80211_recv_action(uint32_t ic, void *frame, void *body, void *efrm);
void lmacAdjustTimestamp(PpNode *node);
void lmac_check_ac(void);
int lmacIsIdle(uint32_t ctx);
void lmacProcessTxTimeout(void);
void lmacTxFrame(PpNode *node, uint32_t ctx);
void noise_check_loop(int on);
void os_timer_arm(void *timer, uint32_t ms, int repeat);
void os_timer_disarm(void *timer);
void os_timer_setfn(void *timer, void *fn, void *arg);
void PendSV(int n);
int pm_add_aid(const void *mac, const void *end);
int pm_allow_tx(int n);
void pm_have_more_data_to_recv(void);
void pm_onBcnRx(PpNode *node, const void *a, const void *b);
void pm_recv_all_data(void);
void pm_reset_keepalive_time(void);
int pm_rf_is_closed(void);
void pm_set_addr(uint32_t ifidx, const void *mac);
int pm_sleep_for(const void *tim);
int read_hw_noisefloor(void);
uint32_t RC_GetAckTime(uint32_t rate);
uint16_t RC_GetCtsTime(uint32_t rate, uint32_t len, uint32_t extra);
void *rc_get_trc(uint32_t ifidx, const void *mac);
void rcGetSched(void *rec, void *txctl);
void rcUpdateRxDone(void *rec, const void *desc);
uint32_t RxNodeNum(void);
void sta_input(uint32_t ic, PpNode *node, int len, int flag);
void tx_pwctrl_background(int a, int b);
void vPortEnterCritical(void);
void vPortETSIntrLock(void);
void vPortETSIntrUnlock(void);
void vPortExitCritical(void);
void wDev_recycle_hw_rx_buf(void);
uint32_t __wifi_queue_create(uint32_t len, uint32_t itemsize);
uint32_t __wifi_queue_msg_num(uint32_t q);
int __wifi_queue_recv(uint32_t q, void *msg, uint32_t timeout);
int __wifi_queue_send(uint32_t q, void *msg, uint32_t timeout, int pos);
int __wifi_queue_send_from_isr(uint32_t q, void *msg, int *woken);
uint32_t __wifi_task_create(void *fn, const char *name, uint32_t stack,
			    int arg, uint32_t prio);
uint32_t __wifi_task_get_max_priority(void);
void __wifi_task_resume_all(void);
void __wifi_task_suspend_all(void);
void __wifi_task_yield_from_isr(void);


/* ---- pp.o-owned state (names from the blob's bss) ---- */
static uint8_t TxRxCxt[240];
void *pTxRx = &TxRxCxt;
uint16_t NoiseTimerInterval = 100;
static uint8_t noise_now = 23;
uint8_t pend_flag_noise_check = 0;
static uint8_t noise_test_timer[0x1c];
static uint32_t xphyQueue;
static uint8_t pp_sig_cnt[0x11];
static uint32_t s_pp_queue[32][2];
static uint32_t s_pp_queue_wptr, s_pp_queue_rptr;
static uint8_t current_ifidx;
static uint8_t need_pwctrl;
static uint8_t period_check_ac_tick, node_check_tick;
static struct {
	PpNode *node;
	uint16_t len;
	uint16_t pad;
} fragment;

#define TXRX ((uint8_t *)pTxRx)
#define QB(q) (TXRX + 24 + (q) * 16)
#define QHEAD(q) (*(PpNode **)QB(q))
#define QTAIL(q) (*(PpNode ***)(QB(q) + 4))

/* Tail-append using the queue's tail slot (offset +4
 * holds a pointer to the next-field to fill). */
static void
q_push(uint8_t *qbase, PpNode *node)
{
	node->next = NULL;
	**(PpNode ***)(qbase + 4) = node;
	*(PpNode ***)(qbase + 4) = &node->next;
}

static PpNode *
q_pop(uint8_t *qbase)
{
	PpNode *node = *(PpNode **)qbase;

	if (node != NULL) {
		vPortEnterCritical();
		*(PpNode **)qbase = node->next;
		if (node->next == NULL)
			*(PpNode ***)(qbase + 4) = (PpNode **)qbase;
		vPortExitCritical();
		node->next = NULL;
	}
	return node;
}

void
ppEnqueueTxDone(PpNode *node)
{
		if (node == NULL) {
		ets_printf("%s %u\n", "pp", 0x1fa);
		for (;;)
			;
	}
	vPortEnterCritical();
	q_push(TXRX + 0xa8, node);
	vPortExitCritical();
}

void
ppEnqueueRxq(PpNode *node)
{
		vPortEnterCritical();
	q_push(TXRX + 0xb0, node);
	vPortExitCritical();
}

PpNode *
ppDequeueTxQ(uint32_t q)
{
		q &= 0xFF;
	if (q >= 8)
		return NULL;
	return q_pop(QB(q));
}

/* Pop like ppDequeueTxQ but pre-checks head without
 * the critical section (blob does the racy read). */
static PpNode *
dequeue_locked_span(uint8_t *qbase)
{
	PpNode *node = *(PpNode **)qbase;

	if (node != NULL) {
		vPortEnterCritical();
		*(PpNode **)qbase = node->next;
		if (node->next == NULL)
			*(PpNode ***)(qbase + 4) = (PpNode **)qbase;
		vPortExitCritical();
		node->next = NULL;
	}
	return node;
}

PpNode *
ppFetchTxQFirstAvail(uint32_t q)
{
		q &= 0xFF;
	if (q >= 8)
		return NULL;
	if (QB(q)[11] != 0)
		return NULL;
	return dequeue_locked_span(QB(q));
}

PpNode *
ppGetTxQFirstAvail_Locked(uint32_t q)
{
		PpNode *node = NULL;

	q &= 0xFF;
	if (q < 8 && QB(q)[11] == 0) {
		vPortETSIntrLock();
		node = QHEAD(q);
		vPortETSIntrUnlock();
	}
	return node;
}

void
ppRollBackTxQ(PpNode *node)
{
		uint32_t q = (node->ctrl[1] >> 2) & 0xF;

	if (q < 8) {
		uint8_t *qb = QB(q);

		vPortEnterCritical();
		node->next = *(PpNode **)qb;
		if (node->next == NULL)
			*(PpNode ***)(qb + 4) = &node->next;
		*(PpNode **)qb = node;
		vPortExitCritical();
	}
}

void
ppRecycleRxPkt(PpNode *node)
{
		wDev_recycle_hw_rx_buf();
	esf_buf_recycle(node, 7);
}

void
ppProcessRxPktHdr(void *hdr)
{
		uint32_t *h = (uint32_t *)hdr;

	if (promiscuous_cb != NULL) {
		char *frame = (char *)(uintptr_t)h[1];

		*frame = (char)(*frame - 0x60);
		((void (*)(char *, uint32_t))promiscuous_cb)(frame, h[0]);
	}
	_heap_caps_free(hdr, "pp", 0x131);
}

int
ppRegisterTxCallback(void *fn, uint32_t idx)
{
		uint32_t bit = 1u << (idx & 0x1F);
	int taken = (*(uint32_t *)(TXRX + 0xb8) & bit) != 0;

	if (!taken) {
		*(uint32_t *)(TXRX + 0xb8) |= bit;
		*(void **)(TXRX + (idx & 0xFF) * 4 + 0xbc) = fn;
	}
	return taken;
}

void
ppUnregisterTxCallback(uint32_t idx)
{
	uint32_t bit = 1u << (idx & 0x1F);

	if ((*(uint32_t *)(TXRX + 0xb8) & bit) != 0) {
		*(uint32_t *)(TXRX + 0xb8) &= ~bit;
		*(void **)(TXRX + (idx & 0xFF) * 4 + 0xbc) = NULL;
	}
}

int32_t
TxNodeNum(void)
{
		extern char wDevCtrl;
	int32_t n = 5;
	void *p;

	if ((*(uint8_t *)&wDevCtrl) == 0) {
		n = 0;
		for (p = *(void **)((uint8_t *)&wDevCtrl + 0x18); p != NULL;
		     p = *(void **)((uint8_t *)p + 8))
			n++;
	}
	return n;
}

static int
ieee80211_hdrsize(const uint8_t *fc)
{
	int n = 0x1e;

	if ((fc[1] & 3) != 3)
		n = 0x18;
	if ((fc[0] & 0x8c) == 0x88)
		n += 2;
	return n;
}

static int
lzcount(uint32_t x)
{
	return x ? __builtin_clz(x) : 32;
}

/* Queue state byte -> access category code. */
static uint32_t
ac_from_state(uint32_t state)
{
	if (state == 0 || state == 3)
		return 2;
	if (state > 2)
		return state > 5 ? 0 : 1;
	return 3;
}

static void
ppEnableQueue(uint32_t q, uint8_t b10, uint8_t b9, uint8_t state,
	      int32_t ctx)
{
	if (q > 1) {
		uint8_t *qb = QB(q);

		qb[10] = b10;
		qb[12] = 0;
		qb[8] = state;
		qb[9] = b9;
		*(uint32_t *)(TXRX + (ctx + 1) * 4) |= 1u << (q & 0x1F);
		*(uint32_t *)TXRX |= 1u << (q & 0x1F);
	}
}

static void
ppDisableQueue(uint32_t q, int32_t ctx)
{
	*(uint32_t *)(TXRX + (ctx + 1) * 4) &= ~(1u << (q & 0x1F));
	*(uint32_t *)TXRX &= ~(1u << (q & 0x1F));
	pp_post(8, 0);
}

static uint32_t
ppSearchTxQueue(uint32_t p1, uint32_t p2, uint32_t p3, int32_t ctx)
{
	uint32_t mask = *(uint32_t *)(TXRX + (ctx + 1) * 4);
	uint32_t word0 = *(uint32_t *)TXRX;
	uint32_t q, u4, u5, u6;
	int32_t i3;

	if (mask == 0) {
		q = TXRX[ctx + 0x14];
		if ((word0 & (1u << (q & 0x1F))) == 0) {
			ppEnableQueue(q, p1, p2, p3, ctx);
			return q;
		}
	} else {
		while ((q = 31 - lzcount((~mask + 1) & mask)) != 0xFFFFFFFFu) {
			uint8_t *qb = QB(q);

			if (qb[8] == (uint8_t)p3 && qb[9] == (uint8_t)p2 &&
			    qb[10] == (uint8_t)p1)
				return q & 0xFF;
			mask &= ~(1u << (q & 0x1F));
		}
	}
	u6 = (word0 | 3) ^ 0xFF;
	u4 = ((1u << (TXRX[ctx + 0x14] & 0x1F)) - 1) & u6;
	u5 = 31 - lzcount(u4);
	if (u5 == 0xFFFFFFFFu) {
		u5 = 31 - lzcount(u6 ^ u4);
		if (u5 == 0xFFFFFFFFu)
			return 8;
	}
	u4 = ((1u << (u5 & 0x1F)) - 1) & word0;
	i3 = 31 - lzcount(u4);
	if (i3 == -1) {
		u5 = (u5 + 0x25 - lzcount(word0 ^ u4)) >> 1;
		if ((int32_t)u5 > 7)
			u5 -= 6;
	} else {
		u5 = (u5 + 1 + i3) >> 1;
	}
	ppEnableQueue(u5, p1, p2, p3, ctx);
	return u5 & 0xFF;
}

static PpNode *
ppGetTxframe(int32_t q)
{
	uint8_t *qb = QB(q);
	PpNode *node;

	GetAccess(ac_from_state(qb[8]));
	node = *(PpNode **)qb;
	if (node != NULL) {
		vPortEnterCritical();
		*(PpNode **)qb = node->next;
		if (node->next == NULL)
			*(PpNode ***)(qb + 4) = (PpNode **)qb;
		vPortExitCritical();
		node->next = NULL;
		lmacAdjustTimestamp(node);
	}
	return node;
}

uint32_t
ppCheckTxIdle(uint32_t lockflag)
{
		uint32_t ret;
	int32_t i;

	lockflag &= 0xFF;
	if (lockflag != 0)
		vPortETSIntrLock();
	for (i = 0; i < 4; i++) {
		if (lmacIsIdle(i) == 0)
			goto notidle;
	}
	for (;;) {
		uint32_t word0 = *(uint32_t *)TXRX;
		int32_t k = 31 - lzcount((~word0 + 1) & word0);

		if (k == -1)
			break;
		if (QHEAD(k) != NULL)
			goto notidle;
		ppDisableQueue(k, ac_from_state(QB(k)[8]));
	}
	if (QHEAD(0) == NULL && QHEAD(1) == NULL &&
	    *(void **)(TXRX + 0x98) == NULL &&
	    *(void **)(TXRX + 0xa0) == NULL)
		ret = 0;
	else
		ret = 1;
	goto out;
notidle:
	ret = 1;
out:
	if (lockflag != 0)
		vPortETSIntrUnlock();
	return ret;
}

void
ppCalFrameTimes(PpNode *node)
{
		uint32_t *ctrl = node->ctrl;
	uint32_t ack = 0;

	if ((ctrl[0] & 0x402) == 0)
		ack = RC_GetAckTime((int8_t)ctrl[3]) & 0x1FF;
	ctrl[2] = (ctrl[2] & 0x007FFFFFu) | (ack << 0x17);
	*(uint16_t *)((uint8_t *)ctrl + 0xE) =
		RC_GetCtsTime(*(uint8_t *)((uint8_t *)ctrl + 0xC),
			      node->len + node->len18, ctrl[2] >> 0x17);
}

static void process_waitq(void);

uint32_t
ppCalTxop(uint32_t q, uint32_t acc, uint32_t limit)
{
		PpNode *node;

	q &= 0xFF;
	acc &= 0xFFFF;
	limit &= 0xFFFF;
	if (QB(q)[11] == 0) {
		vPortETSIntrLock();
		for (node = QHEAD(q); node != NULL; node = node->next) {
			acc = (acc + *(uint16_t *)((uint8_t *)node->ctrl + 0xE)) & 0xFFFF;
			if (limit <= acc) {
				vPortETSIntrUnlock();
				return limit;
			}
		}
		vPortETSIntrUnlock();
	}
	return acc;
}

static void
noise_body(void)
{
	uint32_t rx = RxNodeNum();
	uint32_t tx = TxNodeNum();

	if ((rx & 0xFF) < 3 || tx < 2) {
		uint8_t c = node_check_tick;

		node_check_tick = c + 1;
		if ((int)(1000 / NoiseTimerInterval) <=
		    (int)(uint8_t)(c + 1)) {
			node_check_tick = 0;
			if ((rx & 0xFF) < 3)
				esp_log_write(1, "wifi", "LmacRxBlk:%d",
					      rx & 0xFF);
			if (tx < 2)
				esp_log_write(1, "wifi", "LmacTxBlk:%d", tx);
		}
	} else {
		node_check_tick = 0;
	}
	{
		int nf = read_hw_noisefloor();
		uint8_t c = period_check_ac_tick;

		noise_now = (uint8_t)((nf + 2) >> 2);
		period_check_ac_tick = c + 1;
		if ((int)(10000 / NoiseTimerInterval) <
		    (int)(uint8_t)(c + 1)) {
			period_check_ac_tick = 0;
			lmac_check_ac();
		}
	}
}

void
pp_noise_test(void)
{
	__wifi_task_suspend_all();
	os_timer_disarm(&noise_test_timer);
	if (pm_rf_is_closed() == 0)
		noise_check_loop(1);
	else
		pend_flag_noise_check = 1;
	noise_body();
	os_timer_arm(&noise_test_timer, NoiseTimerInterval, 0);
	__wifi_task_resume_all();
	}

void
pp_disable_noise_timer(void)
{
	os_timer_disarm(&noise_test_timer);
}

void
reset_noise_timer(uint32_t ms)
{
	ms &= 0xFFFF;
	node_check_tick = 0;
	os_timer_disarm(&noise_test_timer);
	NoiseTimerInterval = (uint16_t)ms;
	os_timer_arm(&noise_test_timer, ms, 0);
}

void
pp_attach(void)
{
	uint8_t *base = TXRX;
	uint32_t q;

	
	for (q = 0; q < 8; q++) {
		uint8_t *qb = QB(q);

		*(PpNode **)qb = NULL;
		*(PpNode ***)(qb + 4) = (PpNode **)qb;
		qb[11] = 0;
	}
	base[32] = 7;
	base[48] = 7;
	base[20] = 2;
	base[21] = 3;
	base[22] = 5;
	base[23] = 6;
	*(void **)(base + 156) = base + 152;
	*(void **)(base + 164) = base + 160;
	*(void **)(base + 172) = base + 168;
	*(void **)(base + 180) = base + 176;
	*(uint32_t *)base = 0;
	base[35] = 1;
	base[51] = 1;
	*(uint32_t *)(base + 4) = 0;
	*(uint32_t *)(base + 8) = 0;
	*(uint32_t *)(base + 12) = 0;
	*(uint32_t *)(base + 16) = 0;
	*(void **)(base + 152) = NULL;
	*(void **)(base + 160) = NULL;
	*(void **)(base + 168) = NULL;
	*(void **)(base + 176) = NULL;
	MEMW();
#ifndef PP_HOST_TEST
	*(volatile uint32_t *)0x600011F8u = 0x000A0700u;
#endif
	esf_buf_setup();
	xphyQueue = __wifi_queue_create(0x4B, 8);
	{
		int prio = __wifi_task_get_max_priority();

		__wifi_task_create((void *)ppTask, "ppT",
				   _g_esp_wifi_ppt_task_stk_size, 0, prio - 2);
	}
	__wifi_task_get_max_priority();
	os_timer_setfn(&noise_test_timer, (void *)pp_noise_test, NULL);
	__wifi_task_suspend_all();
	os_timer_disarm(&noise_test_timer);
	if (pm_rf_is_closed() == 0)
		noise_check_loop(1);
	else
		pend_flag_noise_check = 1;
	noise_body();
	os_timer_arm(&noise_test_timer, NoiseTimerInterval, 0);
	__wifi_task_resume_all();
}

int32_t
pp_post(uint32_t type, uint32_t arg)
{
	vPortETSIntrLock();
	if (pp_sig_cnt[type] == 0) {
		if (type != 9)
			pp_sig_cnt[type] = 1;
		vPortETSIntrUnlock();
		if (type < 4) {
			if (__wifi_queue_msg_num(xphyQueue) < 0x4B) {
				uint32_t msg[2];

				msg[0] = type;
				msg[1] = arg;
				if (__wifi_queue_send(xphyQueue, msg, 0, 1) == 1)
					return 0;
				pp_sig_cnt[type] = 0;
			}
		} else {
			uint32_t w = s_pp_queue_wptr;
			uint32_t nw = w + 1;

			if (nw > 0x1F)
				nw = 0;
			vPortETSIntrLock();
			if (s_pp_queue_rptr != nw) {
				s_pp_queue_wptr = nw;
				s_pp_queue[w][1] = arg;
				s_pp_queue[w][0] = type;
				vPortETSIntrUnlock();
				PendSV(2);
				return 0;
			}
			pp_sig_cnt[type] = 0;
			vPortETSIntrUnlock();
		}
		return 1;
	}
	vPortETSIntrUnlock();
	return 0;
}

int32_t
pp_post_from_isr(uint32_t type, uint32_t arg)
{
		uint32_t msg[2];
	int woken = 0;
	int32_t r;

	msg[0] = type;
	msg[1] = arg;
	r = __wifi_queue_send_from_isr(xphyQueue, msg, &woken);
	if (r == 1 && woken == 1)
		__wifi_task_yield_from_isr();
	return r;
}

int
MacIsrSigPostDefHdl(void)
{
		int ret = 0;

	while (s_pp_queue_rptr != s_pp_queue_wptr) {
		uint32_t msg[2];
		int woken = 0;

		if (__wifi_queue_msg_num(xphyQueue) > 0x4A)
			return ret;
		if (s_pp_queue_rptr == s_pp_queue_wptr)
			return ret;
		msg[0] = s_pp_queue[s_pp_queue_rptr][0];
		msg[1] = s_pp_queue[s_pp_queue_rptr][1];
		if (__wifi_queue_send_from_isr(xphyQueue, msg, &woken) != 1)
			break;
		if (s_pp_queue_rptr != s_pp_queue_wptr) {
			uint32_t n = s_pp_queue_rptr + 1;

			if (n > 0x1F)
				n = 0;
			s_pp_queue_rptr = n;
		}
		if (woken == 1)
			ret = 1;
	}
	return ret;
}

static void
process_waitq(void)
{
	uint32_t idx = current_ifidx;
	uint32_t found = 0xFFFFFFFFu;

	do {
		uint32_t word0;
		uint8_t *wq;
		PpNode *node;

		vPortETSIntrLock();
		word0 = *(uint32_t *)TXRX;
		vPortETSIntrUnlock();
		if ((word0 | 3) != 0xFF) {
			wq = TXRX + idx * 8 + 0x98;
			node = *(PpNode **)wq;
			if (node != NULL) {
				uint32_t i5 = idx + 0x12;
				PpNode *next;

				*(PpNode **)wq = NULL;
				*(PpNode ***)(wq + 4) = (PpNode **)wq;
				do {
					uint32_t res;

					next = node->next;

					node->next = NULL;
					if (node->trc == NULL) {
						res = (node->ctrl[1] >> 1) & 1;
						goto enqueue;
					}
					vPortETSIntrLock();
					res = ppSearchTxQueue(
						(node->ctrl[1] >> 1) & 1,
						*(uint8_t *)((uint8_t *)node->trc + 0x44),
						node->ctrl[2] & 0xF,
						(node->ctrl[2] >> 0x14) & 7);
					vPortETSIntrUnlock();
					if (res != 8)
						goto enqueue;
					vPortEnterCritical();
					node->next = NULL;
					**(PpNode ***)(TXRX + i5 * 8 + 0xC) = node;
					*(PpNode ***)(TXRX + i5 * 8 + 0xC) = &node->next;
					vPortExitCritical();
					goto nextnode;
				enqueue:
					node->ctrl[1] = (node->ctrl[1] & 0xFFFFFFC3u) |
							((res & 0xF) << 2);
					vPortETSIntrLock();
					vPortEnterCritical();
					node->next = NULL;
					q_push(QB((node->ctrl[1] >> 2) & 0xF), node);
					vPortExitCritical();
					vPortETSIntrUnlock();
					pp_post((node->ctrl[2] >> 0x14) & 7, 0);
				nextnode:
					node = next;
					found = idx;
				} while (next != NULL);
			}
		}
		idx = (idx + 1) & 0xFF;
		if (idx == 2)
			idx = 0;
		if (current_ifidx == (uint8_t)idx) {
			if (found != 0xFFFFFFFFu) {
				if (((found + 1) & 0xFF) == 2)
					current_ifidx = 0;
				else
					current_ifidx = (uint8_t)(found + 1);
			}
			return;
		}
	} while (1);
}

void
ppProcessWaitQ(void)
{
		process_waitq();
}

int32_t
ppProcessTxQ(uint32_t ctx)
{
		uint32_t mask, cur, below, rest, k;
	PpNode *node;

	ctx &= 0xFF;
	vPortETSIntrLock();
	if (lmacIsIdle(ctx) == 0) {
		vPortETSIntrUnlock();
		return -1;
	}
	mask = *(uint32_t *)(TXRX + ctx * 4 + 4);
	cur = TXRX[ctx + 0x14];
	if ((mask & (1u << (cur & 0x1F))) != 0 && QB(cur)[12] != 0) {
		QB(cur)[12] = 0;
		if (QHEAD(cur) == NULL)
			ppDisableQueue(cur, ctx);
		mask = *(uint32_t *)(TXRX + ctx * 4 + 4);
		if (mask != 0) {
			uint32_t above = (mask ^ (((1u << (cur & 0x1F)) - 1) & mask)) &
					 ~(1u << (cur & 0x1F));
			int32_t nxt = 31 - lzcount((~above + 1) & above);

			if (nxt == -1) {
				uint32_t b = ((1u << (cur & 0x1F)) - 1) & mask;

				nxt = 31 - lzcount((~b + 1) & b);
			}
			if (nxt != -1)
				TXRX[ctx + 0x14] = (uint8_t)nxt;
		}
	}
	cur = TXRX[ctx + 0x14];
	node = NULL;
	if (ctx == 0 &&
	    ((*(uint32_t *)(TXRX + 4) & (1u << (cur & 0x1F))) == 0 ||
	     QB(cur)[12] == 0)) {
		node = ppGetTxframe(0);
		if (node == NULL)
			node = ppGetTxframe(1);
		if (node != NULL)
			goto send;
	}
	mask = *(uint32_t *)(TXRX + ctx * 4 + 4);
	below = ((1u << (cur & 0x1F)) - 1) & mask;
	rest = mask ^ below;
	for (;;) {
		int32_t kk = 31 - lzcount((~rest + 1) & rest);

		if (kk == -1)
			break;
		k = (uint32_t)kk;
		node = ppGetTxframe((int32_t)k);
		if (node != NULL) {
			TXRX[ctx + 0x14] = (uint8_t)k;
			QB(k)[12] = 1;
			goto send;
		}
		rest &= ~(1u << (k & 0x1F));
	}
	for (;;) {
		int32_t kk = 31 - lzcount((~below + 1) & below);

		if (kk == -1) {
			vPortETSIntrUnlock();
			return -2;
		}
		k = (uint32_t)kk;
		node = ppGetTxframe((int32_t)k);
		if (node == NULL) {
			below &= ~(1u << (k & 0x1F));
			continue;
		}
		TXRX[ctx + 0x14] = (uint8_t)k;
		QB(k)[12] = 1;
		goto send;
	}
send:
	if ((node->ctrl[0] & 0x800000) != 0) {
		*(int32_t *)(TXRX + 0xEC) -= 1;
		*(int32_t *)(TXRX + 0xE8) += 1;
	}
	vPortETSIntrUnlock();
	lmacTxFrame(node, ctx);
	return 0;
}

int32_t
ppTxPkt(PpNode *node)
{
	{ extern uint32_t lmac_dbg[16]; lmac_dbg[10]++; }
		static const uint8_t lensw[3] = { 8, 16, 12 };
	uint32_t *ctrl = node->ctrl;
	uint32_t w2 = ctrl[2];
	uint32_t ac = ac_from_state(w2 & 0xF);
	uint8_t *fc;
	uint32_t add;
	uint32_t idx;
	uint32_t *desc;

	if (ac != ((w2 >> 0x14) & 7)) {
		ets_printf("%s %u\n", "pp", 0x1FA);
		for (;;)
			;
	}
	if ((app_optimize_and_debug_mac & 0x8000000) != 0 &&
	    ((app_optimize_and_debug_mac & 0x04000000) != 0 ||
	     ((*(char *)node->frame != 'P' &&
	       *(char *)node->frame != (char)0x80)))) {
		uint8_t *frame = (uint8_t *)node->frame;
		uint32_t i = 0;

		printf("@ T:%x, P:%x\n", node->len, frame[1]);
		while ((int32_t)i < (int32_t)(uint32_t)node->len &&
		       (((app_optimize_and_debug_mac >> 0x1D) & 1) != 1 ||
			i < 0x60)) {
			printf("%02x ", frame[i]);
			i++;
			if ((i & 0xF) == 0)
				printf("\n");
		}
		if ((i & 0xF) != 0)
			printf("\n");
	}
	fc = node->hdr;
	if ((fc[4] & 1) != 0)
		ctrl[0] |= 2;
	{
		uint32_t fc0 = fc[0];

		if ((fc0 & 0xC) == 8) {
			uint32_t v = ctrl[0];

			ctrl[0] = v | 8;
			v &= ~8u;
			if ((fc0 & 0x70) == 0x40)
				ctrl[0] = v;
		} else if ((fc0 & 0xC) == 0) {
			uint32_t t = fc0 & 0xF0;

			if (t == 0x80) {
				if ((ctrl[0] & 0x800000) != 0)
					*(int32_t *)(TXRX + 0xEC) += 1;
			} else {
				uint32_t bits;

				if (t == 0x40)
					bits = 0x00000000u;
				else if (t == 0x50)
					bits = 0x08000000u;
				else
					goto lendone;
				if ((ctrl[0] & 2) == 0)
					ctrl[0] = ctrl[0] | bits;
			}
		}
	}
lendone:
	add = 4;
	idx = (((ctrl[1] >> 0xE) & 0xF) - 1) & 0xFF;
	if (idx < 3)
		add = lensw[idx];
	desc = (uint32_t *)node->desc;
	node->len = (uint16_t)(node->len + (int16_t)add);
	MEMW();
	MEMW();
	MEMW();
	*desc = (*desc & 0xFF000FFFu) | ((((*desc >> 0xC) & 0xFFF) + add) & 0xFFF) << 0xC;
	MEMW();
	MEMW();
	*desc |= 0x40000000u;
	vPortETSIntrLock();
	rcGetSched(node->trc, ctrl);
	vPortETSIntrUnlock();
	ppCalFrameTimes(node);
	ctrl[5] = WDEV_COUNT;
	process_waitq();
	if (node->trc == NULL) {
		if ((ctrl[0] & 8) != 0 ||
		    ((ctrl[4] - 0x20) & 0xFFFFFFDFu) != 0) {
			if (pm_allow_tx(1) == 0)
				goto waitqueue;
		}
		goto setctrl;
	}
	if ((fc[0] & 0xC) == 0) {
		if (pm_allow_tx(1) == 0)
			goto waitqueue;
		goto setctrl;
	}
	{
		int allow = pm_allow_tx(1);
		uint32_t q;

		if (allow == 0 && (ctrl[0] & 0x2000) != 0)
			goto waitqueue;
		vPortETSIntrLock();
		q = ppSearchTxQueue((ctrl[1] >> 1) & 1,
				    *(uint8_t *)((uint8_t *)node->trc + 0x44),
				    ctrl[2] & 0xF, (ctrl[2] >> 0x14) & 7);
		vPortETSIntrUnlock();
		if (q == 8 || pm_allow_tx(1) == 0)
			goto waitqueue;
		ctrl[1] = (ctrl[1] & 0xFFFFFFC3u) | ((q & 0xF) << 2);
		goto enqueue;
	}
setctrl:
	ctrl[1] = (ctrl[1] & 0xFFFFFFC3u) | ((ctrl[1] >> 1) & 1) << 2;
	ctrl[2] = (ctrl[2] & 0xFFFF8FF0u) | 7;
enqueue:
	vPortETSIntrLock();
	vPortEnterCritical();
	node->next = NULL;
	q_push(QB((ctrl[1] >> 2) & 0xF), node);
	vPortExitCritical();
	if (node->trc != NULL)
		ppEnableQueue((ctrl[1] >> 2) & 0xF, (ctrl[1] >> 1) & 1,
			      *(uint8_t *)((uint8_t *)node->trc + 0x44),
			      ctrl[2] & 0xF, (ctrl[2] >> 0x14) & 7);
	vPortETSIntrUnlock();
	return pp_post((ctrl[2] >> 0x14) & 7, 0);
waitqueue:
	vPortEnterCritical();
	node->next = NULL;
	q_push(TXRX + ((ctrl[1] >> 1) & 1) * 8 + 0x98, node);
	vPortExitCritical();
	return 0;
}

static PpNode *
pop_locked(uint32_t off)
{
	PpNode *node;

	vPortETSIntrLock();
	node = *(PpNode **)(TXRX + off);
	if (node != NULL) {
		vPortEnterCritical();
		*(PpNode **)(TXRX + off) = node->next;
		if (node->next == NULL)
			*(PpNode ***)(TXRX + off + 4) = (PpNode **)(TXRX + off);
		vPortExitCritical();
		node->next = NULL;
	}
	vPortETSIntrUnlock();
	return node;
}

static uint32_t
rx_len(uint32_t *info)
{
	uint32_t n = (info[0] >> 0x10) & 0xFFF;

	if ((info[0] & 0xC000) != 0)
		n = (info[1] >> 8) & 0xFFFF;
	return n;
}

void
ppTask(void)
{
	uint32_t msg[2];
	uint32_t type, arg;
	PpNode *node;
	int32_t keepalive;
	uint32_t cls;

		for (;;) {
		while (__wifi_queue_recv(xphyQueue, msg, 0xFFFFFFFFu) != 1)
			;
		type = msg[0];
		arg = msg[1];
		if (type == 8) {
			vPortETSIntrLock();
			pp_sig_cnt[8] = 0;
			vPortETSIntrUnlock();
			process_waitq();
			continue;
		}
		if (type >= 9) {
			if (type == 11) {
				vPortETSIntrLock();
				pp_sig_cnt[11] = 0;
				vPortETSIntrUnlock();
				if (GICW(0x148) != 0) {
					void *cb = *(void **)(GICW(0x148) + 4);

					if (cb != NULL)
						((void (*)(void))cb)();
				}
			} else if (type < 12) {
				if (type == 9) {
					ppProcessRxPktHdr((void *)(uintptr_t)arg);
				} else if (type == 10) {
					vPortETSIntrLock();
					pp_sig_cnt[10] = 0;
					vPortETSIntrUnlock();
				}
			} else if (type == 12) {
				vPortETSIntrLock();
				pp_sig_cnt[12] = 0;
				vPortETSIntrUnlock();
				lmacProcessTxTimeout();
			} else if (type == 16) {
				ets_timer_proc();
			}
			continue;
		}
		if (type == 4)
			goto txdone;
		if (type < 4) {
			vPortETSIntrLock();
			pp_sig_cnt[type] = 0;
			vPortETSIntrUnlock();
			ppProcessTxQ(type & 0xFF);
			continue;
		}
		if (type == 5) {
			vPortETSIntrLock();
			pp_sig_cnt[5] = 0;
			vPortETSIntrUnlock();
			goto rxdrain;
		}
		continue;
	txdone:
		vPortETSIntrLock();
		keepalive = 0;
		pp_sig_cnt[4] = 0;
		cls = 0;
		vPortETSIntrUnlock();
		for (;;) {
			node = pop_locked(0xA8);
			if (node == NULL) {
				if (keepalive != 0)
					pm_reset_keepalive_time();
				ppCheckTxIdle(1);
				break;
			}
			{
				uint32_t *ctrl = node->ctrl;
				uint32_t mask = ctrl[4];

				while (mask != 0) {
					int32_t k = 31 - lzcount((~mask + 1) & mask);

					if ((*(uint32_t *)(TXRX + 0xB8) &
					     (1u << (k & 0x1F))) != 0)
						(*(void (**)(PpNode *))(TXRX + 0xBC + k * 4))(node);
					mask &= ~(1u << (k & 0x1F));
				}
				{
					uint32_t thresh = GICB(0x141) == 2 ? 1 : 5;
					uint32_t v = ((ctrl[2] >> 4) & 0xF) + need_pwctrl;

					if ((v & 0xFF) < thresh)
						need_pwctrl = (uint8_t)v;
					else {
						need_pwctrl = 0;
						tx_pwctrl_background(1, 0);
					}
				}
				if (((ctrl[0] & 8) != 0 ||
				     ((ctrl[4] - 0x20) & 0xFFFFFFDFu) == 0) &&
				    ((ctrl[1] & 0x03FC0002u) == 0x00040000u))
					keepalive = 1;
				if ((ctrl[0] & 0x2000) == 0) {
					if ((ctrl[0] & 0x4000) == 0) {
						if ((ctrl[0] & 0x8000) != 0)
							cls = 5;
					} else {
						cls = 4;
					}
				} else {
					cls = 1;
				}
				if ((ctrl[0] & 0x800000) == 0) {
					esf_buf_recycle(node, cls);
					continue;
				}
				{
					uint32_t *desc = (uint32_t *)node->desc;

					node->len = (uint16_t)(node->len - 4);
					MEMW();
					MEMW();
					MEMW();
					*desc = (*desc & 0xFF000FFFu) |
						((((*desc >> 0xC) & 0xFFF) - 4) & 0xFFF) << 0xC;
				}
				if (PendFreeBcnEb != 0) {
					PendFreeBcnEb = 0;
					esf_buf_recycle(node, cls);
				}
			}
		}
		continue;
	rxdrain:
		for (;;) {
			uint8_t *frame;
			uint32_t *info;
			uint32_t fctype, subtype, ifidx;

			node = pop_locked(0xB0);
			if (node == NULL)
				break;
			frame = (uint8_t *)node->frame;
			info = (uint32_t *)node->info;
			fctype = frame[0] & 0xC;
			subtype = frame[0] & 0xF0;
			if ((int8_t)noise_now > 0)
				noise_now = (uint8_t)((read_hw_noisefloor() + 2) >> 2);
			{
				uint32_t w = info[0];

				if ((w & 0x50000000u) == 0)
					ifidx = (w & 0xA0000000u) != 0 ? 1 : 2;
				else
					ifidx = (w & 0xA0000000u) != 0 ? 2 : 0;
			}
			if (fctype == 8) {
				if (esp_wifi_get_pm_recv_multi_data() != 0 ||
				    (frame[4] & 1) == 0) {
					if ((frame[1] & 0x20) == 0)
						pm_recv_all_data();
					else
						pm_have_more_data_to_recv();
				}
			} else if (fctype == 0) {
				uint32_t hdrlen = ieee80211_hdrsize(frame);
				uint32_t len = rx_len(info);

				if (subtype == 0x80) {
					if (pm_sleep_for(frame + 10) != 0)
						pm_onBcnRx(node, frame + 0x18,
							   frame + 0x18 +
							   ((len - 4 - hdrlen) & 0xFFFF));
				} else if (subtype == 0x40 && GICW(8) != 0 &&
					   wifi_sta_rx_probe_req != NULL) {
					len = rx_len(info);
					((void (*)(void *, uint32_t, int))wifi_sta_rx_probe_req)
						(node->frame, len - 4,
						 (int)(int8_t)info[0] - 0x60);
				}
			}
			if (ifidx != 2) {
				node->trc = rc_get_trc(ifidx, frame + 10);
				rcUpdateRxDone(node->trc, info);
				*(uint8_t *)info = (uint8_t)(*(uint8_t *)info + 3);
				if (ic_is_pure_sta(ifidx) != 0 && fctype == 0) {
					uint32_t hdrlen = ieee80211_hdrsize(frame);
					uint32_t len = rx_len(info);

					if ((subtype == 0x10 || subtype == 0x30) &&
					    pm_add_aid(frame + 0x18,
						       frame + 0x18 +
						       ((len - hdrlen - 4) & 0xFFFF)) != 0)
						pm_set_addr(ifidx, frame + 10);
				}
				if ((frame[1] & 0x40) != 0) {
					if ((frame[4] & 1) == 0)
						ic_get_ptk_alg(ifidx);
					else
						ic_get_gtk_alg(ifidx);
				}
			}
			if (fragment.node != NULL &&
			    (uint32_t)(MIBW(8) << 10) <
			    (uint32_t)(WDEV_COUNT - fragment.node->ctrl[1])) {
				ppRecycleRxPkt(fragment.node);
				fragment.node = NULL;
				fragment.len = 0;
			}
			frame = (uint8_t *)node->frame;
			if ((frame[0] & 4) == 0) {
				uint32_t seq = *(uint16_t *)(frame + 0x16);
				uint32_t morefrag = frame[1] & 4;
				uint32_t fragno = seq & 0xF;
				uint32_t hdrlen, paylen;

				if (morefrag == 0 && fragno == 0)
					goto deliver;
				hdrlen = ieee80211_hdrsize(frame) & 0xFFFF;
				if ((frame[1] & 0x40) == 0) {
					paylen = (rx_len(info) - hdrlen) - 4;
				} else if ((frame[hdrlen + 3] & 0x20) == 0) {
					hdrlen = (hdrlen + 4) & 0xFFFF;
					paylen = (rx_len(info) - hdrlen) - 8;
				} else {
					uint32_t algarg = 0;
					int alg;

					if (node->trc != NULL)
						algarg = *(uint8_t *)((uint8_t *)node->trc + 0x45);
					alg = ic_get_ptk_alg(algarg);
					hdrlen = (hdrlen + 8) & 0xFFFF;
					if (alg != 2) {
						paylen = (rx_len(info) - hdrlen) - 0xC;
						goto have_len;
					}
					paylen = (rx_len(info) - hdrlen) - 8;
					goto have_len;
				}
				goto have_len2;
			have_len:
				paylen = paylen;
			have_len2:
				if (fragment.node == NULL) {
					if (fragno != 0) {
						ppRecycleRxPkt(node);
						continue;
					}
					fragment.node = node;
					fragment.len = (uint16_t)((uint16_t)hdrlen +
								   (uint16_t)paylen);
					continue;
				}
				{
					PpNode *fnode = fragment.node;
					uint8_t *fframe = (uint8_t *)fnode->frame;
					uint8_t tid_new = 0x10, tid_old = 0x10;

					if ((frame[0] & 0x8C) == 0x88) {
						tid_new = (uint8_t)(((frame[1] & 3) == 3 ?
							frame[0x1E] : frame[0x18]) & 0xF);
					}
					if ((fframe[0] & 0x8C) == 0x88) {
						tid_old = (uint8_t)(((fframe[1] & 3) == 3 ?
							fframe[0x1E] : fframe[0x18]) & 0xF);
					}
					if (tid_old != tid_new ||
					    memcmp(frame + 4, fframe + 4, 0xC) != 0) {
						ppRecycleRxPkt(node);
						continue;
					}
					if (((seq ^ *(uint16_t *)(fframe + 0x16)) & 0xFFFFFFF0u) == 0) {
						uint32_t expect = *(uint16_t *)(fframe + 0x16) + 1;

						if (seq == expect) {
							uint32_t total = fragment.len +
									 (paylen & 0xFFFF);

							if ((int32_t)total <
							    *(int32_t *)((uint8_t *)&g_wifi_menuconfig + 0xC) - 0xC) {
								uint32_t *finfo = (uint32_t *)fnode->info;

								memcpy(fframe + fragment.len,
								       frame + hdrlen,
								       paylen & 0xFFFF);
								finfo[1] = (finfo[1] & 0xFF0000FFu) |
									(((paylen & 0xFFFF) +
									  ((finfo[1] >> 8) & 0xFFFF)) & 0xFFFF) << 8;
								finfo[0] = (finfo[0] & 0xF000FFFFu) |
									(((paylen & 0xFFFF) +
									  ((finfo[0] >> 0x10) & 0xFFF)) & 0xFFF) << 0x10;
								*(uint16_t *)(fframe + 0x16) = (uint16_t)seq;
								fragment.len = (uint16_t)total;
							}
							ppRecycleRxPkt(node);
							if (morefrag == 0) {
								PpNode *done = fnode;

								*(uint16_t *)(fframe + 0x16) =
									(uint16_t)seq & 0xFFF0;
								fframe[1] &= 0xFB;
								fragment.node = NULL;
								fragment.len = 0;
								node = done;
								frame = fframe;
								goto deliver;
							}
							continue;
						}
						if ((int32_t)seq <= (int32_t)expect) {
							ppRecycleRxPkt(node);
							continue;
						}
					} else if (fragno == 0) {
						ppRecycleRxPkt(fnode);
						fragment.node = node;
						fragment.len = (uint16_t)((uint16_t)hdrlen +
									   (uint16_t)paylen);
						continue;
					}
					ppRecycleRxPkt(node);
					if (fragment.node != NULL)
						ppRecycleRxPkt(fragment.node);
					fragment.node = NULL;
					fragment.len = 0;
					continue;
				}
			}
		deliver:
			{
				uint8_t *dframe = frame;
				uint32_t *dinfo = info;

				if ((int8_t)((uint8_t *)dinfo)[8] == (int8_t)0xF5) {
					if (GICW(0x144) != 0) {
						void *cb = *(void **)(GICW(0x144) + 0x3C);

						if (cb != NULL)
							((void (*)(uint32_t))cb)(
								(dinfo[0] >> 0xC & 1) ^ 1);
					}
				} else {
					uint32_t w = dinfo[0];

					if (((w & 0x50000000u) == 0 ||
					     (int32_t)w < 0) && GICB(0x141) != 1) {
						if ((w & 0xA0000000u) == 0) {
							if (GICB(0x160) != 0) {
								uint32_t ic = GICW(8);
								uint32_t hdrlen2;
								uint32_t len2;

								if (ic == 0)
									ic = GICW(0xC);
								hdrlen2 = ieee80211_hdrsize(dframe);
								len2 = rx_len(dinfo);
								if (ic != 0)
									ieee80211_recv_action(
										ic, dframe,
										dframe + 24,
										dframe + 24 +
										(len2 - 4 - hdrlen2));
							}
						} else if (TmpSTAAPCloseAP != 1) {
							hostap_input(GICW(0xC), node,
								     (int)*(int8_t *)dinfo, 0);
							continue;
						}
					} else if ((int8_t)((uint8_t *)dinfo)[8] == 0 ||
						   (int8_t)((uint8_t *)dinfo)[8] == '2') {
						sta_input(GICW(8), node,
							  (int)*(int8_t *)dinfo, 0);
						continue;
					}
				}
				ppRecycleRxPkt(node);
				continue;
			}
		}
		continue;
	}
}
