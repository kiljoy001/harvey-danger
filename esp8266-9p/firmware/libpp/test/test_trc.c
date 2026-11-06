/* Host test for trc.c (clean-room trc.o replacement).
 * Build: cc -m32 -std=c99 -Wall -Wextra -Werror \
 *   -DTRC_HOST_TEST -I.. -o /tmp/test_trc \
 *   ../trc.c ../trc_tables.c test_trc.c
 * Expected values are hand-computed from the decoded
 * blob arithmetic (re/inventory/b36/STAGE2-NOTES.md),
 * not from running the blob. 32-bit build so pointer
 * fields in descriptors match the target ABI. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../trc.h"
#include "../trc_tables.h"

/* ---- stubs for blob-owned externs ---- */
char g_ic[0x200];
char lmacConfMib[0x40];
uint8_t sig_max_rate2;
static uint32_t tick_now;
static int log_calls;

uint32_t trc_tick(void) { return tick_now; }
uint32_t bit_popcount(uint32_t v)
{
	uint32_t n = 0;
	while (v) { n += v & 1; v >>= 1; }
	return n;
}
void esp_log_write(int level, const char *tag, const char *fmt, ...)
{
	(void)level; (void)tag; (void)fmt;
	log_calls++;
}

#define GICB(off) (((uint8_t *)g_ic)[(off)])
#define MBBB(off) (((uint8_t *)lmacConfMib)[(off)])

