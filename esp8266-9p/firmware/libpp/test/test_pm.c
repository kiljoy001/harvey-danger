/*
 * test_pm.c - Host tests for the clean-room pm.c.
 *
 * Build:
 *   cc -m32 -std=c99 -Wall -Wextra -Werror -DPM_HOST_TEST -I. \
 *      -o /tmp/test_pm pm.c test/test_pm.c
 *
 * Expected values are hand-computed from the blob decompiles in
 * re/inventory/b38/, not from pm.c itself.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "pm.h"

/* ------------------------------------------------------------------ */
/* Stub world                                                          */

char g_ic[0x480];
static char nvs_buf[64];
static char sniffer_buf[4];
char *g_wifi_nvs = nvs_buf;
char *sniffer_enable = sniffer_buf;
uint64_t WdevTimOffSet;
uint8_t pend_flag_noise_check;
extern uint32_t app_optimize_and_debug_mac;

static int suspend_depth;
static int nvs_listen_interval = 3;
static uint32_t host_wdev_count;
static uint32_t host_now32;
static uint64_t host_timer_us;
static uint32_t txidle_ret;
static int trc_null = 1;

/* timer bookkeeping */
static void *setfn_timer[8];
static void *setfn_fn[8];
static int nsetfn;
static void *arm_timer;
static uint32_t arm_ms;
static int narm, narm_us, ndisarm;
static uint32_t arm_us_val;
static void *regcb_fn[8];
static uint32_t regcb_idx[8];
static int nregcb;
static int nprocesswaitq, ntxpkt, nsendsetup, ngetmgtframe;

/* fake conn / bss behind g_ic offsets */
static uint8_t conn_buf[0x100];
static uint8_t bss_buf[0x200];
static uint8_t ps_buf[0x20];

/* fake nullfunc frame */
static uint32_t fake_hdr[4];
static uint32_t fake_desc[16];
static uint8_t fake_buf[64];
static uint32_t fake_frame[16];
static int mgtframe_null;

void __wifi_task_suspend_all(void) { suspend_depth++; }
void __wifi_task_resume_all(void) { suspend_depth--; }
void clockgate_watchdog(int on) { (void)on; }
void *ic_get_addr(int idx) { (void)idx; return (void *)0x1000; }
uint32_t esp_log_early_timestamp(void) { return 42; }
uint64_t esp_timer_get_time(void) { return host_timer_us; }
void esp_log_write(int level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; }
void esp_sleep_lock(void) {}
void esp_sleep_unlock(void) {}
int ets_printf(const char *fmt, ...) { (void)fmt; return 0; }

void *
ieee80211_getmgtframe(int *len, int size, int arg3)
{
	(void)len; (void)size; (void)arg3;
	ngetmgtframe++;
	if (mgtframe_null)
		return NULL;
	memset(fake_frame, 0, sizeof(fake_frame));
	fake_frame[0] = (uint32_t)(uintptr_t)fake_hdr;
	fake_frame[3] = (uint32_t)(uintptr_t)fake_buf;
	fake_frame[10] = (uint32_t)(uintptr_t)fake_desc;
	return fake_frame;
}

void
ieee80211_send_setup(void *conn, void *frame, int len, int waitmask,
		     void *addr, void *dst, void *bssid)
{
	(void)conn; (void)frame; (void)len; (void)waitmask;
	(void)addr; (void)dst; (void)bssid;
	nsendsetup++;
}

void noise_check_loop(int on, int second) { (void)on; (void)second; }

void
os_timer_arm(void *timer, uint32_t ms, int repeat)
{
	(void)repeat;
	arm_timer = timer;
	arm_ms = ms;
	narm++;
}

void
os_timer_arm_us(void *timer, uint32_t us, int repeat)
{
	(void)timer; (void)repeat;
	arm_us_val = us;
	narm_us++;
}

void os_timer_disarm(void *timer) { (void)timer; ndisarm++; }

void
os_timer_setfn(void *timer, void (*fn)(void *), void *arg)
{
	(void)arg;
	setfn_timer[nsetfn] = timer;
	setfn_fn[nsetfn] = (void *)fn;
	nsetfn++;
}

