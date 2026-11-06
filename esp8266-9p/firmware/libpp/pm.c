/*
 * pm.c - Clean-room ESP8266 Wi-Fi power management (pm.o replacement).
 * Full header comment in pm.h. Transliterated from the disassembly and
 * Ghidra decompiles in re/inventory/b38/; the blob's own arithmetic and
 * quirks (inverted clamps, unreachable branches) are kept byte-exact.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "pm.h"

#ifdef PM_HOST_TEST
#include "test/pm_host.h"
#define WDEV_COUNT32 pm_host_wdev_count()
#else
#include "pp.h"		/* ppCheckTxIdle, ppProcessWaitQ, ppRegisterTxCallback */

extern char g_ic;
extern char *g_wifi_nvs;
extern char *sniffer_enable;
extern void *g_phyFuns;		/* function table, slots via byte offsets */
extern uint64_t WdevTimOffSet;
extern uint8_t pend_flag_noise_check;

extern void __wifi_task_resume_all(void);
extern void __wifi_task_suspend_all(void);
extern void clockgate_watchdog(int on);
extern void *ic_get_addr(int idx);
extern uint32_t esp_log_early_timestamp(void);
extern uint64_t esp_timer_get_time(void);
extern void esp_log_write(int level, const char *tag,
			  const char *fmt, ...);
extern void esp_sleep_lock(void);
extern void esp_sleep_unlock(void);
extern int ets_printf(const char *fmt, ...);
extern void *ieee80211_getmgtframe(int *len, int size, int arg3);
extern void ieee80211_send_setup(void *conn, void *frame, int len,
				 int waitmask, void *addr, void *dst,
				 void *bssid);
extern void noise_check_loop(int on, int second);
extern void os_timer_arm(void *timer, uint32_t ms, int repeat);
extern void os_timer_arm_us(void *timer, uint32_t us, int repeat);
extern void os_timer_disarm(void *timer);
extern void os_timer_setfn(void *timer, void (*fn)(void *), void *arg);
extern void phy_close_rf(void);
extern void phy_open_rf(void);
extern uint32_t pm_rtc_clock_cali(void *pmcblock);
extern void pm_set_sleep_cycles(uint32_t cycles);
extern uint32_t pm_usec2rtc(uint32_t usec, uint32_t period);
extern uint64_t pm_usec2rtc_64(uint64_t usec, uint32_t period);
extern void *rc_get_trc(uint32_t ifidx, const void *mac);

int wifi_nvs_get_sta_listen_interval(void);

#define WDEV_COUNT32 (*(volatile uint32_t *)0x3ff20c00)
#endif /* !PM_HOST_TEST */

/* ic fields pm reads through &g_ic (offsets from the blob's pools) */
#define IC_CONN(p)	(*(void **)((char *)(p) + 8))
#define IC_BSS(p)	(*(void **)((char *)(p) + 0xc))
#define IC_RTC_CALI_PERIOD(p)	(*(uint32_t *)((char *)(p) + 0xc))
#define IC_NO_DISTURB(p)	(*(uint8_t *)((char *)(p) + 0x160))
#define CONN_STATE(c)	(*(uint32_t *)((char *)(c) + 0x78))
#define CONN_PS(c)	(*(void **)((char *)(c) + 0xc0))

/* forward declarations (blob-internal helpers) */
static void pm_txcb_idx5(void *desc);
static void pm_txcb_idx6(void *desc);

/* ------------------------------------------------------------------ */
/* State (byte-exact with the blob's sections)                         */

static Pmc pmc;

/* .data of the blob */
static uint32_t cur_timeout = 10000;
static uint32_t cur_early = 33500;

/* .bss of the blob */
static uint8_t early_time_cnt[0x177];
static uint32_t tsf_diff;
static uint8_t cntnum;
static uint8_t idle_timer_set;
static uint8_t retry_idx5;		/* blob retry$8607 (tx callback 5) */
static uint8_t retry_idx6;		/* blob retry$8613 (tx callback 6) */

/* blob function-local statics (power-consumption accounting) */
static uint64_t the_time_of_open_rf;
static uint64_t all_time_of_open_rf;
static uint64_t last_open_rf_time;
static uint64_t last_close_cpu_and_rf_time;
static uint64_t last_now;
static uint64_t last_only_cpu_on_time;

/* Defined by the blob in its own .bss section; pp.c references it.
 * Initialized (strong .data, not a common) on purpose: a common here
 * makes ld extract libpp.a(pm.o) to upgrade it, which then collides
 * with every other pm symbol (same trap class as B37). */
uint32_t app_optimize_and_debug_mac = 0;

/* ------------------------------------------------------------------ */

void
pm_attach(void)
{
	__wifi_task_suspend_all();
	pmc.sleep_type = 1;
	pmc.wait_time = 0x14;
	pmc.recv_multi = 1;
	pmc.wait_rx_udata_cnt = 4;
	pmc.cnt_udata = 4;
	pmc.max_bcn_timeout_ms = 0x18;
	pmc.max_bcn_early_ms = 4;
	pmc.wait_tx_cnt = 2;
	pmc.wait_rx_bdata_cnt = 2;
	pmc.cnt_tx = 2;
	pmc.cnt_bdata = 2;
	pmc.recv_bdata = 2;
	pmc.listen_interval = (uint8_t)wifi_nvs_get_sta_listen_interval();
	pm_reset_param();
	os_timer_setfn(&pmc.timers[0], (void (*)(void *))pm_wait_bcn_fun, 0);
	os_timer_setfn(&pmc.timers[1], (void (*)(void *))pm_open_rf_for_bcn_fun, 0);
	os_timer_setfn(&pmc.timers[2], (void (*)(void *))pm_wait_tx_rx_idle_fun, 0);
	ppRegisterTxCallback((void *)pm_txcb_idx6, 6);
	ppRegisterTxCallback((void *)pm_txcb_idx5, 5);
	__wifi_task_resume_all();
}

