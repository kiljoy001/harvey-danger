/*
 * if_hwctrl.c - Clean-room ESP8266 interface control
 * (if_hwctrl.o replacement). Full header comment in if_hwctrl.h.
 * Transliterated from the disassembly and Ghidra decompiles in
 * re/inventory/b39/.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "if_hwctrl.h"
#include "pp.h"		/* ppRegisterTxCallback */
#include "trc.h"		/* rc_*_trc, rcUpdatePhyMode */

#ifdef IFHW_HOST_TEST
#include "test/ifhw_host.h"
#define MAC_CTRL_REG (*ifhw_host_mac_ctrl())
#else

extern char g_ic;

extern uint32_t bit_popcount(uint32_t v);
extern int esp_wifi_try_rate_from_high(void);
extern void lmacSetAcParam(uint8_t p1, uint8_t p2, uint8_t p3,
			   uint8_t p4, uint16_t p5);
extern void vPortETSIntrLock(void);
extern void vPortETSIntrUnlock(void);
extern void wDev_Crypto_Conf(uint32_t ifidx, uint32_t alg,
			     uint32_t gtkflag);
extern void wDev_Crypto_Disable(uint32_t ifidx);
extern void wDev_Get_KeyEntry(uint32_t p2, uint32_t p1, void *p3,
			      void *p4, uint8_t p5, void *p6, uint8_t p7);
/* The blob calls this with a5/a6/a7 flowing through from
 * ic_set_key's own arguments (key MAC in a5, keyidx in a6,
 * key data in a7) and forwards its own incoming stack arg
 * (the key length) into this call's stack slot: wDev does
 * memcpy(entry, keydata, keylen). Declare all seven so the
 * compiler sets them; dropping the stack arg installs a
 * truncated key and the station goes deaf after join (B39). */
extern void wDev_Insert_KeyEntry(uint32_t alg, uint32_t ifidx,
				 uint8_t p3, void *mac, uint32_t keyidx,
				 void *keydata, uint32_t keylen);
extern void wDev_remove_KeyEntry(uint8_t ifidx);
extern void wDev_SetMacAddress(uint32_t ifidx, void *mac);
extern void wDev_SetRxPolicy(int policy, uint32_t ifidx, void *arg);

#define MAC_CTRL_REG (*(volatile uint32_t *)0x3ff20004)
#endif /* !IFHW_HOST_TEST */

/* Blob commons (tentative definitions merge with the other
 * objects' commons at link, exactly as in the blob build). */
uint8_t if_ctrl[0x20];
uint8_t interface_mask;

#define IC(i)	(if_ctrl + ((i) & 0xff) * 0x10)
#define IC_OPMODE(i)	(*(uint32_t *)(IC(i) + 8))
#define IC_NO_DISTURB	(*(uint8_t *)((char *)&g_ic + 0x160))
#define IC_STA_COUNT	(*(uint8_t *)((char *)&g_ic + 0x174))

void *
ic_get_addr(uint32_t ifidx)
{
	return IC(ifidx);
}

void
ic_set_opmode(uint32_t ifidx, uint32_t mode)
{
	IC_OPMODE(ifidx) = mode;
}

uint32_t
ic_enable_interface(uint32_t ifidx, void *mac, uint32_t opmode)
{
	ifidx &= 0xff;
	memcpy(IC(ifidx), mac, 6);
	IC_OPMODE(ifidx) = opmode;
	interface_mask |= (uint8_t)(1u << (ifidx & 0x1f));
	return (uint8_t)bit_popcount(interface_mask);
}

uint32_t
ic_disable_interface(uint32_t ifidx)
{
	if ((interface_mask & (uint8_t)(1u << (ifidx & 0x1f))) != 0) {
		interface_mask &= (uint8_t)~(1u << (ifidx & 0x1f));
		rc_disable_trc_by_interface(ifidx);
	}
	return (uint8_t)bit_popcount(interface_mask);
}

uint32_t
ic_interface_enabled(uint32_t ifidx)
{
	return (interface_mask >> (ifidx & 0x1f)) & 1;
}

int
ic_is_pure_sta(uint32_t ifidx)
{
	return IC_OPMODE(ifidx) != 1;
}

uint32_t
ic_get_ptk_alg(uint32_t ifidx)
{
	return IC(ifidx)[0xc];
}

uint32_t
ic_get_gtk_alg(uint32_t ifidx)
{
	return IC(ifidx)[0xd];
}

void
ic_set_ptk_alg(uint32_t ifidx, uint8_t alg)
{
	IC(ifidx)[0xc] = alg;
}

