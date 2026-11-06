/* trc - see trc.h. Transliterated from the trc.o
 * disassembly (re/inventory/b36); every constant below
 * traces to that dump. */
#include "trc.h"
#include "trc_tables.h"

#include <string.h>

/* Blob-owned globals we read at the blob's own offsets
 * (same externs trc.o references). */
extern char g_ic;		/* net80211 common block */
extern char lmacConfMib;	/* lmac MIB block */
extern uint8_t sig_max_rate2;
uint32_t bit_popcount(uint32_t v);
void esp_log_write(int level, const char *tag,
		   const char *fmt, ...);

#define GIC(off) (((uint8_t *)&g_ic)[(off)])
#define MIB8(off) (((uint8_t *)&lmacConfMib)[(off)])
#define MIB16(off) (*(uint16_t *)((uint8_t *)&lmacConfMib + (off)))

static TrcRec trcs[9];
static uint8_t trc_mask;
static uint8_t interface_trc_mask[2];
static uint32_t updateCounter;

#ifndef TRC_HOST_TEST
uint32_t
trc_tick(void)
{
	return *(volatile uint32_t *)0x3ff20c00u; /* WDEV_COUNT_REG */
}
#endif

/* The blob's wraparound idiom: elapsed = now - then,
 * minus 1 more when now < then. Replicated literally. */
static uint32_t
elapsed_wrap(uint32_t now, uint32_t then)
{
	uint32_t d = now - then;
	if (now < then)
		d -= 1;
	return d;
}

int
phytype2mode(uint8_t phytype)
{
	if (phytype == 1)
		return 1;
	if (phytype != 0 && phytype < 4)
		return 2;
	return 0;
}

static uint8_t
rc11n_rate2schedidx(uint32_t rate)
{
	uint32_t r = rate & 0xFFu;

	if (r == 17)
		return 7;
	if (r > 17) {
		if (r == 21)
			return 3;
		if (r > 21) {
			if (r == 23)
				return 1;
			if (r < 23)
				return 2;
			if (r == 31)
				return 0;
			return (uint8_t)(24 - (int)r);
		}
		if (r == 19)
			return 5;
		if (r == 18)
			return 6;
		return 4;
	}
	if (r == 2)
		return 9;
	if (r >= 3) {
		if (r == 6)
			return 9;
		if (r == 16)
			return 7;
		if (r == 5)
			return 10;
		return (uint8_t)(24 - (int)r);
	}
	if (r == 0)
		return 11;
	if (r == 1)
		return 10;
	return (uint8_t)(24 - (int)r);
}

void
rcAttach(void)
{
	int i;

	trc_mask = 0;
	interface_trc_mask[0] = 0;
	interface_trc_mask[1] = 0;
	for (i = 0; i < 12; i++)
		rc11n_sched[i][10] = (uint8_t)i;
	for (i = 0; i < 11; i++)
		rc11g_sched[i][10] = (uint8_t)i;
	for (i = 0; i < 4; i++)
		rc11b_sched[i][10] = (uint8_t)i;
}

TrcRec *
rc_enable_trc(uint32_t ifidx, const void *mac, uint32_t slot,
	      uint16_t arg12)
{
	TrcRec *rec = &trcs[slot];
	uint8_t bit = (uint8_t)(1u << (slot & 0x1F));

	if ((trc_mask & bit) != 0 &&
	    (interface_trc_mask[ifidx & 0xFF] & bit) != 0 &&
	    memcmp(rec->mac, mac, 6) != 0) {
		esp_log_write(1, "wifi", "add mismatch");
		return NULL;
	}
	interface_trc_mask[ifidx & 0xFF] |= bit;
	trc_mask |= bit;
	memset(rec, 0, sizeof(*rec));
	rec->arg12 = arg12;
	rec->ifidx = (uint8_t)ifidx;
	rec->slot = (uint8_t)slot;
	memcpy(rec->mac, mac, 6);
	return rec;
}

void
rc_disable_trc(uint32_t ifidx, uint32_t unused, uint32_t slot)
{
	uint8_t bit = (uint8_t)(1u << (slot & 0x1F));

	(void)unused;
	if ((interface_trc_mask[ifidx & 0xFF] & bit) != 0) {
		interface_trc_mask[ifidx & 0xFF] &= (uint8_t)~bit;
		trc_mask &= (uint8_t)~bit;
	}
}