/* Blob: pm_set_addr(ctx, mac) - the MAC is the second argument. */
int
pm_set_addr(void *ctx, void *mac)
{
	(void)ctx;
	memcpy(pmc.ap_mac, mac, 6);
	return 1;
}

/* Blob: pm_add_aid(conninfo) - reads a struct: u16 at +2 must be 0,
 * then the AID is the u16 at +4. Returns that zero-test. */
int
pm_add_aid(void *conninfo)
{
	int ok = *(uint16_t *)((uint8_t *)conninfo + 2) == 0;

	if (ok)
		pmc.aid = *(uint16_t *)((uint8_t *)conninfo + 4);
	return ok;
}

/* The blob's pm_sleep_for body is a 6-byte compare against pmc.ap_mac. */
int
pm_sleep_for(void *mac)
{
	return memcmp(mac, pmc.ap_mac, 6) == 0;
}

uint32_t
pm_get_sleep_type(void)
{
	return pmc.sleep_type;
}

void
pm_set_sleep_type(int type)
{
	__wifi_task_suspend_all();
	if ((int)pmc.sleep_type != type) {
		pm_reset_param();
		if (type == 0) {
			pmc.sleep_type = 0;
			if (2 < pmc.state) {
				__wifi_task_suspend_all();
				pm_change_to_state(0);
				__wifi_task_resume_all();
			}
			ets_printf("\033[0;32mI (%d) %s:pm stop\n\033[0m",
				   (int)esp_log_early_timestamp(), "wifi");
		} else {
			int old = (int)pmc.sleep_type;

			pmc.sleep_type = (uint32_t)type;
			if (old == 0 && g_wifi_nvs[0] == 1 &&
			    IC_CONN(&g_ic) != 0 &&
			    CONN_STATE(IC_CONN(&g_ic)) == 5) {
				__wifi_task_suspend_all();
				pm_change_to_state(2);
				__wifi_task_resume_all();
			}
			ets_printf("\033[0;32mI (%d) %s:pm start, type: %d\n\033[0m",
				   (int)esp_log_early_timestamp(), "wifi",
				   (int)pmc.sleep_type);
		}
	}
	__wifi_task_resume_all();
}

int
esp_wifi_get_pm_config(void *cfgp)
{
	uint8_t *cfg = (uint8_t *)cfgp;

	if (cfg == NULL)
		return 0;
	cfg[0] = pmc.max_bcn_early_ms;
	cfg[1] = pmc.max_bcn_timeout_ms;
	cfg[2] = pmc.wait_time;
	cfg[3] = pmc.wait_tx_cnt;
	cfg[4] = pmc.wait_rx_bdata_cnt;
	cfg[5] = pmc.wait_rx_udata_cnt;
	cfg[6] = pmc.recv_bdata;
	cfg[7] = pmc.recv_multi;
	return 0;
}

/* Blob clamps every field into range (never errors) and also
 * stores the 8th byte into recv_multi. */
int
esp_wifi_set_pm_config(void *cfgp)
{
	const uint8_t *cfg = (const uint8_t *)cfgp;

	if (cfg == NULL)
		return 0;
	pmc.max_bcn_early_ms = 16 <= cfg[0] ? 15 :
		(cfg[0] < 3 ? 3 : cfg[0]);
	pmc.max_bcn_timeout_ms = 33 <= cfg[1] ? 32 :
		(cfg[1] < 13 ? 13 : cfg[1]);
	pmc.wait_time = 101 <= cfg[2] ? 100 :
		(cfg[2] < 10 ? 10 : cfg[2]);
	pmc.wait_tx_cnt = 21 <= cfg[3] ? 20 :
		(cfg[3] == 0 ? 1 : cfg[3]);
	pmc.wait_rx_bdata_cnt = 101 <= cfg[4] ? 100 :
		(cfg[4] == 0 ? 1 : cfg[4]);
	pmc.wait_rx_udata_cnt = 101 <= cfg[5] ? 100 :
		(cfg[5] == 0 ? 1 : cfg[5]);
	pmc.recv_bdata = 3 < cfg[6] ? 3 : cfg[6];
	pmc.recv_multi = cfg[7];
	return 0;
}

uint8_t
esp_wifi_get_pm_recv_multi_data(void)
{
	return pmc.recv_multi;
}

void
esp_wifi_set_pm_recv_multi_data(uint8_t v)
{
	pmc.recv_multi = v;
}

/* The blob's ps lock is just bit 0 of app_optimize_and_debug_mac:
 * while set, pm will not enter modem sleep (see wait_tx_rx_idle_fun). */
int
esp_wifi_ps_lock(void)
{
	app_optimize_and_debug_mac |= 1;
	return 0;
}

int
esp_wifi_ps_unlock(void)
{
	app_optimize_and_debug_mac &= ~1u;
	return 0;
}

