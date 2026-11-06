/*
 * if_hwctrl.h - Clean-room interface control (replaces if_hwctrl.o
 * from libpp.a).
 *
 * Transliterated from the xtensa disassembly and Ghidra decompiles
 * in re/inventory/b39/. The per-interface state is the blob's
 * if_ctrl commons: 2 interfaces x 16 bytes (+0 addr[6], +8 opmode,
 * +0x0c ptk_alg, +0x0d gtk_alg), plus the interface_mask byte.
 * Crypto key programming itself stays in wdev.o (wDev_*_KeyEntry)
 * until that blob is replaced.
 */

#ifndef IF_HWCTRL_H
#define IF_HWCTRL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t if_ctrl[0x20];	/* blob COMMON, 2 x 16-byte entries */
extern uint8_t interface_mask;	/* blob COMMON */

void *ic_get_addr(uint32_t ifidx);
void ic_set_opmode(uint32_t ifidx, uint32_t mode);
uint32_t ic_enable_interface(uint32_t ifidx, void *mac, uint32_t opmode);
uint32_t ic_disable_interface(uint32_t ifidx);
uint32_t ic_interface_enabled(uint32_t ifidx);
int ic_is_pure_sta(uint32_t ifidx);
uint32_t ic_get_ptk_alg(uint32_t ifidx);
uint32_t ic_get_gtk_alg(uint32_t ifidx);
void ic_set_ptk_alg(uint32_t ifidx, uint8_t alg);
void ic_set_gtk_alg(uint32_t ifidx, uint8_t alg);
void ic_set_vif(uint32_t ifidx, int type, void *mac, uint32_t opmode);
void ic_set_sta(uint32_t ifidx, int on, void *mac, uint32_t slot,
		uint16_t arg12, uint32_t field6);
void ic_bss_info_update(uint32_t ifidx, void *arg2, int16_t kind,
			int arg4);
void ic_set_key(uint32_t ifidx, uint8_t alg, uint8_t p3, void *p4,
		uint8_t keyidx, void *p6, void *p7, uint8_t gtkflag);
void ic_get_key(uint32_t p1, uint32_t p2, void *p3, void *p4,
		uint8_t p5, void *p6, uint8_t p7);
void ic_remove_key(uint8_t ifidx);
char ic_get_rssi(uint32_t ifidx, const void *mac);
void ic_register_tx_cb(void *fn, uint8_t idx);
void ic_set_ac_param(uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4,
		     uint16_t p5);

#ifdef __cplusplus
}
#endif

#endif /* IF_HWCTRL_H */