void phy_close_rf(void) {}
void phy_open_rf(void) {}
void pm_set_sleep_cycles(uint32_t cycles) { (void)cycles; }
uint32_t pm_usec2rtc(uint32_t usec, uint32_t period)
{ (void)period; return usec * 3; }
uint64_t pm_usec2rtc_64(uint64_t usec, uint32_t period)
{ (void)period; return usec * 3; }
void ppTxPkt(void *desc) { (void)desc; ntxpkt++; }
void *rc_get_trc(uint32_t ifidx, const void *mac)
{ (void)ifidx; (void)mac; return trc_null ? NULL : (void *)0x2000; }
uint32_t ppCheckTxIdle(uint32_t lockflag) { (void)lockflag; return txidle_ret; }
void ppProcessWaitQ(void) { nprocesswaitq++; }

int
ppRegisterTxCallback(void *fn, uint32_t idx)
{
	regcb_fn[nregcb] = fn;
	regcb_idx[nregcb] = idx;
	nregcb++;
	return 0;
}

int wifi_nvs_get_sta_listen_interval(void) { return nvs_listen_interval; }
uint32_t pm_host_now32(void) { return host_now32; }
uint32_t pm_host_wdev_count(void) { return host_wdev_count; }

void
pm_host_phy_init_cali(void *pmcblock)
{
	((Pmc *)pmcblock)->rtc_period = 0x1234;
}

/* ------------------------------------------------------------------ */

static Pmc *P;
static int fails;

#define CHK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		fails++; \
	} \
} while (0)

static void
reset_world(void)
{
	memset(g_ic, 0, sizeof(g_ic));
	memset(nvs_buf, 0, sizeof(nvs_buf));
	memset(sniffer_buf, 0, sizeof(sniffer_buf));
	memset(conn_buf, 0, sizeof(conn_buf));
	memset(bss_buf, 0, sizeof(bss_buf));
	memset(ps_buf, 0, sizeof(ps_buf));
	nvs_buf[0] = 1;		/* station mode */
	suspend_depth = 0;
	host_wdev_count = 0;
	host_timer_us = 0;
	txidle_ret = 0;
	trc_null = 1;
	nsetfn = narm = narm_us = ndisarm = 0;
	nregcb = nprocesswaitq = ntxpkt = nsendsetup = ngetmgtframe = 0;
	mgtframe_null = 0;
	WdevTimOffSet = 0;
	pend_flag_noise_check = 0;
	app_optimize_and_debug_mac = 0;
	/* conn: state field +0x78 = 5 (connected), +0xc0 ps ptr */
	*(void **)(g_ic + 8) = conn_buf;
	*(uint32_t *)(conn_buf + 0x78) = 5;
	*(void **)(conn_buf + 0xc0) = NULL;
	*(void **)(g_ic + 0xc) = bss_buf;
}