void
pm_reset_param(void)
{
	cur_early = 33500;
	cur_timeout = 10000;
	memset(early_time_cnt, 0, sizeof(early_time_cnt));
	cntnum = 0;
}

void
pm_reset_tdata(void)
{
	pmc.cnt_tx = pmc.wait_tx_cnt;
}

void
pm_have_more_data_to_recv(void)
{
	if (pmc.cnt_bdata < pmc.wait_rx_bdata_cnt)
		pmc.cnt_bdata = 0;
	if (pmc.cnt_udata < pmc.wait_rx_udata_cnt)
		pmc.cnt_udata = 0;
}

void
pm_recv_all_data(void)
{
	pmc.cnt_bdata = pmc.wait_rx_bdata_cnt;
	if (pmc.cnt_udata != pmc.wait_rx_udata_cnt &&
	    2 < pmc.wait_rx_udata_cnt)
		pmc.cnt_udata = pmc.wait_rx_udata_cnt - 2;
}

void
pm_reset_keepalive_time(void)
{
	pmc.keepalive_dl = (uint32_t)WdevTimOffSet + WDEV_COUNT32;
}

void
pm_set_the_time_of_phy_init(void)
{
	the_time_of_open_rf = esp_timer_get_time();
}

uint64_t
pm_get_sleep_cycles_64(uint64_t us)
{
	return pm_usec2rtc_64(us, pmc.rtc_period);
}

void
pm_set_sleep_time(uint32_t us)
{
	pm_set_sleep_cycles(pm_usec2rtc(us, pmc.rtc_period));
}

uint64_t
wdev_get_tsf_time(int use_hw)
{
#ifdef PM_HOST_TEST
	(void)use_hw;
	return pm_host_now32();
#else
	if (use_hw == 0)
		return (uint64_t)(uint32_t)((uint32_t)WdevTimOffSet +
					     tsf_diff + WDEV_COUNT32);
	return ((uint64_t)*(volatile uint32_t *)0x3ff2104c << 32) |
		*(volatile uint32_t *)0x3ff21048;
#endif
}

uint32_t
pm_rtc_clock_cali_proc(void)
{
	pmc.rtc_period = 0;
#ifdef PM_HOST_TEST
	pm_host_phy_init_cali(&pmc);
#else
	(*(void (**)(int, int, int))((char *)g_phyFuns + 0x98))(0x6a, 2, 8);
	pm_rtc_clock_cali(&pmc);
#endif
	return pmc.rtc_period;
}

void
wifi_pm_open_rf(void)
{
	esp_sleep_lock();
	the_time_of_open_rf = esp_timer_get_time();
	clockgate_watchdog(1);
	phy_open_rf();
	clockgate_watchdog(0);
	if (pend_flag_noise_check == 1) {
		pend_flag_noise_check = 0;
		noise_check_loop(1, 1);
	}
}

void
wifi_pm_close_rf(void)
{
	uint64_t now = esp_timer_get_time();

	all_time_of_open_rf += now - the_time_of_open_rf;
	clockgate_watchdog(1);
	phy_close_rf();
	clockgate_watchdog(0);
	esp_sleep_unlock();
}

void
pm_set_idle_timer(void)
{
	if (idle_timer_set == 0) {
		os_timer_arm(&pmc.timers[2], pmc.wait_time, 0);
		idle_timer_set = 1;
	}
}

int
pm_rf_is_closed(void)
{
	return pmc.state == 1 || pmc.state == 6;
}

/* Blob helper duplicated inline at both ns==6 arm sites. */
static int
pm_bcn_delta(uint32_t *out)
{
	uint32_t now = WDEV_COUNT32;
	uint32_t wake = pmc.wake_tsf + 32000;
	uint32_t d;

	if (wake <= now + cur_early + (uint32_t)WdevTimOffSet)
		return 0;
	d = (wake - now) - cur_early - (uint32_t)WdevTimOffSet;
	if (1199999 < d)
		return 0;
	if (d <= 5000)
		return 0;
	*out = d;
	return 1;
}