void
rc_disable_trc_by_interface(uint32_t ifidx)
{
	trc_mask ^= interface_trc_mask[ifidx & 0xFF];
	interface_trc_mask[ifidx & 0xFF] = 0;
}

uint8_t
rc_get_mask(void)
{
	return trc_mask;
}

TrcRec *
rc_get_sta_trc(uint32_t maskbyte)
{
	uint8_t m = (uint8_t)maskbyte;

	if (bit_popcount(m) != 1)
		return NULL;
	return &trcs[31 - __builtin_clz((unsigned)(-(int32_t)m & m))];
}

TrcRec *
rc_get_trc(uint32_t ifidx, const void *mac)
{
	uint32_t v = interface_trc_mask[ifidx & 0xFF];

	for (;;) {
		uint32_t low = -(int32_t)v & v;
		uint32_t idx;

		if (low == 0)
			return NULL;
		idx = 31 - __builtin_clz((unsigned)low);
		if (memcmp(trcs[idx].mac, mac, 6) == 0)
			return &trcs[idx];
		v &= ~(1u << idx);
	}
}

TrcRec *
rc_get_trc_by_index(uint32_t idx)
{
	if ((trc_mask & (1u << (idx & 0x1F))) == 0)
		return NULL;
	return &trcs[idx & 0xFF];
}

TrcRec *
rcGetTrc(void)
{
	return trcs;
}

int32_t
rc_only_sta_trc(void)
{
	if (bit_popcount(trc_mask) != 1)
		return -1;
	return interface_trc_mask[0] == 0 ? 1 : 0;
}

void
trc_onDisconnect(void)
{
}

void
trc_onScanStart(TrcRec *rec)
{
	if (rec != NULL)
		rec->flags |= 5;
}

void
trc_onScanDone(TrcRec *rec)
{
	if (rec != NULL)
		rec->flags &= ~5u;
}

int32_t
rc_set_rate_limit_id(int8_t mode, uint32_t id,
		     uint8_t minidx, uint8_t maxidx)
{
	uint32_t pair;

	id &= 0xFF;
	if (id >= 2)
		return -1;
	if (mode == 1) {
		pair = id + 2;
		if (minidx == 0xFF && maxidx == 0xFF) {
			user_rc_limit[pair * 2] = 0;
			user_rc_limit[pair * 2 + 1] = 10;
			return 0;
		}
		if (minidx <= maxidx && maxidx < 0x0B) {
			user_rc_limit[pair * 2] = minidx;
			user_rc_limit[pair * 2 + 1] = maxidx;
			return 0;
		}
		return -1;
	}
	if (mode == 0) {
		pair = id;
		if (minidx == 0xFF && maxidx == 0xFF) {
			user_rc_limit[pair * 2] = 0;
			user_rc_limit[pair * 2 + 1] = 3;
			return 0;
		}
		if (minidx <= maxidx && maxidx < 4) {
			user_rc_limit[pair * 2] = minidx;
			user_rc_limit[pair * 2 + 1] = maxidx;
			return 0;
		}
		return -1;
	}
	if (mode != 2)
		return -2;
	pair = id + 4;
	if (minidx == 0xFF && maxidx == 0xFF) {
		user_rc_limit[pair * 2] = 0;
		user_rc_limit[pair * 2 + 1] = 11;
		return 0;
	}
	if (minidx <= maxidx && maxidx < 0x0C) {
		user_rc_limit[pair * 2] = minidx;
		user_rc_limit[pair * 2 + 1] = maxidx;
		return 0;
	}
	return -1;
}

static const uint8_t rate_order[32] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08,
	0x15, 0x0f, 0x09, 0x04, 0x17, 0x12, 0x0c, 0x07,
	0x05, 0x0a, 0x0d, 0x10, 0x13, 0x16, 0x19, 0x1a,
	0x06, 0x0b, 0x0e, 0x11, 0x14, 0x18, 0x1a, 0x1b
};

int
should_update_rate(uint32_t cur, uint32_t max)
{
	return rate_order[max & 0xFF] < rate_order[cur & 0xFF];
}

int
rcReachRetryLimit(const uint32_t *desc)
{
	uint32_t retries = (desc[2] >> 4) & 0xF;

	if ((desc[0] & 0x08000000u) != 0 && retries >= 5)
		return 1;
	return MIB8(0x0C) <= retries;
}