int
main(void)
{
	static const uint8_t mac[6] = { 0x48, 0x3f, 0xda, 0x7e, 0x39, 0x68 };
	uint8_t cfg[8];

	P = (Pmc *)pm_test_pmc();
	reset_world();

	/* ---- pm_attach defaults ------------------------------------ */
	pm_attach();
	CHK(P->sleep_type == 1);
	CHK(P->wait_time == 20);
	CHK(P->recv_multi == 1);
	CHK(P->wait_rx_udata_cnt == 4);
	CHK(P->cnt_udata == 4);
	CHK(P->max_bcn_timeout_ms == 24);
	CHK(P->max_bcn_early_ms == 4);
	CHK(P->wait_tx_cnt == 2);
	CHK(P->wait_rx_bdata_cnt == 2);
	CHK(P->cnt_tx == 2);
	CHK(P->cnt_bdata == 2);
	CHK(P->recv_bdata == 2);
	CHK(P->listen_interval == 3);
	CHK(*pm_test_cur_early() == 33500);
	CHK(*pm_test_cur_timeout() == 10000);
	CHK(nsetfn == 3);
	CHK(setfn_fn[0] == (void *)pm_wait_bcn_fun);
	CHK(setfn_fn[1] == (void *)pm_open_rf_for_bcn_fun);
	CHK(setfn_fn[2] == (void *)pm_wait_tx_rx_idle_fun);
	CHK(nregcb == 2);
	CHK(regcb_idx[0] == 6 && regcb_idx[1] == 5);
	CHK(suspend_depth == 0);

	/* ---- pm config get/set ------------------------------------- */
	memset(cfg, 0, sizeof(cfg));
	CHK(esp_wifi_get_pm_config(cfg) == 0);
	CHK(cfg[0] == 4 && cfg[1] == 24 && cfg[2] == 20 && cfg[3] == 2);
	CHK(cfg[4] == 2 && cfg[5] == 4 && cfg[6] == 2 && cfg[7] == 1);
	cfg[0] = 2; cfg[1] = 12; cfg[2] = 5; cfg[3] = 1;
	cfg[4] = 1; cfg[5] = 1; cfg[6] = 3;
	CHK(esp_wifi_set_pm_config(cfg) == 0);
	CHK(P->max_bcn_early_ms == 3);	/* blob clamps 2 -> 3 */
	CHK(P->max_bcn_timeout_ms == 13);	/* blob clamps 12 -> 13 */
	CHK(P->wait_time == 10);
	CHK(P->recv_bdata == 3);
	cfg[0] = 16; cfg[6] = 4;
	CHK(esp_wifi_set_pm_config(cfg) == 0);	/* blob clamps, never fails */
	CHK(P->max_bcn_early_ms == 15);
	CHK(P->recv_bdata == 3);
	CHK(P->recv_multi == cfg[7]);

	/* ---- addr / aid / sleep_for -------------------------------- */
	CHK(pm_set_addr(NULL, (void *)mac) == 1);
	CHK(memcmp(P->ap_mac, mac, 6) == 0);
	CHK(pm_sleep_for((void *)mac) == 1);
	CHK(pm_sleep_for((void *)"\x01\x02\x03\x04\x05\x06") == 0);
	{
		uint8_t conninfo[8];

		memset(conninfo, 0, sizeof(conninfo));
		conninfo[4] = 0x23; conninfo[5] = 0x01;
		CHK(pm_add_aid(conninfo) == 1);
		CHK(P->aid == 0x123);
		conninfo[2] = 1;
		CHK(pm_add_aid(conninfo) == 0);
		CHK(P->aid == 0x123);
	}
	CHK(pm_get_sleep_type() == 1);

	/* ---- ps lock bit -------------------------------------------- */
	esp_wifi_ps_lock();
	CHK((app_optimize_and_debug_mac & 1) != 0);
	esp_wifi_ps_unlock();
	CHK((app_optimize_and_debug_mac & 1) == 0);

	/* ---- reset helpers ------------------------------------------ */
	P->cnt_tx = 0;
	pm_reset_tdata();
	CHK(P->cnt_tx == P->wait_tx_cnt);
	P->cnt_bdata = 0; P->cnt_udata = 0;
	pm_have_more_data_to_recv();
	CHK(P->cnt_bdata == 0 && P->cnt_udata == 0);
	P->cnt_bdata = 0; P->cnt_udata = 1; P->wait_rx_udata_cnt = 4;
	pm_recv_all_data();
	CHK(P->cnt_bdata == P->wait_rx_bdata_cnt);
	CHK(P->cnt_udata == P->wait_rx_udata_cnt - 2);
	P->wait_rx_udata_cnt = 1;	/* blob: no write when wait <= 2 */
	P->cnt_udata = 0;
	pm_recv_all_data();
	CHK(P->cnt_udata == 0);
	P->wait_rx_udata_cnt = 4;

	/* ---- pm_allow_tx -------------------------------------------- */
	P->state = 0;
	CHK(pm_allow_tx(0) == 1);
	P->state = 7;
	CHK(pm_allow_tx(1) == 1);
	P->state = 2; P->sleep_type = 0;
	CHK(pm_allow_tx(0) == 1);
	P->sleep_type = 1; P->cnt_tx = 9;
	narm = 0;
	CHK(pm_allow_tx(1) == 1);
	CHK(P->cnt_tx == 0);		/* tx activity resets idle count */
	CHK(pm_test_idle_timer_set() == 1);

	/* ---- pm_change_to_state basics ------------------------------ */
	P->state = 0;
	pm_change_to_state(2);
	CHK(P->state == 2);
	P->state = 2;
	pm_change_to_state(4);		/* idle check passes -> state 3 */
	CHK(P->state == 3);
	sniffer_buf[0] = 1;		/* sniffer gate: no transition */
	P->state = 0;
	pm_change_to_state(2);
	CHK(P->state == 0);
	sniffer_buf[0] = 0;

	/* ---- pm_wait_bcn_fun: missed beacons ------------------------- */
	P->bcn_interval = 102400;
	P->next_bcn_tsf = 1000;
	P->miss_cnt = 0;
	P->recv_bdata = 2;
	P->state = 6;
	pm_wait_bcn_fun(0);
	CHK(P->miss_cnt == 1);
	CHK(P->next_bcn_tsf == 1000 + 102400);
	CHK(P->f60 == 1000);
	P->miss_cnt = 2;
	pm_wait_bcn_fun(0);		/* miss 3 > recv_bdata 2 -> state 4 */
	CHK(P->miss_cnt == 3);

	/* ---- pm_onBcnRx ---------------------------------------------- */
	{
		uint8_t frame[64];
		uint8_t descbuf[0x40];
		uint32_t rxts_word[2];
		uint64_t tsf = 5000;

		memset(frame, 0, sizeof(frame));
		memcpy(frame, &tsf, 8);
		frame[8] = 100;		/* beacon interval 100 TU */
		frame[9] = 0;
		/* SSID IE then TIM IE: id5 len4 count1 period3 ctl0 bmp0 */
		frame[12] = 0; frame[13] = 2; frame[14] = 'x'; frame[15] = 'y';
		frame[16] = 5; frame[17] = 4; frame[18] = 1; frame[19] = 3;
		frame[20] = 0; frame[21] = 0;
		memset(descbuf, 0, sizeof(descbuf));
		rxts_word[0] = 0;
		rxts_word[1] = 5000 + 16000;	/* -> hist idx (21000-0)>>7 */
		*(void **)(descbuf + 0x28) = rxts_word;

		P->state = 2;
		P->sleep_type = 1;
		P->wake_tsf = 0;
		P->cnt_bdata = P->wait_rx_bdata_cnt;
		P->cnt_udata = P->wait_rx_udata_cnt;
		P->aid = 0;
		*pm_test_cntnum() = 0;
		memset(pm_test_early_hist(), 0, 0x177);
		pm_onBcnRx(descbuf, frame, frame + 22);
		CHK(P->bcn_interval == (100u << 10));
		CHK(P->miss_cnt == 0);
		CHK(*pm_test_cntnum() == 1);
		/* f60 = rxts(21000) - tsf%interval(5000) = 16000 */
		CHK(P->f60 == 16000);
		CHK(P->next_bcn_tsf == (100u << 10) + 16000);
		/* dtim_count=1 -> wake after 1 interval */
		CHK(P->wake_tsf == (100u << 10) + 16000);
	}

	/* ---- keepalive ----------------------------------------------- */
	P->keepalive_dl = 0;
	host_wdev_count = 20000000;
	CHK(pm_send_keepalive_pkt() == 1);
	CHK(P->keepalive_dl == 20000000);
	CHK(pm_send_keepalive_pkt() == 0);

	/* ---- nullfunc frame fill ------------------------------------- */
	memset(fake_hdr, 0, sizeof(fake_hdr));
	memset(fake_desc, 0, sizeof(fake_desc));
	memset(fake_buf, 0, sizeof(fake_buf));
	host_wdev_count = 777;
	ntxpkt = 0;
	pm_send_nullfunc(0);
	CHK(ntxpkt == 1);
	CHK(fake_desc[5] == 777);
	CHK(fake_desc[4] == 0x40);
	CHK((fake_hdr[0] & 0x80000000u) != 0);
	pm_send_nullfunc(1);
	CHK(fake_desc[4] == 0x20);
	CHK((fake_buf[1] & 0x10) != 0);

	/* ---- rtc cali proc -------------------------------------------- */
	CHK(pm_rtc_clock_cali_proc() == 0x1234);
	CHK(P->rtc_period == 0x1234);

	/* ---- tsf time -------------------------------------------------- */
	host_now32 = 4242;
	CHK(wdev_get_tsf_time(0) == 4242);

	if (fails == 0)
		printf("PM: ALL PASS\n");
	else
		printf("PM: %d FAILURES\n", fails);
	return fails != 0;
}