void
pm_change_to_state(int ns)
{
	uint8_t mode = (uint8_t)g_wifi_nvs[0];
	uint32_t cur;
	uint32_t delta;

	if ((uint8_t)(mode - 2) < 2 && IC_BSS(&g_ic) != 0 &&
	    (*(uint32_t *)((char *)IC_BSS(&g_ic) + 0x104) & 0x100) != 0) {
		pmc.old_state = (uint32_t)ns;
		ns = 0;
	}
	if (sniffer_enable[0] != 0 && ns != 7)
		return;
	if (IC_NO_DISTURB(&g_ic) != 0) {
		if (ns == 7)
			return;
		pmc.old_state = (uint32_t)ns;
		return;
	}
	cur = pmc.state;
	if (cur == 7) {
		pmc.old_state = (uint32_t)ns;
		return;
	}
	if (ns == (int)cur && ns != 6)
		return;
	if (ns == 2) {
		if (cur == 0) {
			pmc.state = 2;
			return;
		}
		if (cur == 5) {
			pmc.state = 2;
			ppProcessWaitQ();
			return;
		}
		if (cur != 4) {
			if (cur == 6) {
				os_timer_disarm(&pmc.timers[0]);
				wifi_pm_open_rf();
			} else if (cur != 3) {
				return;
			}
		}
		pm_send_nullfunc(0);
		pmc.state = 5;
		return;
	}
	if (ns < 3) {
		if (ns == 0) {
			if (cur == 6) {
				os_timer_disarm(&pmc.timers[0]);
				wifi_pm_open_rf();
				pm_send_nullfunc(0);
			} else if (cur == 1) {
				wifi_pm_open_rf();
			}
			pmc.state = 0;
			return;
		}
		/* ns == 1 */
		if (cur == 6) {
			pmc.state = 1;
			return;
		}
		if ((mode & 0xfd) == 1 && IC_CONN(&g_ic) != 0 &&
		    CONN_STATE(IC_CONN(&g_ic)) == 5)
			return;
		if (ppCheckTxIdle(1) != 0 ||
		    pmc.cnt_udata < pmc.wait_rx_udata_cnt ||
		    pmc.cnt_bdata < pmc.wait_rx_bdata_cnt ||
		    pmc.cnt_tx < pmc.wait_tx_cnt) {
			pm_wait_tx_rx_idle_fun(0);
			return;
		}
		wifi_pm_close_rf();
		pmc.state = 1;
		return;
	}
	if (ns == 6) {
		if (cur - 3 < 2) {
			if (ppCheckTxIdle(1) != 0 ||
			    pmc.cnt_udata < pmc.wait_rx_udata_cnt ||
			    pmc.cnt_bdata < pmc.wait_rx_bdata_cnt ||
			    pmc.cnt_tx < pmc.wait_tx_cnt) {
				pm_wait_tx_rx_idle_fun(0);
				return;
			}
			if (!pm_bcn_delta(&delta))
				return;
			os_timer_arm_us(&pmc.timers[0], delta, 0);
			wifi_pm_close_rf();
			pmc.state = 6;
			return;
		}
		if (cur != 2) {
			if (cur != 6)
				return;
			if (!pm_bcn_delta(&delta))
				return;
			os_timer_arm_us(&pmc.timers[0], delta, 0);
			pmc.state = cur;
			return;
		}
		if (ppCheckTxIdle(1) != 0)
			return;
		if (pmc.cnt_udata < pmc.wait_rx_udata_cnt)
			return;
		if (pmc.cnt_bdata < pmc.wait_rx_bdata_cnt)
			return;
		if (pmc.cnt_tx < pmc.wait_tx_cnt)
			return;
		/* fall through: nullfunc, state 3 */
	} else if (ns == 7) {
		pmc.old_state = cur;
		if (cur != 4) {
			if (cur == 6 || cur == 1) {
				os_timer_disarm(&pmc.timers[0]);
				wifi_pm_open_rf();
			} else {
				if (cur != 2 && cur != 5) {
					if (cur != 0)
						return;
					if (IC_CONN(&g_ic) == 0 || mode != 1 ||
					    CONN_STATE(IC_CONN(&g_ic)) != 5) {
						pmc.state = 7;
						return;
					}
				}
				pm_send_nullfunc(1);
			}
		}
		pmc.state = 7;
		return;
	} else {
		if (ns != 4)
			return;
		if (cur == 3) {
			pmc.state = 4;
			return;
		}
		if (cur == 6) {
			os_timer_disarm(&pmc.timers[0]);
			wifi_pm_open_rf();
			pmc.state = 4;
			return;
		}
		if (cur != 2)
			return;
		if (ppCheckTxIdle(1) != 0)
			return;
	}
	pm_send_nullfunc(1);
	pmc.state = 3;
}

uint32_t
pm_allow_tx(uint32_t is_tx)
{
	uint32_t state = pmc.state;
	uint32_t r = (state == 0 || state == 7);

	if (r != 0)
		return r;
	if (state == 2) {
		if (pmc.sleep_type == 0)
			return 1;
		if ((int8_t)is_tx == 0)
			goto lab11450;
		pmc.cnt_tx = 0;
	} else if ((int8_t)is_tx != 0) {
		pmc.cnt_tx = 0;
	}
	if (state == 1) {
		__wifi_task_suspend_all();
		pm_change_to_state(0);
		__wifi_task_resume_all();
		pm_set_idle_timer();
		return state;
	}
lab11450:
	if (state == 2 || state == 5) {
		pm_set_idle_timer();
		r = 1;
	} else {
		__wifi_task_suspend_all();
		pm_change_to_state(2);
		__wifi_task_resume_all();
		pm_set_idle_timer();
	}
	return r;
}