void
ic_set_gtk_alg(uint32_t ifidx, uint8_t alg)
{
	IC(ifidx)[0xd] = alg;
}

/* Blob-local helper (FUN_00010010): clear crypto state for an
 * interface unless the ic no-disturb byte is set. */
static void
ic_clear_crypto(uint32_t ifidx)
{
	if (IC_NO_DISTURB == 0) {
		IC(ifidx)[0xc] = 0;
		IC(ifidx)[0xd] = 0;
		wDev_Crypto_Disable(ifidx);
	}
	wDev_SetRxPolicy(1, ifidx, 0);
}

void
ic_set_vif(uint32_t ifidx, int type, void *mac, uint32_t opmode)
{
	ifidx &= 0xff;
	if (type == 2) {
		IC_OPMODE(ifidx) = opmode;
	} else if (type == 1) {
		wDev_SetMacAddress(ifidx, mac);
		if (ic_enable_interface(ifidx, mac, opmode) == 1)
			MAC_CTRL_REG |= 0x80000000u;
		ic_clear_crypto(ifidx);
	} else {
		wDev_SetRxPolicy(0, ifidx, 0);
		if (ic_disable_interface(ifidx) == 0)
			MAC_CTRL_REG &= 0x7fffffffu;
	}
}

void
ic_set_sta(uint32_t ifidx, int on, void *mac, uint32_t slot,
	   uint16_t arg12, uint32_t field6)
{
	/* Blob truncates slot to a byte in its prologue (extui
	 * a4, a5, 0, 8); wl_cnx packs flags above the low byte,
	 * and rc_enable_trc indexes trcs[9] with it. */
	slot &= 0xff;
	ifidx &= 0xff;
	if (on == 0) {
		rc_disable_trc(ifidx, 0, slot);
		if (ifidx != 1)
			return;
		vPortETSIntrLock();
		if (IC_STA_COUNT != 0)
			IC_STA_COUNT--;
	} else {
		TrcRec *rec = rc_enable_trc(ifidx, mac, slot, arg12);

		if (rec != NULL) {
			*(uint32_t *)((uint8_t *)rec + 0x30) = field6;
			vPortETSIntrLock();
			rcUpdatePhyMode(rec,
					esp_wifi_try_rate_from_high() ?
					0x21 : 0);
			vPortETSIntrUnlock();
		}
		if (ifidx != 1)
			return;
		vPortETSIntrLock();
		IC_STA_COUNT++;
	}
	vPortETSIntrUnlock();
}

void
ic_bss_info_update(uint32_t ifidx, void *arg2, int16_t kind, int arg4)
{
	if (kind == 2) {
		if (arg4 == 0) {
			ic_clear_crypto(ifidx);
			return;
		}
	} else if (kind != 1) {
		ic_clear_crypto(ifidx);
		return;
	}
	wDev_SetRxPolicy(2, ifidx, arg2);
}

void
ic_set_key(uint32_t ifidx, uint8_t alg, uint8_t p3, void *p4,
	   uint8_t keyidx, void *p6, void *p7, uint8_t gtkflag)
{
	(void)p4; (void)p6; (void)p7;
	ifidx &= 0xff;
	if (keyidx < 6) {
		if (alg == 0)
			IC(ifidx)[0xc] = alg;
		IC(ifidx)[0xd] = alg;
	} else {
		IC(ifidx)[0xc] = alg;
	}
	wDev_Insert_KeyEntry(alg, ifidx, p3, p4, keyidx, p6,
			     (uint32_t)(uintptr_t)p7);
	wDev_Crypto_Conf(ifidx, alg, gtkflag);
}

void
ic_get_key(uint32_t p1, uint32_t p2, void *p3, void *p4,
	   uint8_t p5, void *p6, uint8_t p7)
{
	wDev_Get_KeyEntry(p2, p1, p3, p4, p5, p6, p7);
}

void
ic_remove_key(uint8_t ifidx)
{
	wDev_remove_KeyEntry(ifidx);
}

char
ic_get_rssi(uint32_t ifidx, const void *mac)
{
	TrcRec *rec = rc_get_trc(ifidx, mac);

	if (rec != NULL)
		return (char)(*(char *)((uint8_t *)rec + 3) - 0x60);
	return 0x1f;
}

void
ic_register_tx_cb(void *fn, uint8_t idx)
{
	ppRegisterTxCallback(fn, idx);
}

void
ic_set_ac_param(uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4,
		uint16_t p5)
{
	lmacSetAcParam(p1, p2, p3, p4, p5);
}