uint32_t
trc_NeedRTS(const void *cnx)
{
	const uint8_t *p = (const uint8_t *)cnx;
	uint8_t rate = *(const uint8_t *)(*(const uint8_t *const *)(p + 0x28) + 0x0C);
	uint32_t len = (uint32_t)(*(const uint16_t *)(p + 0x18)) +
		       *(const uint16_t *)(p + 0x1A);
	uint32_t thresh;

	switch (rate) {
	case 0:
		return len < 0x272 ? 0 : 1;
	case 1:
	case 5:
		thresh = 1000;
		break;
	case 10:
	case 17:
	case 25:
		thresh = 0x460;
		break;
	case 15:
		thresh = 0x41A;
		break;
	case 11:
	case 24:
		thresh = 700;
		break;
	default:
		thresh = MIB16(0x0E);
		break;
	}
	return len <= thresh ? 0 : 1;
}

/* ---- RSSI margin (blob .text.rssi_margin) ----
 * Per-rate leaf: t = (rssi - K1) mod 256, unsigned;
 * if t > LIM the margin is 0; else
 * min((3 * ((rssi - K2) >> 1)) mod 256, 32) with the
 * shift arithmetic on the signed difference. Rates
 * outside the switch return min(a4in, 32) - in the
 * blob a4 is a live register at entry (the caller's
 * PER quality); we pass it explicitly. The rate sets
 * below are the full dispatch tree, traced branch by
 * branch from the disassembly. */
static uint32_t
rssi_margin(uint32_t rssi, uint32_t rate, uint32_t a4in)
{
	uint32_t k1, k2, lim, t;
	int32_t v;

	rssi &= 0xFF;
	switch (rate & 0xFF) {
	case 23: case 31: k1 = 34; k2 = 33; lim = 220; break;
	case 12: case 22: case 30: k1 = 30; k2 = 29; lim = 224; break;
	case 8: case 21: case 29: k1 = 27; k2 = 26; lim = 227; break;
	case 13: case 20: case 28: k1 = 22; k2 = 21; lim = 232; break;
	case 27: k1 = 17; k2 = 16; lim = 237; break;
	case 3: case 7: k1 = 15; k2 = 14; lim = 239; break;
	case 14: case 26: k1 = 14; k2 = 13; lim = 240; break;
	case 2: case 6: case 10: case 17: case 18: case 19:
	case 25: k1 = 12; k2 = 11; lim = 242; break;
	case 1: case 5: k1 = 11; k2 = 10; lim = 243; break;
	case 15: k1 = 10; k2 = 9; lim = 244; break;
	case 9: case 11: case 16: case 24: k1 = 9; k2 = 8; lim = 245; break;
	default:
		return a4in < 32 ? a4in : 32;
	}
	t = (rssi - k1) & 0xFF;
	if (t > lim)
		return 0;
	v = ((int32_t)rssi - (int32_t)k2) >> 1;
	t = (uint32_t)(3 * v) & 0xFF;
	return t < 32 ? t : 32;
}

/* Step the record toward a robust entry (blob
 * rcLowerSched / .text.rssi_margin-named body in
 * FUN_00010314): reset PER state, pick a target index
 * from RSSI or the limit table, step one entry or
 * jump, stamp ts_lower. */
static void
rcLowerSched(TrcRec *rec)
{
	uint32_t target;

	rec->sched[11] = 0;
	rec->per_avg = 0;
	rec->budget = 500000;
	rec->attempts = 0;
	rec->per_num = 0;
	rec->txdone = 0;
	rec->per_latest = 0;
	rec->badper = 0;
	if ((GIC(0x165) & (1u << (rec->ifidx & 0x1F))) == 0) {
		if (rec->rssi_rx_new < 26 && rec->rssi_rx_avg < 26)
			target = rec->maxidx;
		else
			target = rec->modeval;
	} else {
		target = user_rc_limit[(phytype2mode((uint8_t)rec->phytype) * 2 +
					rec->ifidx) * 2 + 1];
	}
	if (rec->sched[10] < target)
		rec->sched += 12;
	else
		rec->sched = rec->tblbase + target * 12;
	rec->ts_lower = trc_tick();
}