void
pm_wait_tx_rx_idle_fun(void *arg)
{
	int from6 = 0;

	(void)arg;
	__wifi_task_suspend_all();
	os_timer_disarm(&pmc.timers[2]);
	idle_timer_set = 0;
	if (pmc.state == 6)
		from6 = 1;
	if (!from6 && pmc.state != 1) {
		uint8_t cb, cu;
		int ns;

		if (ppCheckTxIdle(0) == 0) {
			pmc.cnt_tx = pmc.cnt_tx + 1;
		} else {
			pmc.cnt_tx = 0;
			ppProcessWaitQ();
		}
		cb = pmc.cnt_bdata;
		cu = pmc.cnt_udata;
		pmc.cnt_bdata = cb + 1;
		pmc.cnt_udata = cu + 1;
		if (pmc.wait_rx_bdata_cnt < (uint8_t)(cb + 1))
			pmc.cnt_bdata = pmc.wait_rx_bdata_cnt;
		if (pmc.wait_rx_udata_cnt < (uint8_t)(cu + 1))
			pmc.cnt_udata = pmc.wait_rx_udata_cnt;
		if (pmc.wait_tx_cnt < pmc.cnt_tx)
			pmc.cnt_tx = pmc.wait_tx_cnt;
		if (pmc.cnt_bdata < pmc.wait_rx_bdata_cnt ||
		    pmc.cnt_udata < pmc.wait_rx_udata_cnt ||
		    pmc.cnt_tx < pmc.wait_tx_cnt) {
			pm_set_idle_timer();
		} else if (g_wifi_nvs[0] == 1 && IC_CONN(&g_ic) != 0 &&
			   CONN_STATE(IC_CONN(&g_ic)) == 5) {
			if ((app_optimize_and_debug_mac & 1) != 0) {
				pm_set_idle_timer();
			} else {
				__wifi_task_suspend_all();
				ns = 6;
				pm_change_to_state(ns);
				__wifi_task_resume_all();
			}
		} else {
			if ((app_optimize_and_debug_mac & 1) != 0) {
				pm_set_idle_timer();
			} else {
				__wifi_task_suspend_all();
				ns = 1;
				pm_change_to_state(ns);
				__wifi_task_resume_all();
			}
		}
	}
	__wifi_task_resume_all();
}

void
pm_wait_bcn_fun(void *arg)
{
	uint32_t e, t;
	uint8_t missed;
	int ns;

	(void)arg;
	__wifi_task_suspend_all();
	pmc.f60 = pmc.next_bcn_tsf;
	pmc.next_bcn_tsf = pmc.next_bcn_tsf + pmc.bcn_interval;
	pmc.wake_tsf = pmc.next_bcn_tsf;
	missed = pmc.miss_cnt + 1;
	pmc.miss_cnt = missed;
	e = cur_early + 0x100;
	t = cur_timeout + 0x200;
	if ((uint32_t)pmc.max_bcn_early_ms * 1000 + 32000 < e)
		e = (uint32_t)pmc.max_bcn_early_ms * 1000 + 32000;
	if (e < 0x5dc)
		e = 0x5dc;
	cur_early = e;
	if ((uint32_t)pmc.max_bcn_timeout_ms * 1000 < t)
		t = (uint32_t)pmc.max_bcn_timeout_ms * 1000;
	if (t <= 9999)
		t = 10000;
	cur_timeout = t;
	__wifi_task_suspend_all();
	if (pmc.recv_bdata < missed)
		ns = 4;
	else if ((uint32_t)pmc.recv_bdata + 1 < (uint32_t)missed)
		ns = 2;
	else
		ns = 6;
	pm_change_to_state(ns);
	__wifi_task_resume_all();
	__wifi_task_resume_all();
}

void
pm_open_rf_for_bcn_fun(void *arg)
{
	(void)arg;
	__wifi_task_suspend_all();
	os_timer_disarm(&pmc.timers[0]);
	if (pmc.state == 6) {
		__wifi_task_suspend_all();
		pm_change_to_state(4);
		__wifi_task_resume_all();
		os_timer_arm_us(&pmc.timers[0], cur_early, 0);
	}
	__wifi_task_resume_all();
}

void
pm_goto_rf_on(void)
{
	__wifi_task_suspend_all();
	pm_change_to_state(0);
	__wifi_task_resume_all();
}

void
pm_goto_rf_on_set_old_state(void)
{
	if (pmc.state != 7)
		pmc.old_state = pmc.state;
	__wifi_task_suspend_all();
	pm_change_to_state(0);
	__wifi_task_resume_all();
}

void
pm_goto_rf_off(void)
{
	__wifi_task_suspend_all();
	os_timer_disarm(&pmc.timers[0]);
	os_timer_disarm(&pmc.timers[1]);
	__wifi_task_suspend_all();
	pm_change_to_state(1);
	__wifi_task_resume_all();
	__wifi_task_resume_all();
}

void
pm_goto_rf_old_state(void)
{
	uint32_t old;

	__wifi_task_suspend_all();
	old = pmc.old_state;
	if (old == 1) {
		pmc.state = 0;
	} else if (old == 6) {
		pmc.state = 4;
	} else if (old != 0) {
		pmc.state = old;
		__wifi_task_resume_all();
		return;
	} else {
		if (g_wifi_nvs[0] == 1 && IC_CONN(&g_ic) != 0 &&
		    CONN_STATE(IC_CONN(&g_ic)) == 5)
			pm_send_nullfunc(0);
		pmc.state = 0;
	}
	__wifi_task_suspend_all();
	pm_change_to_state((int)old);
	__wifi_task_resume_all();
	__wifi_task_resume_all();
}

void
pm_wifi_connected(void)
{
	uint8_t li = (uint8_t)wifi_nvs_get_sta_listen_interval();

	pmc.listen_interval = li < 11 ? li : 10;
	pm_reset_param();
	__wifi_task_suspend_all();
	pm_change_to_state(2);
	__wifi_task_resume_all();
}

void
pm_wifi_set_mode(void)
{
	int ns;

	__wifi_task_suspend_all();
	if (g_wifi_nvs[0] == 1 && IC_CONN(&g_ic) != 0 &&
	    CONN_STATE(IC_CONN(&g_ic)) == 5) {
		__wifi_task_suspend_all();
		ns = 2;
	} else if ((uint8_t)(g_wifi_nvs[0] - 2) < 2 &&
		   IC_BSS(&g_ic) != 0 &&
		   (*(uint32_t *)((char *)IC_BSS(&g_ic) + 0x104) & 0x100) != 0) {
		__wifi_task_suspend_all();
		ns = 0;
	} else {
		__wifi_task_suspend_all();
		ns = 1;
	}
	pm_change_to_state(ns);
	__wifi_task_resume_all();
	__wifi_task_resume_all();
}

