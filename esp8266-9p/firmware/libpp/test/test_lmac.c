/*
 * test_lmac.c - Host tests for the clean-room lmac.c.
 *
 * Build:
 *   cc -m32 -std=c99 -Wall -Wextra -Werror -DLMAC_HOST_TEST -I. \
 *      -o /tmp/test_lmac lmac.c test/test_lmac.c
 *
 * Expected values hand-computed from re/inventory/b40/.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "lmac.h"
#include "pp.h"
#include "trc.h"

/* ---- fake register file ------------------------------------------- */

static uint32_t regs[16384];

uint32_t *lmac_host_reg(uint32_t addr)
{
	return &regs[(addr & 0xffff) >> 2];
}

static uint32_t REG(uint32_t a) { return *lmac_host_reg(a); }
static void REGSET(uint32_t a, uint32_t v) { *lmac_host_reg(a) = v; }

/* ---- stub world ----------------------------------------------------- */

void *pTxRx;
static uint32_t txrx_buf[64];

static int n_enq_done, n_enq_rx, n_post, last_post_type;
static int n_process_txq, last_txq_ctx;
static int n_rcupdate, n_rcgetrate, n_rcattach;
static uint32_t last_basic_mask;
static int last_basic_erp;
static int reach_limit_ret;
static uint32_t caltxop_ret;
static int n_enable_tx;
static uint32_t en_tx_ac, en_tx_aifs, en_tx_backoff;
static int n_phy_open, n_wakeup;
static uint32_t last_wakeup;
static int lock_depth;
static PpNode *fetch_ret;

void ppEnqueueTxDone(PpNode *n) { (void)n; n_enq_done++; }
void ppEnqueueRxq(PpNode *n) { (void)n; n_enq_rx++; }
int32_t pp_post(uint32_t t, uint32_t a)
{ (void)a; n_post++; last_post_type = (int)t; return 0; }
int32_t ppProcessTxQ(uint32_t ctx)
{ n_process_txq++; last_txq_ctx = (int)ctx; return 0; }
PpNode *ppFetchTxQFirstAvail(uint32_t q) { (void)q; return fetch_ret; }
uint32_t ppCalTxop(uint32_t q, uint32_t a, uint32_t l)
{ (void)q; (void)a; (void)l; return caltxop_ret; }
void ppCalFrameTimes(PpNode *n) { (void)n; }

void rcAttach(void) { n_rcattach++; }
int RC_SetBasicRate(uint32_t mask, int8_t erp)
{ last_basic_mask = mask; last_basic_erp = erp; return 0; }
uint16_t RC_GetAckTime(uint32_t r) { (void)r; return 0; }
void rcGetRate(TrcRec *r, uint32_t *t) { (void)r; (void)t; n_rcgetrate++; }
void rcUpdateTxDone(TrcRec *r, const uint32_t *d)
{ (void)r; (void)d; n_rcupdate++; }
int rcReachRetryLimit(const uint32_t *d) { (void)d; return reach_limit_ret; }

void wDev_EnableTransmit(uint32_t ac, uint32_t aifs, uint32_t bo)
{ n_enable_tx++; en_tx_ac = ac; en_tx_aifs = aifs; en_tx_backoff = bo; }
void vPortETSIntrLock(void) { lock_depth++; }
void vPortETSIntrUnlock(void) { lock_depth--; }
void ets_delay_us(uint32_t us) { (void)us; }
int ets_printf(const char *fmt, ...) { (void)fmt; return 0; }
void esp_log_write(int l, const char *t, const char *f, ...)
{ (void)l; (void)t; (void)f; }
void clockgate_watchdog(int on) { (void)on; }
void phy_open_rf(void) { n_phy_open++; }
void pm_wakeup_init(uint32_t a) { n_wakeup++; last_wakeup = a; }

/* ---- AC mirror (layout from lmac.h) --------------------------------- */

typedef struct {
	void *cur; uint8_t ac, aifs, cw, cwmin, cwmax, qsrc, qlrc;
	uint8_t intxop, txop_enough, trytxop, f0e, acm, org, state, end_status;
	uint8_t f13; int16_t txop, txop_delta; uint16_t txop_max, lifetime;
	uint32_t su_cnt, fai_cnt;
} Ac;