/* PER accounting per transmitted frame (blob
 * rcTxUpdatePer): n is retries consumed (success) or
 * the entry's total budget (failure). */
static void
rcTxUpdatePer(TrcRec *rec, uint32_t n)
{
	uint8_t e1 = rec->sched[1];

	rec->attempts++;
	rec->per_num += (e1 < n ? 2 : 1) + n;
	if (n >= 6)
		rec->badper++;
	else if (n <= 2)
		rec->badper = 0;
	if (rec->badper > 10)
		rcLowerSched(rec);
}

void
rcUpdateTxDone(TrcRec *rec, const uint32_t *desc)
{
	uint32_t n;

	if (rec == NULL || rec->sched == NULL)
		return;
	if ((uint8_t *)rec->sched != (const uint8_t *)(uintptr_t)desc[6])
		return;
	rec->txdone++;
	if ((desc[1] & 0x03FC0000u) == 0x00040000u) {
		if ((rec->flags & 4) == 0) {
			int8_t c1 = (int8_t)((desc[3] >> 8) & 0xFF);

			if (c1 != 0x7f) {
				int8_t avg = 0;

				if (rec->rssi_tx_new != 0x7f)
					avg = (int8_t)((rec->rssi_tx_new + c1) >> 1);
				rec->rssi_tx_new = c1;
				if (rec->rssi_tx_avg == 0x7f) {
					rec->rssi_tx_avg = avg;
				} else {
					int32_t v = rec->rssi_tx_avg * 3 + avg;

					if (v < 0)
						v += 3;
					rec->rssi_tx_avg = (int8_t)(v >> 2);
				}
			}
		}
		n = (desc[2] >> 4) & 0xF;
	} else if ((desc[1] & 0x03F80000u) == 0x00080000u) {
		n = rec->sched[8];
	} else {
		return;
	}
	rcTxUpdatePer(rec, n);
}

void
rcUpdateRxDone(TrcRec *rec, const int8_t *desc)
{
	int8_t c, avg;

	if (rec == NULL || (rec->flags & 1) != 0)
		return;
	c = desc[0];
	if (rec->rssi_rx_new == 0x7f) {
		rec->rssi_rx_new = c;
		avg = 0;
	} else {
		avg = (int8_t)((rec->rssi_rx_new + c) >> 1);
		rec->rssi_rx_new = c;
		c = avg;
	}
	if (rec->rssi_rx_avg != 0x7f) {
		int32_t v = rec->rssi_rx_avg * 3 + avg;
		if (v < 0)
			v += 3;
		rec->rssi_rx_avg = (int8_t)(v >> 2);
		return;
	}
	rec->rssi_rx_avg = c;
}

void
rcUpdateDataRxDone(void)
{
}