int
pm_scan_start(void)
{
	int ok;

	__wifi_task_suspend_all();
	if (pmc.state == 7 || pmc.state == 0) {
		__wifi_task_resume_all();
		return 1;
	}
	__wifi_task_suspend_all();
	pm_change_to_state(7);
	__wifi_task_resume_all();
	__wifi_task_resume_all();
	ok = (pmc.state == 7);
	return ok;
}

void
pm_scan_stop(void)
{
	__wifi_task_suspend_all();
	if (pmc.state == 7)
		pm_goto_rf_old_state();
	__wifi_task_resume_all();
}

int
pm_send_keepalive_pkt(void)
{
	int due;

	due = 10000000u < (uint32_t)((uint32_t)WdevTimOffSet -
				      pmc.keepalive_dl + WDEV_COUNT32);
	if (due) {
		pmc.keepalive_dl = (uint32_t)WdevTimOffSet + WDEV_COUNT32;
		pm_send_nullfunc((pmc.state & ~2u) == 4);
	}
	return due;
}

void
pm_send_nullfunc(uint32_t pmflag)
{
	void *conn = IC_CONN(&g_ic);

	pmflag &= 0xff;
	if (conn != 0 && CONN_STATE(conn) == 5) {
		int len = 0x18;
		int outlen = 0;
		uint32_t *frame;

		if (CONN_PS(conn) != 0 &&
		    (*(uint32_t *)((char *)CONN_PS(conn) + 8) & 0x42) != 0)
			len = 0x1a;
		frame = (uint32_t *)ieee80211_getmgtframe(&outlen, len, 0);
		if (frame == NULL) {
			esp_log_write(1, "wifi", "n buf for null");
		} else {
			uint8_t *buf = (uint8_t *)(uintptr_t)frame[3];
			uint32_t *hdr = (uint32_t *)(uintptr_t)frame[0];
			uint32_t *desc;

			memset(buf, 0, (size_t)len);
			*(uint16_t *)((uint8_t *)frame + 0x1a) = 0;
			*(uint16_t *)((uint8_t *)frame + 0x18) =
				(uint16_t)len;
			*hdr |= 0x80000000u;
			*hdr |= 0x40000000u;
			*hdr &= 0xdfffffffu;
			*hdr = (*hdr & 0xff000fffu) | ((uint32_t)len << 12);
			ieee80211_send_setup(conn, frame, 0x48, 0,
					     ic_get_addr(0),
					     pmc.ap_mac, pmc.ap_mac);
			desc = (uint32_t *)(uintptr_t)frame[10];
			desc[5] = WDEV_COUNT32;
			desc[2] = (desc[2] & 0xff8fffffu & 0xfffffff0u) | 6;
			*desc &= 0xfffffff7u;
			desc[1] &= 0xfffffffdu;
			frame[8] = 0;
			if (pmflag == 0) {
				desc[4] = 0x40;
			} else {
				buf[1] |= 0x10;
				desc = (uint32_t *)(uintptr_t)frame[10];
				desc[4] = 0x20;
			}
			ppTxPkt((void *)frame);
		}
	}
}

