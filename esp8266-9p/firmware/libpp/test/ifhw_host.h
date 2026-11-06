/*
 * ifhw_host.h - Host-test seam for if_hwctrl.c (IFHW_HOST_TEST only).
 * Declares the wdev/SDK symbols if_hwctrl.c references so the host
 * test can stub them. Never included in device builds.
 */

#ifndef IFHW_HOST_H
#define IFHW_HOST_H

#include <stdint.h>

extern char g_ic;

uint32_t bit_popcount(uint32_t v);
int esp_wifi_try_rate_from_high(void);
void lmacSetAcParam(uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4,
		    uint16_t p5);
void vPortETSIntrLock(void);
void vPortETSIntrUnlock(void);
void wDev_Crypto_Conf(uint32_t ifidx, uint32_t alg, uint32_t gtkflag);
void wDev_Crypto_Disable(uint32_t ifidx);
void wDev_Get_KeyEntry(uint32_t p2, uint32_t p1, void *p3, void *p4,
		       uint8_t p5, void *p6, uint8_t p7);
void wDev_Insert_KeyEntry(uint32_t alg, uint32_t ifidx, uint8_t p3,
			  void *mac, uint32_t keyidx, void *keydata,
			  uint32_t keylen);
void wDev_remove_KeyEntry(uint8_t ifidx);
void wDev_SetMacAddress(uint32_t ifidx, void *mac);
void wDev_SetRxPolicy(int policy, uint32_t ifidx, void *arg);

/* host register stand-in for 0x3ff20004 (dereferenceable) */
extern uint32_t *ifhw_host_mac_ctrl(void);

#endif /* IFHW_HOST_H */