void
rcUpdatePhyMode(TrcRec *rec, int8_t arg)
{
	uint8_t *tbl;
	uint32_t start, idx;
	int mode;

	rec->phy_arg = arg;
	switch (rec->phytype) {
	case 1:	/* G table */
		rec->f4 = 0;
		rec->maxidx = 10;
		rec->rate2idx = (void *)rc11g_rate2schedidx;
		rec->tblbase = &rc11g_sched[0][0];
		rec->modeval = 7;
		start = 10;
		if (arg > 11) {
			start = 5;
			if (arg > 16) {
				start = 3;
				if (arg > 21) {
					start = 0;
					if (arg < 30)
						start = 2;
				}
			}
		}
		tbl = &rc11g_sched[0][0];
		mode = 1;
		break;
	case 0:	/* B table, pair-clamped start */
		idx = user_rc_limit[(phytype2mode(0) * 2 + rec->ifidx) * 2];
		if (idx < 4) {
			idx = user_rc_limit[(phytype2mode(0) * 2 + rec->ifidx) * 2 + 1];
			if (idx > 2)
				idx = 3;
		}
		rec->tblbase = &rc11b_sched[0][0];
		rec->f4 = 0;
		rec->maxidx = 3;
		rec->modeval = 3;
		rec->sched = &rc11b_sched[0][0] + idx * 12;
		rec->rate2idx = (void *)rc11b_rate2schedidx;
		rec->fallback = &rc11b_sched[0][0];
		goto init;
	default:	/* B table (as case 0 minus pair clamp) */
		rec->f4 = 0;
		rec->tblbase = &rc11b_sched[0][0];
		rec->maxidx = 3;
		rec->modeval = 3;
		rec->sched = &rc11b_sched[0][0];
		rec->rate2idx = (void *)rc11b_rate2schedidx;
		rec->fallback = &rc11b_sched[0][0];
		goto init;
	case 2:
	case 3:	/* N table */
		start = rec->phytype == 2 ? 1 : 0;
		rec->maxidx = 11;
		rec->rate2idx = (void *)rc11n_rate2schedidx;
		rec->modeval = 8;
		rec->tblbase = &rc11n_sched[0][0];
		rec->f4 = (uint8_t)start;
		if (arg < 12)
			start = 11;
		else if (arg < 17)
			start = 7;
		else if (arg < 22)
			start = 5;
		else if (arg < 27)
			start = 4;
		else if (arg < 30)
			start = 3;
		else if (arg < 34)
			start = 2;
		tbl = &rc11n_sched[0][0];
		mode = (int)rec->phytype;
		break;
	}
	{
		const uint8_t *pair =
			&user_rc_limit[(phytype2mode((uint8_t)mode) * 2 +
					rec->ifidx) * 2];

		idx = pair[0];
		if (idx <= start) {
			idx = pair[1];
			if (idx < start)
				start = idx;
			idx = start;
		}
		rec->sched = tbl + idx * 12;
		rec->fallback = &rc11g_sched[0][0];
	}
init:
	rec->sched[11] = 0;
	rec->budget = 500000;
	rec->attempts = 0;
	rec->per_num = 0;
	rec->txdone = 0;
	rec->per_latest = 0;
	rec->per_avg = 0;
	rec->badper = 0;
	rec->rssi_tx_new = 0x7f;
	rec->rssi_tx_avg = 0x7f;
	rec->rssi_rx_new = 0x7f;
	rec->rssi_rx_avg = 0x7f;
}

/* Shared tail of rcGetSched/rcGetRate: pick the byte
 * that goes into txctl[3] (byte offset 12). */
static void
pick_rate(TrcRec *rec, uint32_t *txctl, uint8_t *sched, uint8_t rate)
{
	if (rec != NULL && (rec->flags & 0x40) != 0 &&
	    (txctl[0] & 8) != 0)
		rate = rec->forced_rate;
	else if ((GIC(0x161) & (1u << ((txctl[1] >> 1) & 1))) == 0)
		rate = sched != NULL ? sched[0] : rate;
	else
		rate = GIC(0x162);
	((uint8_t *)txctl)[12] = rate;
	if ((txctl[1] & 2) == 0 &&
	    should_update_rate(rate, sig_max_rate2))
		((uint8_t *)txctl)[12] = sig_max_rate2;
}

