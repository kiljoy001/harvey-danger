/* blob_glue_stubs - NONOS libmain glue that the RTOS blobs do
 * not provide under these names. The blobtest never calls the
 * NONOS Wi-Fi/lwip APIs (the RTOS blob stack replaces them), so
 * these exist only to close the link. If boot shows libmain
 * actually reaching one of them, that is a finding, not a
 * silent success: several are on the system-init path and are
 * watched in the B17 boot log analysis.
 */
#include <stdint.h>

#define STUB0(name) int name(void) { return 0; }

STUB0(dhcp_cleanup)
STUB0(dhcps_start)
STUB0(dhcps_stop)
STUB0(dhcp_start)
STUB0(dhcp_stop)
STUB0(espconn_init)
STUB0(etharp_output)
STUB0(ethernet_input)
STUB0(ieee80211_chan_in_regdomain)
STUB0(lwip_init)
STUB0(netif_add)
STUB0(netif_remove)
STUB0(netif_set_addr)
STUB0(netif_set_default)
STUB0(pm_clear_gpio_wakeup_pin)
STUB0(pm_is_open)
STUB0(pm_is_waked)
STUB0(pm_post)
STUB0(pm_set_gpio_wakeup_pin)
STUB0(pm_set_sleep_type_from_upper)
STUB0(pp_soft_wdt_feed)
STUB0(pp_soft_wdt_init)
STUB0(sys_check_timeouts)
STUB0(TestStaFreqCalValInput)
STUB0(fpm_allow_tx)
STUB0(fpm_attach)
STUB0(fpm_do_wakeup)
STUB0(get_fpm_auto_sleep_flag)
STUB0(wDevDisableRx)
STUB0(wDevEnableRx)
STUB0(wifi_fpm_do_sleep)
STUB0(wifi_fpm_open)
STUB0(wifi_fpm_set_sleep_type)
extern void ets_delay_us(uint32_t us);
void os_delay_us(uint32_t us) { ets_delay_us(us); }

/* Data symbols libmain expects to find. */
uint8_t netif_default[256];
uint8_t ethbroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
uint8_t sta_con_timer[64];
uint8_t no_ap_found_index;

STUB0(pbuf_free)
