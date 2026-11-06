/* rate_control - clean-room replacement for the blob
 * rate_control.o in libpp.a (B31).
 *
 * The blob object is a rate table plus airtime arithmetic:
 * 32 rate records of 8 bytes (our_controls). Record layout
 * as decoded from the blob getters:
 *   +0  u32  duration factor (fixed point; airtime uses
 *            (bits * factor + 0x3ffff) >> 18)
 *   +4  u8   RTS rate index (RC_GetRtsRate)
 *   +5  u8   ACK rate index (RC_GetAckRate; also indexes
 *            the 32-entry ACK-time table in RC_GetAckTime)
 *   +6  u8   preamble class (short/long variants)
 * RC_SetBasicRate fills the table when the supported-rate
 * mask is complete (mask & 0x15f) == 0x15f, and otherwise
 * only retunes the RTS-rate byte of the OFDM records from
 * an ERP flag - exactly the blob's two branches. The
 * default table below was reconstructed from the blob's
 * RC_SetBasicRate store sequence (Ghidra + literal pool,
 * re/B31) and is verified by the host test through the
 * getter formulas.
 */
#ifndef RATE_CONTROL_H
#define RATE_CONTROL_H

#include <stdint.h>

uint32_t RC_GetAckRate(uint32_t rate);
uint32_t RC_GetRtsRate(uint32_t rate);
uint16_t RC_GetAckTime(uint32_t rate);
uint32_t RC_GetCtsTime(uint32_t rate, uint32_t len, uint32_t extra);
uint32_t RC_GetBlockAckTime(uint32_t rate);
int RC_SetBasicRate(uint32_t supported_mask, int8_t erp);

/* Test/debug view of the table (32 records x 8 bytes). */
extern uint8_t our_controls[256];

#endif