void
rcGetSched(TrcRec *rec, uint32_t *txctl)
{
	uint8_t *sched;
	uint32_t now, quality = 0;

	if (rec == NULL || rec->sched == NULL) {
		if ((txctl[0] & 0x02020002u) == 0) {
			if (rec != NULL && rec->phytype != 0)
				sched = &rc11g_sched[0][0];
			else if (rec != NULL)
				sched = &rc11b_sched[0][0];
			else
				sched = &rc11g_sched[0][0];
		} else {
			sched = &rc11b_sched[0][0];
		}
		goto out;
	}
	if ((txctl[0] & 2) != 0) {
		sched = rec->fallback;
		goto out;
	}
	if ((txctl[0] & 8) == 0) {
		if ((txctl[0] & 0x00200800u) == 0 || rec->phytype == 0)
			sched = rec->fallback;
		else
			sched = &basic_ofdm_sched[0][0];
		goto out;
	}
	/* evaluation path (txctl flag 8) */
	now = trc_tick();
	if (rec->per_num != 0 &&
	    (rec->txdone > 99 || elapsed_wrap(now, rec->ts_eval) > 99999)) {
		uint8_t *entry = rec->sched;
		uint32_t elapsed20, margin;
		int bad;

		rec->ts_eval = now;
		updateCounter++;
		quality = ((rec->attempts << 7) / rec->per_num) & 0xFF;
		rec->per_latest = (uint8_t)quality;
		if (rec->per_avg == 0) {
			uint32_t v = ((uint32_t)entry[9] * 3 + 128) >> 2;

			v &= 0xFF;
			if (v < quality)
				rec->per_avg = (uint8_t)((quality + v) >> 1);
			else
				rec->per_avg = (uint8_t)v;
		} else {
			rec->per_avg = (uint8_t)((quality + rec->per_avg) >> 1);
		}
		elapsed20 = elapsed_wrap(now, rec->ts_step);
		margin = (rssi_margin(rec->rssi_tx_avg, entry[0], quality) >> 1) & 0xFF;
		if (entry[9] == 0)
			bad = (90 - (int)margin < (int)quality);
		else if (entry[9] < 65)
			bad = (100 - (int)margin < (int)quality);
		else
			bad = (110 - (int)margin < (int)quality) &&
			      ((int)entry[9] + 2 - (int)margin < (int)quality);
		if (bad && rec->budget >= elapsed20)
			bad = 0;
		if (bad) {
			uint32_t limbyte;

			entry[11] = 0;
			rec->budget = 500000;
			rec->attempts = 0;
			rec->per_num = 0;
			rec->txdone = 0;
			rec->per_latest = 0;
			rec->per_avg = 0;
			rec->badper = 0;
			if ((GIC(0x165) & (1u << (rec->ifidx & 0x1F))) != 0)
				limbyte = user_rc_limit[
					(phytype2mode((uint8_t)rec->phytype) * 2 +
					 rec->ifidx) * 2];
			else
				limbyte = rec->f4;
			if (limbyte < entry[10]) {
				rec->sched = entry - 12;
				rec->sched[11] |= 1;
				rec->ts_step = trc_tick();
			}
		} else {
			if ((entry[11] & 1) == 0 && (updateCounter & 3) != 0)
				goto clamp;
			if ((int)quality >= (int)entry[9] - (int)margin) {
				if ((entry[11] & 1) != 0) {
					entry[11] &= 0xFE;
					rec->budget = 500000;
				}
				goto clamp;
			}
			if ((entry[11] & 1) != 0 && rec->budget <= 4000000)
				rec->budget <<= 1;
			if (elapsed_wrap(now, rec->ts_lower) > 100000)
				rcLowerSched(rec);
		}
clamp:
		{
			const uint8_t *pair =
				&user_rc_limit[(phytype2mode((uint8_t)rec->phytype) * 2 +
						rec->ifidx) * 2];
			uint32_t idx = rec->sched[10];

			if (idx < pair[0])
				idx = pair[0];
			else if (pair[1] < idx)
				idx = pair[1];
			rec->sched = rec->tblbase + idx * 12;
			rec->attempts = 0;
			rec->per_num = 0;
			rec->txdone = 0;
		}
	}
	txctl[0] |= 0x00100000u;
	sched = rec->sched;
out:
	txctl[6] = (uint32_t)(uintptr_t)sched;
	if (sched == NULL)
		esp_log_write(1, "==dl", "wifi");
	pick_rate(rec, txctl, sched, sched != NULL ? sched[0] : 0);
}

void
rcGetRate(TrcRec *rec, uint32_t *txctl)
{
	uint8_t *entry = (uint8_t *)(uintptr_t)txctl[6];
	uint32_t used = (txctl[2] >> 4) & 0xF;
	uint32_t cum = 0;
	uint8_t rate;
	int i;

	for (i = 0; i < 4; i++) {
		cum = (cum + entry[1 + i * 2]) & 0xFF;
		if (used < cum) {
			if (rec == NULL || (rec->flags & 0x40) == 0 ||
			    (txctl[0] & 8) == 0) {
				if ((GIC(0x161) & (1u << ((txctl[1] >> 1) & 1))) == 0) {
					if ((int)used < (int)MIB8(0x0C) - 2)
						rate = entry[i * 2];
					else
						rate = entry[6];
				} else {
					rate = GIC(0x162);
				}
			} else {
				rate = rec->forced_rate;
			}
			((uint8_t *)txctl)[12] = rate;
			goto cap;
		}
	}
	if ((GIC(0x161) & (1u << ((txctl[1] >> 1) & 1))) == 0)
		rate = entry[6];
	else
		rate = GIC(0x162);
	((uint8_t *)txctl)[12] = rate;
cap:
	if ((txctl[1] & 2) == 0 &&
	    should_update_rate(((uint8_t *)txctl)[12], sig_max_rate2))
		((uint8_t *)txctl)[12] = sig_max_rate2;
}
