/*
 * pm_host.h - Host-test seam for pm.c (PM_HOST_TEST builds only).
 *
 * Declares every blob/SDK symbol pm.c references so the host test can
 * provide controllable stubs. Never included in device builds.
 */

#ifndef PM_HOST_H
#define PM_HOST_H

#include <stdint.h>
#include <stddef.h>

/* blob globals (defined by the host test) */
extern char g_ic;
extern char *g_wifi_nvs;
extern char *sniffer_enable;
extern uint64_t WdevTimOffSet;
extern uint8_t pend_flag_noise_check;

/* substrate SDK symbols (stubbed by the host test) */
void __wifi_task_resume_all(void);
void __wifi_task_suspend_all(void);
void clockgate_watchdog(int on);
void *ic_get_addr(int idx);
uint32_t esp_log_early_timestamp(void);
uint64_t esp_timer_get_time(void);
void esp_log_write(int level, const char *tag, const char *fmt, ...);
void esp_sleep_lock(void);
void esp_sleep_unlock(void);
int ets_printf(const char *fmt, ...);
void *ieee80211_getmgtframe(int *len, int size, int arg3);
void ieee80211_send_setup(void *conn, void *frame, int len,
			  int waitmask, void *addr, void *dst, void *bssid);
void noise_check_loop(int on, int second);
void os_timer_arm(void *timer, uint32_t ms, int repeat);
void os_timer_arm_us(void *timer, uint32_t us, int repeat);
void os_timer_disarm(void *timer);
void os_timer_setfn(void *timer, void (*fn)(void *), void *arg);
void phy_close_rf(void);
void phy_open_rf(void);
void pm_set_sleep_cycles(uint32_t cycles);
uint32_t pm_usec2rtc(uint32_t usec, uint32_t period);
uint64_t pm_usec2rtc_64(uint64_t usec, uint32_t period);
void ppTxPkt(void *desc);
void *rc_get_trc(uint32_t ifidx, const void *mac);
uint32_t ppCheckTxIdle(uint32_t lockflag);
void ppProcessWaitQ(void);
int ppRegisterTxCallback(void *fn, uint32_t idx);
int wifi_nvs_get_sta_listen_interval(void);

/* host-only hooks implemented by the host test */
uint32_t pm_host_now32(void);
uint32_t pm_host_wdev_count(void);
void pm_host_phy_init_cali(void *pmcblock);

#endif /* PM_HOST_H */
