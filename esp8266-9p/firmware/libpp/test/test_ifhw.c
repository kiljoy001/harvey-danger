/*
 * test_ifhw.c - Host tests for the clean-room if_hwctrl.c.
 *
 * Build:
 *   cc -m32 -std=c99 -Wall -Wextra -Werror -DIFHW_HOST_TEST -I. \
 *      -o /tmp/test_ifhw if_hwctrl.c test/test_ifhw.c
 *
 * Expected values hand-computed from re/inventory/b39/.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "if_hwctrl.h"
#include "trc.h"

/* ---- stub world ---------------------------------------------------- */

char g_ic[0x480];
static uint32_t mac_ctrl_reg;
static int lock_depth;
static int n_setmac, n_rxpolicy, n_crypto_disable, n_insert, n_conf;
static int last_rxpolicy = -1;
static int try_rate_ret;
static TrcRec fake_rec;
static TrcRec *enable_ret;

uint32_t *ifhw_host_mac_ctrl(void) { return &mac_ctrl_reg; }

uint32_t bit_popcount(uint32_t v)
{
	uint32_t n = 0;

	while (v != 0) {
		n += v & 1;
		v >>= 1;
	}
	return n;
}

int esp_wifi_try_rate_from_high(void) { return try_rate_ret; }
void lmacSetAcParam(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; }
void vPortETSIntrLock(void) { lock_depth++; }
void vPortETSIntrUnlock(void) { lock_depth--; }
void wDev_Crypto_Conf(uint32_t i, uint32_t a, uint32_t g)
{ (void)i; (void)a; (void)g; n_conf++; }
void wDev_Crypto_Disable(uint32_t i) { (void)i; n_crypto_disable++; }
void wDev_Get_KeyEntry(uint32_t a, uint32_t b, void *c, void *d,
		       uint8_t e, void *f, uint8_t g)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; }
static uint32_t insert_keylen_seen;
void wDev_Insert_KeyEntry(uint32_t a, uint32_t i, uint8_t p, void *m,
			  uint32_t k, void *d, uint32_t len)
{ (void)a; (void)i; (void)p; (void)m; (void)k; (void)d;
  insert_keylen_seen = len; n_insert++; }
void wDev_remove_KeyEntry(uint8_t i) { (void)i; }
void wDev_SetMacAddress(uint32_t i, void *m) { (void)i; (void)m; n_setmac++; }
void wDev_SetRxPolicy(int policy, uint32_t i, void *arg)
{ (void)i; (void)arg; last_rxpolicy = policy; n_rxpolicy++; }

int ppRegisterTxCallback(void *fn, uint32_t idx)
{ (void)fn; (void)idx; return 0; }

static uint32_t enable_slot_seen;
TrcRec *rc_enable_trc(uint32_t ifidx, const void *mac, uint32_t slot,
		      uint16_t arg12)
{ (void)ifidx; (void)mac; (void)arg12; enable_slot_seen = slot;
  return enable_ret; }
void rc_disable_trc(uint32_t ifidx, uint32_t unused, uint32_t slot)
{ (void)ifidx; (void)unused; (void)slot; }
void rc_disable_trc_by_interface(uint32_t ifidx) { (void)ifidx; }
TrcRec *rc_get_trc(uint32_t ifidx, const void *mac)
{ (void)ifidx; (void)mac; return enable_ret; }
void rcUpdatePhyMode(TrcRec *rec, int8_t arg) { (void)rec; (void)arg; }

/* ---- tests ---------------------------------------------------------- */

static int fails;

#define CHK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		fails++; \
	} \
} while (0)