void
pm_onBcnRx(void *desc, uint8_t *frame, uint8_t *end)
{
	uint32_t *fp = (uint32_t *)frame;
	uint8_t *tim = NULL;
	uint8_t *p;
	uint32_t interval;
	uint32_t rxts;
	int stype;

	if (end - frame < 0xc)
		return;
	pmc.bcn_interval = (uint32_t)(*(uint16_t *)(frame + 8)) << 10;
	os_timer_disarm(&pmc.timers[0]);
	os_timer_disarm(&pmc.timers[1]);
	stype = (int)pmc.sleep_type;
	if (stype == 0) {
		pm_send_keepalive_pkt();
		return;
	}
	if ((app_optimize_and_debug_mac & 1) != 0) {
		pm_send_keepalive_pkt();
		pm_set_idle_timer();
		return;
	}
	pmc.miss_cnt = 0;
	if (g_wifi_nvs[0] != 1 || IC_CONN(&g_ic) == 0 ||
	    CONN_STATE(IC_CONN(&g_ic)) != 5) {
		pm_send_keepalive_pkt();
		return;
	}
	interval = pmc.bcn_interval;
	if (interval == 0)
		return;
	for (p = frame + 12; 1 < end - p; p += p[1] + 2) {
		if (end - p <= p[1] + 1)
			return;
		if (p[0] == 5)
			tim = p;
	}
	if (tim == NULL)
		return;
	tsf_diff = (fp[0] - WDEV_COUNT32) - (uint32_t)WdevTimOffSet;
	if ((uint16_t)(pmc.cnt_bdata | ((uint16_t)pmc.cnt_udata << 8)) ==
	    (uint16_t)(pmc.wait_rx_bdata_cnt |
		       ((uint16_t)pmc.wait_rx_udata_cnt << 8))) {
		uint8_t bctl = tim[4];
		uint16_t aid = pmc.aid;

		if ((bctl & 1) != 0)
			pmc.cnt_bdata = 0;
		if ((aid & 0x3fff) != 0) {
			uint32_t byteidx = (aid & 0x3fff) >> 3;
			uint32_t off = bctl & 0xfe;

			if (byteidx < off ||
			    (uint32_t)((off + (tim[1] - 4)) & 0xff) < byteidx ||
			    ((tim[(byteidx - off) + 5] &
			      (1 << (aid & 7))) == 0))
				goto lab_da3;
		}
		pmc.cnt_udata = 0;
	}
lab_da3:
	if (pmc.cnt_bdata == 0 && pmc.recv_multi == 0)
		pmc.cnt_bdata = pmc.wait_rx_bdata_cnt;
	rxts = *(uint32_t *)(*(uint8_t **)((uint8_t *)desc + 0x28) + 4);
	{
		uint32_t idx = (rxts + 16000 - pmc.wake_tsf) >> 7;
		uint64_t tsf = (uint64_t)fp[0] | ((uint64_t)fp[1] << 32);
		uint32_t rem, f60;
		int i;

		if (idx - 1 < 0x176) {
			early_time_cnt[idx]++;
			cntnum++;
		}
		if (99 < cntnum) {
			uint8_t acc = 0;
			uint32_t lo = 0, hi;

			for (i = 0; i < 0x177; i++) {
				acc = (uint8_t)(acc + early_time_cnt[i]);
				lo = (uint32_t)i;
				if (4 < acc)
					break;
			}
			if (i == 0x177)
				lo = 0;
			hi = lo;
			for (;;) {
				hi++;
				if (0x176 < hi) {
					hi = 0;
					break;
				}
				acc = (uint8_t)(acc + early_time_cnt[hi]);
				if (0x5f <= acc)
					break;
			}
			cur_early = 0xbdf4 - lo * 0x80;
			cur_timeout = (hi - lo) * 0x80 + 0x80;
			if ((uint32_t)pmc.max_bcn_early_ms * 1000 + 32000 <
			    cur_early)
				cur_early = (uint32_t)pmc.max_bcn_early_ms *
					1000 + 32000;
			if (cur_early < 0x5dc)
				cur_early = 0x5dc;
			if ((uint32_t)pmc.max_bcn_timeout_ms * 1000 <
			    cur_timeout)
				cur_timeout =
					(uint32_t)pmc.max_bcn_timeout_ms * 1000;
			if (cur_timeout <= 9999)
				cur_timeout = 10000;
			memset(early_time_cnt, 0, sizeof(early_time_cnt));
			cntnum = 0;
		}
		rem = (uint32_t)(tsf % interval);
		if ((uint32_t)(interval - 16000) < rem)
			f60 = interval + rxts;
		else
			f60 = rxts;
		pmc.f60 = f60 - rem;
		pmc.next_bcn_tsf = interval + pmc.f60;
	}
	{
		uint32_t n;

		if (stype == 1) {
			n = tim[2];
			if (n == 0)
				n = tim[3];
		} else {
			n = pmc.listen_interval;
			if (stype == 2) {
				uint32_t dcnt = tim[2];

				if (n == 0) {
					n = dcnt;
					if (dcnt == 0)
						n = tim[3];
				} else if (dcnt == 0) {
					uint32_t dper = tim[3];

					if (dper >> 1 < n) {
						n = ((n + (dper >> 1)) / dper) *
							dper;
						n &= 0xff;
					}
				} else if (dcnt < n) {
					uint32_t dper = tim[3];

					n = ((n - dcnt + (dper >> 1)) / dper) *
						dper + dcnt;
					n &= 0xff;
				}
			}
		}
		if (n == 0)
			n = 3;
		pmc.wake_tsf = n * interval + pmc.f60;
	}
	if (pmc.cnt_udata == 0) {
		/* Blob calls pm_allow_tx() with a2 untouched. */
		pm_allow_tx(0);
		return;
	}
	if (pmc.cnt_bdata == 0) {
		if (pmc.state == 6) {
			__wifi_task_suspend_all();
			pm_change_to_state(4);
			__wifi_task_resume_all();
		}
		pm_wait_tx_rx_idle_fun(0);
		return;
	}
	if (pm_send_keepalive_pkt() != 0)
		return;
	__wifi_task_suspend_all();
	pm_change_to_state(6);
	__wifi_task_resume_all();
}

/* pp tx-done callbacks, registered by pm_attach at indices 5 and 6. */
static uint32_t *
pm_desc_status(void *desc)
{
	return (uint32_t *)(*(uint8_t **)((uint8_t *)desc + 0x28) + 4);
}

static void
pm_txcb_idx5(void *desc)
{
	uint32_t *st = pm_desc_status(desc);

	if ((*st & 0x3fc0000) == 0x40000) {
		pmc.cnt_tx = pmc.wait_tx_cnt;
		retry_idx5 = 0;
		if (pmc.state == 4) {
			__wifi_task_suspend_all();
			pm_change_to_state(6);
			__wifi_task_resume_all();
		} else if (pmc.state == 3) {
			os_timer_disarm(&pmc.timers[2]);
			os_timer_arm(&pmc.timers[2], 10, 0);
			idle_timer_set = 1;
		}
	} else {
		uint8_t b = retry_idx5;

		retry_idx5 = b + 1;
		if (0xe < b)
			retry_idx5 = 0xe;
		pm_send_nullfunc(1);
	}
}

