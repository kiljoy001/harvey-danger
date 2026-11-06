/*
 * pm.h - Clean-room power management (replaces pm.o from libpp.a).
 *
 * Transliterated from the xtensa disassembly and Ghidra decompiles in
 * re/inventory/b38/. Pure C: no Espressif .a inputs, only calls into
 * the same undefined symbols the blob used (g_ic fields, SDK timers,
 * phy rf control, rtc helpers in libphy).
 *
 * The pmc state block is byte-exact with the blob's .bss.pmc: the
 * surrounding SDK objects poke it through the same offsets, and
 * pm_attach wires three ETSTimer blocks at the front
 * (+0x00/+0x1c/+0x38, 28 bytes each).
 */

#ifndef PM_H
#define PM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pm control block (blob .bss.pmc, 0x90 bytes) ----------------- */

typedef struct {
	uint32_t w[7];		/* SDK ETSTimer, 28 bytes */
} PmTimer;

typedef struct {
	union {
		PmTimer timers[3];	/* +0x00 wait_bcn     */
						/* +0x1c open_rf_bcn  */
						/* +0x38 wait_idle    */
		struct {
			uint32_t _t0w0;		/* +0x00 */
			uint32_t rtc_period;	/* +0x04 phy writes here */
		};
	};
	uint32_t bcn_interval;		/* +0x54 beacon interval, us */
	uint32_t next_bcn_tsf;		/* +0x58 */
	uint32_t wake_tsf;		/* +0x5c */
	uint32_t f60;			/* +0x60 */
	uint32_t keepalive_dl;		/* +0x64 */
	uint32_t f68;			/* +0x68 */
	uint32_t state;			/* +0x6c */
	uint32_t old_state;		/* +0x70 */
	uint32_t sleep_type;		/* +0x74 wifi_ps_type_t */
	uint8_t  listen_interval;	/* +0x78 */
	uint8_t  ap_mac[6];		/* +0x79 */
	uint8_t  f7f;			/* +0x7f */
	uint16_t aid;			/* +0x80 */
	uint8_t  miss_cnt;		/* +0x82 */
	uint8_t  cnt_tx;		/* +0x83 idle ticks since tx */
	uint8_t  cnt_bdata;		/* +0x84 */
	uint8_t  cnt_udata;		/* +0x85 */
	uint8_t  wait_time;		/* +0x86 esp_pm_config_t.wait_time */
	uint8_t  wait_tx_cnt;		/* +0x87 */
	uint8_t  wait_rx_bdata_cnt;	/* +0x88 */
	uint8_t  wait_rx_udata_cnt;	/* +0x89 */
	uint8_t  max_bcn_early_ms;	/* +0x8a */
	uint8_t  max_bcn_timeout_ms;	/* +0x8b */
	uint8_t  recv_bdata;		/* +0x8c beacon-loss threshold */
	uint8_t  recv_multi;		/* +0x8d */
	uint8_t  f8e;			/* +0x8e */
	uint8_t  f8f;			/* +0x8f */
} Pmc;					/* 0x90 bytes */

/* ---- blob public functions (pm.o exports) -------------------------- */

void pm_attach(void);
void pm_change_to_state(int state);
void pm_onBcnRx(void *desc, uint8_t *frame, uint8_t *end);
void pm_send_nullfunc(uint32_t pmflag);
int  pm_send_keepalive_pkt(void);
uint32_t pm_allow_tx(uint32_t is_tx);
int  pm_rf_is_closed(void);
int  pm_sleep_for(void *mac /* 6 bytes; blob body is a MAC compare */);
int  pm_set_addr(void *ctx, void *mac);
int  pm_add_aid(void *conninfo);
void pm_wifi_connected(void);
void pm_wifi_set_mode(void);
int  pm_scan_start(void);
void pm_scan_stop(void);
void wifi_pm_open_rf(void);
void wifi_pm_close_rf(void);
void pm_goto_rf_on(void);
void pm_goto_rf_on_set_old_state(void);
void pm_goto_rf_off(void);
void pm_goto_rf_old_state(void);
uint32_t pm_rtc_clock_cali_proc(void);
uint32_t pm_get_sleep_type(void);
void pm_set_sleep_type(int type);
uint64_t wdev_get_tsf_time(int use_hw);
void pm_reset_param(void);
void pm_reset_tdata(void);
void pm_reset_keepalive_time(void);
void pm_have_more_data_to_recv(void);
void pm_recv_all_data(void);
void pm_set_idle_timer(void);
void pm_set_the_time_of_phy_init(void);
void pm_open_rf_for_bcn_fun(void *arg);
void pm_wait_bcn_fun(void *arg);
void pm_wait_tx_rx_idle_fun(void *arg);

/* SDK-facing power API (esp_wifi.h) */
int  esp_wifi_get_pm_config(void *cfg /* esp_pm_config_t */);
int  esp_wifi_set_pm_config(void *cfg);
uint8_t esp_wifi_get_pm_recv_multi_data(void);
void esp_wifi_set_pm_recv_multi_data(uint8_t v);
int  esp_wifi_ps_lock(void);
int  esp_wifi_ps_unlock(void);
void esp_power_consumption_info(uint32_t reset);

/* rtc-cycle helpers implemented here on top of phy's usec2rtc */
uint64_t pm_get_sleep_cycles_64(uint64_t us);
void pm_set_sleep_time(uint32_t us);

#ifdef PM_HOST_TEST
/* Host-test seams */
void *pm_test_pmc(void);
uint32_t *pm_test_cur_early(void);
uint32_t *pm_test_cur_timeout(void);
uint8_t *pm_test_early_hist(void);
uint8_t *pm_test_cntnum(void);
int pm_test_idle_timer_set(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PM_H */
