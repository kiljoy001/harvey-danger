/*
 * lmac.h - Clean-room ESP8266 LMAC (replaces lmac.o from libpp.a).
 *
 * Transliterated from the xtensa disassembly and Ghidra decompiles
 * in re/inventory/b40/. lmac is the MAC state machine between pp's
 * queues and wdev's transmitter: per-access-category EDCA state,
 * TXOP frame-exchange sequences, ACK/CTS timeout and retry logic.
 * It touches hardware only through the MAC register block
 * (0x3ff20xxx) and wDev_EnableTransmit.
 *
 * Byte-exact by necessity: the AC table and the frame (PpNode /
 * control block) layouts are shared with pp/wdev by pointer.
 *
 * AC slot (36 bytes, 8 slots in our_instances):
 *   +0x00 cur PpNode*      +0x04 ac id (byte)
 *   +0x05 aifs             +0x06 cw (current exponent)
 *   +0x07 cw_min           +0x08 cw_max
 *   +0x09 qsrc (short retries)  +0x0a qlrc (long retries)
 *   +0x0b intxop           +0x0c txop_enough
 *   +0x0d trytxop          +0x0f acm
 *   +0x10 org              +0x11 state (0 idle,1 tx,2 done,3 retx,
 *                          5 success-end, 6 discard)
 *   +0x12 end_status
 *   +0x14 txop remaining (s16)  +0x16 txop delta (s16)
 *   +0x18 txop_max (u16)        +0x1a lifetime (u16)
 *   +0x1c su_cnt           +0x20 fai_cnt
 *
 * lmacConfMib (36-byte common):
 *   +0x00 u32 0x400  +0x04 u32 0x200  +0x08 u32 0x200
 *   +0x0c long-retry limit  +0x0d short-retry limit
 *   +0x0e u16 0x92a  +0x10 u16 0x92a  +0x14 u16 9
 *   +0x1c u16 basic-rate mask 0x15f  +0x1e u16 0xfff
 */

#ifndef LMAC_H
#define LMAC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t lmacConfMib[36];	/* blob COMMON */

void *GetAccess(uint32_t ac);
void lmacInit(void);
void lmacInitAc(uint8_t ac, uint8_t aifs, uint8_t cwmin, uint8_t cwmax,
		uint16_t txop);
int lmacIsIdle(uint32_t ac);
void lmacSetAcParam(uint8_t ac, uint8_t aifs, uint8_t cwmin, uint8_t cwmax,
		    uint16_t txop);
void lmacClrAc(void);
void lmacTxFrame(void *node, uint32_t ac);
void lmacRxDone(void *node);
void lmacRecycleMPDU(void *ac_ignored, void *node, uint8_t flag);
int lmacMSDUAged(void *node);
void lmacAdjustTimestamp(void *node);
void lmacProcessTxTimeout(void);
void lmacProcessTxSuccess(uint32_t ac, uint8_t arg);
void lmacProcessAckTimeout(uint32_t ac);
void lmacProcessCtsTimeout(uint32_t ac);
void lmacProcessTxError(uint32_t code, uint32_t ac);
void lmacProcessTxRtsError(uint32_t code, uint32_t ac);
void lmac_set_status(uint32_t ac, int failed);
void lmac_check_ac(void);
void lmacSetRetryLimit(uint8_t limit);
void esp_wifi_internal_set_tx_param(int32_t p1, int32_t p2, int32_t p3,
				    int8_t p4, int8_t p5);
void esp_wifi_set_11b_tx_plcp(uint8_t set_plcp, uint8_t with_long);
void dbg_display_acs(void);
void dbg_lmac_show(void);

#ifdef __cplusplus
}
#endif

#endif /* LMAC_H */