int
main(void)
{
	static const uint8_t mac[6] = { 0x48, 0x3f, 0xda, 0x7e, 0x39, 0x68 };

	memset(if_ctrl, 0, sizeof(if_ctrl));
	interface_mask = 0;
	memset(g_ic, 0, sizeof(g_ic));
	mac_ctrl_reg = 0;

	/* enable/disable + mask + popcount returns */
	CHK(ic_enable_interface(0, (void *)mac, 1) == 1);
	CHK(memcmp(ic_get_addr(0), mac, 6) == 0);
	CHK(ic_interface_enabled(0) == 1);
	CHK(ic_interface_enabled(1) == 0);
	CHK(ic_enable_interface(1, (void *)mac, 2) == 2);
	CHK(ic_is_pure_sta(0) == 0);	/* opmode 1 -> NOT != 1 */
	CHK(ic_is_pure_sta(1) == 1);	/* opmode 2 -> != 1 */
	ic_set_opmode(1, 1);
	CHK(ic_is_pure_sta(1) == 0);
	CHK(ic_disable_interface(0) == 1);
	CHK(ic_interface_enabled(0) == 0);
	CHK(ic_disable_interface(0) == 1);	/* not set: no-op */

	/* alg bytes */
	ic_set_ptk_alg(0, 4);
	ic_set_gtk_alg(0, 2);
	CHK(ic_get_ptk_alg(0) == 4);
	CHK(ic_get_gtk_alg(0) == 2);
	CHK(ic_get_ptk_alg(1) == 0);

	/* set_key alg-byte logic: keyidx<6 writes gtk always,
	 * ptk only when alg==0; keyidx>=6 writes ptk only */
	ic_set_key(0, 3, 0, NULL, 0, NULL, NULL, 0);
	CHK(ic_get_ptk_alg(0) == 4);	/* untouched (alg != 0) */
	CHK(ic_get_gtk_alg(0) == 3);
	ic_set_key(0, 0, 0, NULL, 0, NULL, NULL, 0);
	CHK(ic_get_ptk_alg(0) == 0);
	ic_set_key(0, 5, 0, NULL, 6, NULL, NULL, 0);
	CHK(ic_get_ptk_alg(0) == 5);
	CHK(ic_get_gtk_alg(0) == 0);	/* untouched in >=6 branch */
	CHK(n_insert == 3 && n_conf == 3);
	/* key length (7th arg, on the stack) must be forwarded:
	 * wDev memcpy's that many key bytes (B39) */
	ic_set_key(0, 3, 0, NULL, 0, NULL, (void *)(uintptr_t)16, 1);
	CHK(insert_keylen_seen == 16);

	/* set_vif type 1: first interface sets MAC ctrl bit 31 */
	interface_mask = 0;
	mac_ctrl_reg = 0;
	n_setmac = 0;
	ic_set_vif(0, 1, (void *)mac, 1);
	CHK(n_setmac == 1);
	CHK((mac_ctrl_reg & 0x80000000u) != 0);
	/* second interface: count 2, bit untouched path */
	ic_set_vif(1, 1, (void *)mac, 1);
	CHK(interface_mask == 3);
	/* type 0 (else): last disable clears the bit */
	ic_set_vif(1, 0, NULL, 0);
	CHK(interface_mask == 1);
	CHK((mac_ctrl_reg & 0x80000000u) != 0);
	ic_set_vif(0, 0, NULL, 0);
	CHK(interface_mask == 0);
	CHK((mac_ctrl_reg & 0x80000000u) == 0);
	/* type 2: opmode only */
	ic_set_vif(0, 2, NULL, 7);
	CHK(*(uint32_t *)(ic_get_addr(0) + 8) == 7);

	/* bss_info_update: kind 1 -> rx policy 2; kind 2 + arg4 0
	 * -> clear crypto; kind 3 -> clear crypto */
	n_rxpolicy = 0; n_crypto_disable = 0;
	ic_bss_info_update(0, NULL, 1, 0);
	CHK(last_rxpolicy == 2 && n_crypto_disable == 0);
	ic_bss_info_update(0, NULL, 2, 0);
	CHK(n_crypto_disable == 1 && last_rxpolicy == 1);
	ic_bss_info_update(0, NULL, 3, 0);
	CHK(n_crypto_disable == 2);

	/* set_sta: g_ic+0x174 counter only moves for ifidx 1 */
	enable_ret = NULL;
	try_rate_ret = 0;
	ic_set_sta(1, 1, (void *)mac, 0, 0, 0);
	CHK(g_ic[0x174] == 1);
	ic_set_sta(1, 1, (void *)mac, 0, 0, 0);
	CHK(g_ic[0x174] == 2);
	ic_set_sta(0, 1, (void *)mac, 0, 0, 0);
	CHK(g_ic[0x174] == 2);
	ic_set_sta(1, 0, (void *)mac, 0, 0, 0);
	CHK(g_ic[0x174] == 1);
	CHK(lock_depth == 0);

	/* set_sta must truncate slot to a byte (blob extui):
	 * wl_cnx passes a 12-bit field, trcs[] has 9 entries */
	enable_ret = &fake_rec;
	ic_set_sta(0, 1, (void *)mac, 0x1ab, 0, 0);
	CHK(enable_slot_seen == 0xab);

	/* get_rssi: no trc record -> 0x1f; record byte+3 = 0x9c
	 * -> 0x9c - 0x60 = 60 (as signed char math on int) */
	enable_ret = NULL;
	CHK(ic_get_rssi(0, mac) == 0x1f);
	memset(&fake_rec, 0, sizeof(fake_rec));
	((uint8_t *)&fake_rec)[3] = 0x9c;
	enable_ret = &fake_rec;
	CHK(ic_get_rssi(0, mac) == (char)(0x9c - 0x60));

	if (fails == 0)
		printf("IFHW: ALL PASS\n");
	else
		printf("IFHW: %d FAILURES\n", fails);
	return fails != 0;
}