static void
pm_txcb_idx6(void *desc)
{
	uint32_t *st = pm_desc_status(desc);

	if (0xf < retry_idx6)
		*st = (*st & 0xfc03ffffu) | 0x40000;
	if ((*st & 0x3fc0000) == 0x40000) {
		retry_idx6 = 0;
		if (pmc.state == 5) {
			__wifi_task_suspend_all();
			pm_change_to_state(2);
			__wifi_task_resume_all();
		}
		ppProcessWaitQ();
	} else if (rc_get_trc(0, pmc.ap_mac) != NULL) {
		retry_idx6++;
		pm_send_nullfunc(0);
	}
}

void
esp_power_consumption_info(uint32_t reset)
{
	uint64_t now = esp_timer_get_time();
	uint64_t allopen = all_time_of_open_rf;
	uint64_t run, rfon, only, off;
	uint32_t q;

	reset &= 0xff;
	if (pmc.state != 1 && pmc.state != 6) {
		uint64_t now2 = esp_timer_get_time();

		allopen = all_time_of_open_rf + (now2 - the_time_of_open_rf);
	}
	run = now;
	rfon = allopen;
	only = (now - WdevTimOffSet) - allopen;
	off = WdevTimOffSet;
	if (reset != 0) {
		rfon = allopen - last_open_rf_time;
		last_open_rf_time = allopen;
		off = WdevTimOffSet - last_close_cpu_and_rf_time;
		last_close_cpu_and_rf_time = WdevTimOffSet;
		run = now - last_now;
		last_now = now;
		only = only - last_only_cpu_on_time;
		last_only_cpu_on_time = (now - WdevTimOffSet) - allopen;
	}
	ets_printf("\033[0;32mI (%d) %s:RUN time       : 0x%08x%08x us = %10dms\n\033[0m",
		   (int)esp_log_early_timestamp(), "wifi",
		   (uint32_t)(run >> 32), (uint32_t)run,
		   (uint32_t)((run + 500) / 1000));
	ets_printf("\033[0;32mI (%d) %s:CPU and RF on  : 0x%08x%08x us = %10dms\n\033[0m",
		   (int)esp_log_early_timestamp(), "wifi",
		   (uint32_t)(rfon >> 32), (uint32_t)rfon,
		   (uint32_t)((rfon + 500) / 1000));
	ets_printf("\033[0;32mI (%d) %s:Only CPU on    : 0x%08x%08x us = %10dms\n\033[0m",
		   (int)esp_log_early_timestamp(), "wifi",
		   (uint32_t)(only >> 32), (uint32_t)only,
		   (uint32_t)((only + 500) / 1000));
	ets_printf("\033[0;32mI (%d) %s:CPU and RF off : 0x%08x%08x us = %10dms\n\033[0m",
		   (int)esp_log_early_timestamp(), "wifi",
		   (uint32_t)(off >> 32), (uint32_t)off,
		   (uint32_t)((off + 500) / 1000));
	q = (uint32_t)(((rfon * 100000) / run + 5) / 10);
	ets_printf("\033[0;32mI (%d) %s:ratio of CPU and RF on : %2d.%02d%c\n\033[0m",
		   (int)esp_log_early_timestamp(), "wifi",
		   (int)(q / 100), (int)(q % 100), 0x25);
	q = (uint32_t)(((only * 100000) / run + 5) / 10);
	ets_printf("\033[0;32mI (%d) %s:ratio of only CPU on   : %2d.%02d%c\n\033[0m",
		   (int)esp_log_early_timestamp(), "wifi",
		   (int)(q / 100), (int)(q % 100), 0x25);
	q = (uint32_t)(((off * 100000) / run + 5) / 10);
	ets_printf("\033[0;32mI (%d) %s:ratio of CPU and RF off: %2d.%02d%c\n\033[0m",
		   (int)esp_log_early_timestamp(), "wifi",
		   (int)(q / 100), (int)(q % 100), 0x25);
	{
		uint64_t joules = off * 297 + only * 4620 + 50000000 +
			rfon * 23100;

		ets_printf("\033[0;32mI (%d) %s:Power consumption        : %8d.%03dJ(A*V*s)\n\033[0m",
			   (int)esp_log_early_timestamp(), "wifi",
			   (uint32_t)(joules / 100000000000ull),
			   (uint32_t)((joules / 100000000) % 1000));
	}
	{
		uint64_t avg = off * 2970 + only * 46200 + rfon * 231000 +
			run / 2;

		avg /= run;
		ets_printf("\033[0;32mI (%d) %s:Average Power consumption: %8d.%03dmW(A*V/s)\n\033[0m",
			   (int)esp_log_early_timestamp(), "wifi",
			   (uint32_t)(avg / 1000), (uint32_t)(avg % 1000));
	}
	ets_printf("\033[0;32mI (%d) %s:ear: %u, to: %u\n\033[0m",
		   (int)esp_log_early_timestamp(), "wifi",
		   cur_early, cur_timeout);
}

#ifdef PM_HOST_TEST
void *pm_test_pmc(void) { return &pmc; }
uint32_t *pm_test_cur_early(void) { return &cur_early; }
uint32_t *pm_test_cur_timeout(void) { return &cur_timeout; }
uint8_t *pm_test_early_hist(void) { return early_time_cnt; }
uint8_t *pm_test_cntnum(void) { return &cntnum; }
int pm_test_idle_timer_set(void) { return idle_timer_set; }
#endif
