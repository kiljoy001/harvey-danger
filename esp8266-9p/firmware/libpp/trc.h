/* trc - clean-room replacement for the blob trc.o in
 * libpp.a (B36, stage 2). Per-station TX rate adaptation:
 * 9 records of 72 bytes, 12-byte schedule entries of four
 * (rate, retries) stages, PER-driven step-up/step-down
 * against the blob's own schedule tables (trc_tables.c).
 *
 * Decoded from trc.o disassembly + Ghidra; see
 * re/inventory/b36/STAGE2-NOTES.md for the field map and
 * the evidence for every constant below. The tick source
 * is the wdev counter register (eagle_soc.h
 * WDEV_COUNT_REG, 0x3ff20c00), read through trc_tick()
 * so host tests can script time.
 *
 * Record layout is ours; the ABI is the 20 exported
 * functions. (The blob's records are only ever reached
 * through these functions - `trcs` is a local symbol in
 * trc.o - so no other object can depend on the layout.)
 */
#ifndef TRC_H
#define TRC_H

#include <stdint.h>

typedef struct TrcRec {
	int8_t rssi_tx_new;	/* +0x00, 0x7f = unset */
	int8_t rssi_tx_avg;	/* +0x01 */
	int8_t rssi_rx_new;	/* +0x02 */
	int8_t rssi_rx_avg;	/* +0x03 */
	uint8_t f4;		/* +0x04 */
	uint8_t maxidx;		/* +0x05: B=3, G=10, N=11 */
	uint8_t badper;		/* +0x06: bad-PER streak */
	uint8_t forced_rate;	/* +0x07 */
	uint32_t flags;		/* +0x08: b0/b2 gate RSSI,
				 * b6 selects forced_rate */
	int8_t phy_arg;		/* +0x09 */
	uint8_t per_latest;	/* +0x0a */
	uint8_t per_avg;	/* +0x0b */
	uint8_t mac[6];		/* +0x0c */
	uint16_t arg12;		/* +0x12 */
	uint32_t per_num;	/* +0x14: weighted PER sum */
	uint32_t attempts;	/* +0x18 */
	uint32_t ts_lower;	/* +0x1c */
	uint32_t ts_step;	/* +0x20 */
	uint32_t ts_eval;	/* +0x24 */
	uint32_t txdone;	/* +0x28 */
	uint32_t budget;	/* +0x2c, init 500000 */
	uint32_t phytype;	/* +0x30 */
	uint8_t *sched;		/* +0x34: current entry */
	uint8_t *fallback;	/* +0x38 */
	uint8_t *tblbase;	/* +0x3c */
	void *rate2idx;	/* +0x40: fn ptr, blob ABI */
	uint8_t slot;		/* +0x44 */
	uint8_t ifidx;		/* +0x45 */
	uint8_t modeval;	/* +0x46: B=3, G=7, N=8 */
	uint8_t pad47;
} TrcRec;

void rcAttach(void);
TrcRec *rc_enable_trc(uint32_t ifidx, const void *mac,
		      uint32_t slot, uint16_t arg12);
void rc_disable_trc(uint32_t ifidx, uint32_t unused, uint32_t slot);
void rc_disable_trc_by_interface(uint32_t ifidx);
uint8_t rc_get_mask(void);
TrcRec *rc_get_sta_trc(uint32_t maskbyte);
TrcRec *rc_get_trc(uint32_t ifidx, const void *mac);
TrcRec *rc_get_trc_by_index(uint32_t idx);
TrcRec *rcGetTrc(void);
int32_t rc_only_sta_trc(void);
void trc_onDisconnect(void);
void trc_onScanStart(TrcRec *rec);
void trc_onScanDone(TrcRec *rec);
void rcUpdatePhyMode(TrcRec *rec, int8_t arg);
void rcUpdateTxDone(TrcRec *rec, const uint32_t *desc);
void rcUpdateRxDone(TrcRec *rec, const int8_t *desc);
void rcUpdateDataRxDone(void);
void rcGetSched(TrcRec *rec, uint32_t *txctl);
void rcGetRate(TrcRec *rec, uint32_t *txctl);
int rcReachRetryLimit(const uint32_t *desc);
int32_t rc_set_rate_limit_id(int8_t mode, uint32_t id,
			     uint8_t minidx, uint8_t maxidx);
uint32_t trc_NeedRTS(const void *cnx);
int phytype2mode(uint8_t phytype);
int should_update_rate(uint32_t cur, uint32_t max);

/* Tick seam: WDEV_COUNT_REG on target; the host test
 * provides its own when built with TRC_HOST_TEST. */
uint32_t trc_tick(void);

#endif