static const uint8_t MAC1[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t MAC2[6] = {0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};

static TrcRec *
mkenabled(uint32_t ifidx, const uint8_t *mac, uint32_t slot, uint32_t phy)
{
	TrcRec *r = rc_enable_trc(ifidx, mac, slot, 0x1234);

	assert(r != NULL);
	r->phytype = phy;
	return r;
}

int
main(void)
{
	TrcRec *r, *r2;
	uint32_t tx[8];
	uint32_t desc[8];
	int i;

	/* rcAttach writes entry indices */
	rcAttach();
	for (i = 0; i < 12; i++)
		assert(rc11n_sched[i][10] == i);
	for (i = 0; i < 11; i++)
		assert(rc11g_sched[i][10] == i);
	for (i = 0; i < 4; i++)
		assert(rc11b_sched[i][10] == i);
	assert(rc_get_mask() == 0);

	/* phytype2mode */
	assert(phytype2mode(0) == 0);
	assert(phytype2mode(1) == 1);
	assert(phytype2mode(2) == 2);
	assert(phytype2mode(3) == 2);
	assert(phytype2mode(4) == 0);
	assert(phytype2mode(255) == 0);

	/* rate limit pairs: defaults, write, range errors */
	assert(rc_set_rate_limit_id(0, 0, 0xFF, 0xFF) == 0);
	assert(user_rc_limit[0] == 0 && user_rc_limit[1] == 3);
	assert(rc_set_rate_limit_id(1, 1, 2, 9) == 0);
	assert(user_rc_limit[6] == 2 && user_rc_limit[7] == 9);
	assert(rc_set_rate_limit_id(1, 1, 0xFF, 0xFF) == 0);
	assert(user_rc_limit[6] == 0 && user_rc_limit[7] == 10);
	assert(rc_set_rate_limit_id(2, 0, 0, 12) == -1);
	assert(rc_set_rate_limit_id(0, 0, 3, 2) == -1);
	assert(rc_set_rate_limit_id(7, 0, 0, 1) == -2);
	assert(rc_set_rate_limit_id(0, 2, 0, 1) == -1);

	/* enable / lookup / disable */
	r = mkenabled(0, MAC1, 3, 3);
	r2 = mkenabled(1, MAC2, 5, 1);
	assert(rc_get_mask() == ((1 << 3) | (1 << 5)));
	assert(rc_get_sta_trc(1 << 3) == r);
	assert(rc_get_sta_trc((1 << 3) | (1 << 5)) == NULL);
	assert(rc_get_trc(0, MAC1) == r);
	assert(rc_get_trc(1, MAC2) == r2);
	assert(rc_get_trc_by_index(5) == r2);
	assert(rc_get_trc_by_index(4) == NULL);
	assert(rcGetTrc() != NULL);
	assert(rc_only_sta_trc() == -1);	/* two stations */
	/* duplicate MAC on same interface+slot: mismatch */
	assert(rc_enable_trc(0, MAC2, 3, 0) == NULL);
	assert(log_calls == 1);
	rc_disable_trc(1, 0, 5);
	assert(rc_get_mask() == (1 << 3));
	assert(rc_only_sta_trc() == 0);	/* ifmask[0] has bit 3 */
	rc_disable_trc_by_interface(0);
	assert(rc_get_mask() == 0);

	r = mkenabled(0, MAC1, 0, 3);

	/* PhyMode: N, arg 20 -> ladder start 5, clamp [0,11] */
	rcUpdatePhyMode(r, 20);
	assert(r->sched == &rc11n_sched[5][0]);
	assert(r->sched[0] == 19);
	assert(r->maxidx == 11 && r->modeval == 8 && r->f4 == 0);
	assert(r->budget == 500000);
	assert(r->rssi_tx_new == 0x7f && r->rssi_rx_avg == 0x7f);
	/* N rate2idx via stored fn ptr */
	{
		uint8_t (*f)(uint32_t) = (uint8_t (*)(uint32_t))r->rate2idx;

		assert(f(31) == 0 && f(23) == 1 && f(22) == 2);
		assert(f(21) == 3 && f(20) == 4 && f(19) == 5);
		assert(f(18) == 6 && f(17) == 7 && f(16) == 7);
		assert(f(0) == 11 && f(1) == 10 && f(5) == 10);
		assert(f(2) == 9 && f(6) == 9 && f(11) == 13);
		assert(f(24) == 0);
	}
	/* PhyMode: G arg 25 -> start 2; B arg anything -> idx 3 */
	r->phytype = 1;
	rcUpdatePhyMode(r, 25);
	assert(r->sched == &rc11g_sched[2][0] && r->modeval == 7);
	r->phytype = 0;
	rcUpdatePhyMode(r, 0);
	assert(r->sched == &rc11b_sched[3][0] && r->maxidx == 3);
	r->phytype = 3;
	rcUpdatePhyMode(r, 20);

	/* TxDone success: PER accumulates, RSSI EMA runs */
	memset(desc, 0, sizeof(desc));
	desc[6] = (uint32_t)(uintptr_t)r->sched;
	desc[1] = 0x00040000;		/* success bits */
	desc[2] = 3 << 4;		/* 3 retries used */
	desc[3] = (uint32_t)(-40) << 8;	/* rssi byte -40 */
	rcUpdateTxDone(r, desc);
	assert(r->txdone == 1 && r->attempts == 1);
	assert(r->per_num == 5);	/* (2<3) -> 2+3 */
	assert(r->rssi_tx_new == -40);
	rcUpdateTxDone(r, desc);
	assert(r->per_num == 10 && r->attempts == 2);
	/* failure path: entry budget 32 per failure */
	desc[1] = 0x00080000;
	rcUpdateTxDone(r, desc);
	assert(r->per_num == 10 + 34 && r->badper == 1);
	/* bad-PER streak > 10 forces LowerSched: entry 5 -> 6 */
	r->sched = &rc11n_sched[5][0];
	r->badper = 10;
	rcUpdateTxDone(r, desc);
	assert(r->sched == &rc11n_sched[6][0]);
	assert(r->badper == 0 && r->per_num == 0);
	/* unknown desc bits: txdone counted, no PER */
	r->txdone = 0;
	desc[6] = (uint32_t)(uintptr_t)r->sched;
	desc[1] = 0;
	rcUpdateTxDone(r, desc);
	assert(r->txdone == 1 && r->attempts == 0);

	/* ---- part 2: GetRate, GetSched, misc ---- */
	rc_disable_trc_by_interface(0);
	r = mkenabled(0, MAC1, 1, 3);
	rcUpdatePhyMode(r, 20);
	assert(r->sched == &rc11n_sched[5][0]);
	MBBB(0x0C) = 10;
	sig_max_rate2 = 19;

	memset(tx, 0, sizeof(tx));
	tx[6] = (uint32_t)(uintptr_t)r->sched;
	tx[2] = 0 << 4;
	rcGetRate(r, tx);
	assert(((uint8_t *)tx)[12] == 19);
	tx[2] = 3 << 4;
	rcGetRate(r, tx);
	assert(((uint8_t *)tx)[12] == 18);
	tx[2] = 6 << 4;
	rcGetRate(r, tx);
	assert(((uint8_t *)tx)[12] == 11);
	tx[2] = 30 << 4;
	rcGetRate(r, tx);
	assert(((uint8_t *)tx)[12] == 6);
	tx[2] = 15 << 4;
	rcGetRate(r, tx);
	assert(((uint8_t *)tx)[12] == 6);
	tx[2] = 32 << 4;	/* retries field is a nibble: wraps to 0 */
	rcGetRate(r, tx);
	assert(((uint8_t *)tx)[12] == 19);
	sig_max_rate2 = 11;	/* order[11]=4 < order[19]=16 */
	tx[2] = 0;
	rcGetRate(r, tx);
	assert(((uint8_t *)tx)[12] == 11);
	sig_max_rate2 = 19;

	/* RX RSSI EMA */
	{
		int8_t d1 = -30, d2 = -20;

		rcUpdateRxDone(r, &d1);
		assert(r->rssi_rx_new == -30 && r->rssi_rx_avg == -30);
		rcUpdateRxDone(r, &d2);
		assert(r->rssi_rx_new == -20 && r->rssi_rx_avg == -28);
	}
	trc_onScanStart(r);
	assert((r->flags & 5) == 5);
	trc_onScanDone(r);
	assert((r->flags & 5) == 0);

	/* GetSched evaluation, no step (budget not elapsed) */
	memset(tx, 0, sizeof(tx));
	tx[0] = 8;
	r->attempts = 100;
	r->per_num = 100;
	r->txdone = 100;
	tick_now = 0;
	rcGetSched(r, tx);
	assert(r->per_avg == 108);	/* (128 + (225+128)/4) / 2 */
	assert(r->per_num == 0 && r->attempts == 0 && r->txdone == 0);
	assert((tx[0] & 0x100000) != 0);
	assert(tx[6] == (uint32_t)(uintptr_t)&rc11n_sched[5][0]);
	assert(((uint8_t *)tx)[12] == 19);

	/* GetSched step-up: budget spent, quality high */
	r->attempts = 100;
	r->per_num = 100;
	r->txdone = 100;
	r->budget = 0;
	r->ts_step = 0;
	tick_now = 200000;
	memset(tx, 0, sizeof(tx));
	tx[0] = 8;
	rcGetSched(r, tx);
	assert(r->sched == &rc11n_sched[4][0]);
	assert(rc11n_sched[4][11] == 1);
	assert(r->budget == 500000);
	assert(tx[6] == (uint32_t)(uintptr_t)&rc11n_sched[4][0]);

	/* NeedRTS thresholds */
	{
		static uint8_t cnx[64];
		static uint8_t tgt[16];

		*(uint16_t *)(cnx + 0x18) = 100;
		*(uint16_t *)(cnx + 0x1A) = 0;
		*(uint8_t **)(cnx + 0x28) = tgt;
		tgt[0x0C] = 11;
		assert(trc_NeedRTS(cnx) == 0);
		*(uint16_t *)(cnx + 0x18) = 701;
		assert(trc_NeedRTS(cnx) == 1);
		tgt[0x0C] = 0;
		*(uint16_t *)(cnx + 0x18) = 625;
		assert(trc_NeedRTS(cnx) == 0);
		*(uint16_t *)(cnx + 0x18) = 626;
		assert(trc_NeedRTS(cnx) == 1);
		tgt[0x0C] = 99;
		*(uint16_t *)(lmacConfMib + 0x0E) = 500;
		*(uint16_t *)(cnx + 0x18) = 501;
		assert(trc_NeedRTS(cnx) == 1);
	}

	/* ReachRetryLimit */
	memset(desc, 0, sizeof(desc));
	desc[0] = 0x08000000;
	desc[2] = 5 << 4;
	assert(rcReachRetryLimit(desc) == 1);
	desc[0] = 0;
	desc[2] = 12 << 4;
	assert(rcReachRetryLimit(desc) == 1);
	desc[2] = 4 << 4;
	assert(rcReachRetryLimit(desc) == 0);

	puts("test_trc: all tests passed");
	return 0;
}