static Ac *AC(int i) { return (Ac *)GetAccess((uint32_t)i); }

static int fails;
#define CHECK(c) do { if (!(c)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t ctrlbuf[8];
static uint8_t hdrbuf[64];
static uint8_t framebyte;
static PpNode node;

static void
mknode(uint32_t c0, uint32_t c1, uint32_t c2)
{
	memset(ctrlbuf, 0, sizeof ctrlbuf);
	memset(hdrbuf, 0, sizeof hdrbuf);
	memset(&node, 0, sizeof node);
	ctrlbuf[0] = c0; ctrlbuf[1] = c1; ctrlbuf[2] = c2;
	node.f0 = 0xffffffffu;
	node.ctrl = ctrlbuf;
	node.hdr = hdrbuf;
	node.frame = &framebyte;
	framebyte = 0;
	node.len18 = 20;
	node.len = 100;
}

int
main(void)
{
	Ac *a0, *a1;

	pTxRx = txrx_buf;
	caltxop_ret = 100;
	reach_limit_ret = 0;

	/* --- lmacInit / MIB / AC defaults --- */
	lmacInit();
	CHECK(lmacConfMib[0x0c] == 0x20 && lmacConfMib[0x0d] == 0x20);
	CHECK(*(uint32_t *)(lmacConfMib + 0) == 0x400);
	CHECK(*(uint16_t *)(lmacConfMib + 0x1c) == 0x15f);
	CHECK(*(uint16_t *)(lmacConfMib + 0x10) == 0x92a);
	CHECK(last_basic_mask == 0x15f && last_basic_erp == 1);
	CHECK(n_rcattach == 1);
	a0 = AC(0); a1 = AC(1);
	CHECK(a0->aifs == 2 && a0->cw == 2 && a0->cwmin == 2 &&
	      a0->cwmax == 3 && a0->txop_max == 0x5e0);
	CHECK(a1->aifs == 2 && a1->cwmin == 3 && a1->cwmax == 4 &&
	      a1->txop_max == 0x0bc0);
	CHECK(lmacIsIdle(0) == 1);

	/* --- retry limit clamps --- */
	lmacSetRetryLimit(3);
	CHECK(lmacConfMib[0x0d] == 5 && lmacConfMib[0x0c] == 5);
	lmacSetRetryLimit(0x40);
	CHECK(lmacConfMib[0x0d] == 0x14 && lmacConfMib[0x0c] == 0x14);
	lmacSetRetryLimit(0x20);	/* >= 0x15 clamps to 0x14 too */
	CHECK(lmacConfMib[0x0d] == 0x14);
	lmacSetRetryLimit(10);
	CHECK(lmacConfMib[0x0d] == 10);

	/* --- internal_set_tx_param: blob stores p4 into both bytes --- */
	memset(lmacConfMib, 0, 36);
	lmacInit();
	esp_wifi_internal_set_tx_param(0, 0, 0, 7, 9);
	CHECK(lmacConfMib[0x0d] == 7);
	CHECK(lmacConfMib[0x0c] == 7);	/* not 9: byte-exact blob bug */

	/* --- lmacTxFrame: registers + backoff + EnableTransmit --- */
	memset(regs, 0, sizeof regs);
	REGSET(0x3ff20e44, 0xffffffffu);	/* RNG */
	mknode(0, 0, 0);
	lmacTxFrame(&node, 0);
	CHECK(a0->state == 1 && a0->cur == &node);
	CHECK(n_enable_tx == 1 && en_tx_ac == 0 && en_tx_aifs == 2);
	/* cw=2: ((1<<2)-1 & RNG) * 0x183 / 100 = 3*387/100 = 11 */
	CHECK(en_tx_backoff == 11);
	CHECK(REG(0x3ff20dc4) == 0x0123ffffu);
	CHECK(REG(0x3ff20dd0) == (100u << 16));	/* ppCalTxop -> duration */
	CHECK(REG(0x3ff20dc8) == 120u);		/* total len 100+20 */
	CHECK(REG(0x3ff20dd4) == (0x400u << 12)); /* MIB 0x400, TSF 0 */
	CHECK(*(uint16_t *)(hdrbuf + 2) == 100);
	CHECK(a0->txop == 100 && a0->txop_enough == 1);

	/* --- set_status + TxSuccess -> EndFrame(0) recycle --- */
	lmac_set_status(0, 0);
	CHECK(a0->su_cnt == 1 && a0->state == 2);
	ctrlbuf[0] = 8;		/* TxDone bit3 -> rcUpdateTxDone */
	lmacProcessTxSuccess(0, 0x55);
	CHECK(((uint8_t *)ctrlbuf)[0x0d] == 0x55);
	CHECK(a0->state == 0 && a0->end_status == 3);
	CHECK((ctrlbuf[1] & 0x00040000) != 0);	/* recycle bits */
	CHECK(n_enq_done == 1 && n_rcupdate == 1);
	CHECK(n_post == 1 && last_post_type == 4);

	/* --- AckTimeout (no ACK flag) -> ShortRetryFail -> retransmit --- */
	mknode(0, 0, 0);
	lmacTxFrame(&node, 0);
	{
		int en_before = n_enable_tx;

		lmacProcessAckTimeout(0);
		CHECK(a0->qsrc == 1 && a0->cw == 3);
		CHECK(ctrlbuf[2] == 0x110u);	/* short+long counters */
		CHECK((hdrbuf[1] & 8) != 0);	/* retry bit set */
		CHECK(n_enable_tx == en_before + 1);	/* retransmitted */
		CHECK(a0->state == 1);
	}

	/* --- MSDU aged / timestamp adjust --- */
	REGSET(0x3ff20c00, (0x400 - 5) * 0x400 + 1);
	CHECK(lmacMSDUAged(&node) == 1);
	REGSET(0x3ff20c00, 0x100000);
	lmacAdjustTimestamp(&node);
	CHECK(*(uint32_t *)((uint8_t *)ctrlbuf + 0x14) ==
	      0x100000 - (0x400 - 0x200) * 0x400);

	/* --- 11b PLCP rate adjust (+4 with short preamble) --- */
	esp_wifi_set_11b_tx_plcp(1, 0);
	a0->state = 0; a0->cur = NULL;
	mknode(0, 0, 0);
	((uint8_t *)ctrlbuf)[0x0c] = 2;
	lmacTxFrame(&node, 0);
	CHECK(((uint8_t *)ctrlbuf)[0x0c] == 6);
	esp_wifi_set_11b_tx_plcp(0, 0);

	/* --- TxTimeout on AC0 (sign bit in AC hw reg) -> discard --- */
	a0->state = 0; a0->cur = NULL;
	mknode(0, 0, 0);
	lmacTxFrame(&node, 0);
	{
		int enq_before = n_enq_done;

		REGSET(0x3ff20dc4, 0x80000000u);
		REGSET(0x3ff20cc0, 1u << 16);	/* AC0 timeout bit */
		lmacProcessTxTimeout();
		CHECK(a0->fai_cnt == 1);
		CHECK(a0->state == 0);		/* discarded + TxDone */
		CHECK(a0->cur == NULL);
		CHECK(n_enq_done == enq_before + 1);
		CHECK((REG(0x3ff20cc0) & (1u << 16)) == 0);
	}

	/* --- check_ac hang detect: two identical snapshots --- */
	a0->state = 0; a0->cur = NULL;
	mknode(0, 0, 0);
	lmacTxFrame(&node, 0);
	REGSET(0x60000710, 0);
	lmac_check_ac();		/* snapshot */
	lmac_check_ac();		/* unchanged -> hang reset */
	CHECK(n_phy_open == 1);
	CHECK(n_wakeup == 1 && last_wakeup == 8);
	CHECK(a0->state == 0);		/* state 1 -> 6 -> discard */

	if (fails == 0)
		printf("LMAC: ALL PASS\n");
	return fails != 0;
}
