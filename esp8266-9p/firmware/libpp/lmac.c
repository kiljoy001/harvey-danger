#define LMAC_BISECT 1
#define LMAC_CNT 1
/*
 * lmac.c - Clean-room ESP8266 LMAC (lmac.o replacement).
 * Full header comment in lmac.h. Transliterated from the
 * disassembly and Ghidra decompiles in re/inventory/b40/;
 * the blob's own arithmetic, asserts and quirks are kept
 * byte-exact. Arguments the blob flows through registers into
 * callees (rcUpdateTxDone's desc, ShortRetryFail's is_rts,
 * wDev_EnableTransmit's third word) are declared in full -
 * see B39 for what dropping one costs.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "lmac.h"
#include "pp.h"		/* PpNode, pp_* queue calls, pTxRx */
#include "trc.h"	/* rcAttach, rcGetRate, rcUpdateTxDone, ... */
#include "rate_control.h"	/* RC_GetAckTime, RC_SetBasicRate */

#ifdef LMAC_HOST_TEST
#include "test/lmac_host.h"
#define LREG(a)		(*lmac_host_reg(a))
#define MEMW()		((void)0)
#else
#define LREG(a)		(*(volatile uint32_t *)(a))
#define MEMW()		__asm__ volatile ("memw" ::: "memory")
#endif

extern int ets_printf(const char *fmt, ...);
extern void ets_delay_us(uint32_t us);
extern void esp_log_write(int level, const char *tag, const char *fmt, ...);
extern void vPortETSIntrLock(void);
extern void vPortETSIntrUnlock(void);
extern void clockgate_watchdog(int on);
extern void phy_open_rf(void);
extern void pm_wakeup_init(uint32_t arg);
extern void wDev_EnableTransmit(uint32_t ac, uint32_t aifs, uint32_t backoff);

/* MAC register block (only touched through these addresses). */
#define REG_TSF_COUNT	0x3ff20c00	/* free-running count (pm too) */
#define REG_AC_BASE1	0x3ff20dc4	/* per-AC regs descend by 0x18 */
#define REG_AC_BASE2	0x3ff20dc8
#define REG_AC_BASE3	0x3ff20dcc
#define REG_AC_BASE4	0x3ff20dd0
#define REG_AC_BASE5	0x3ff20dd4
#define REG_TX_TIMEOUT	0x3ff20cc0	/* timeout bitmap at bits 16..27 */
#define REG_3FF20C70	0x3ff20c70
#define REG_3FF20C74	0x3ff20c74
#define REG_RNG		0x3ff20e44	/* hardware random */
#define REG_CONF_DC0	0x3ff20dc0
#define REG_PHY_60000710 0x60000710
#define REG_3FF201C0	0x3ff201c0
#define REG_3FF201BC	0x3ff201bc

#define REG_AC(base, ac) LREG((base) - (uint32_t)(ac) * 0x18)

/* ------------------------------------------------------------------ */
/* State (byte-exact with the blob's sections)                         */

typedef struct LmacAc {
	void *cur;		/* +0x00 */
	uint8_t ac;		/* +0x04 */
	uint8_t aifs;		/* +0x05 */
	uint8_t cw;		/* +0x06 */
	uint8_t cwmin;		/* +0x07 */
	uint8_t cwmax;		/* +0x08 */
	uint8_t qsrc;		/* +0x09 */
	uint8_t qlrc;		/* +0x0a */
	uint8_t intxop;		/* +0x0b */
	uint8_t txop_enough;	/* +0x0c */
	uint8_t trytxop;	/* +0x0d */
	uint8_t f0e;		/* +0x0e */
	uint8_t acm;		/* +0x0f */
	uint8_t org;		/* +0x10 */
	uint8_t state;		/* +0x11 */
	uint8_t end_status;	/* +0x12 */
	uint8_t f13;		/* +0x13 */
	int16_t txop;		/* +0x14 */
	int16_t txop_delta;	/* +0x16 */
	uint16_t txop_max;	/* +0x18 */
	uint16_t lifetime;	/* +0x1a */
	uint32_t su_cnt;	/* +0x1c */
	uint32_t fai_cnt;	/* +0x20 */
} LmacAc;			/* 36 bytes */

#ifdef LMAC_BISECT
extern LmacAc our_instances[];	/* bisect: blob's table, shared */
#else
static LmacAc our_instances[12];	/* blob: 8 slots; 8..11 absorb timeout writes */
#endif
uint8_t lmacConfMib[36];		/* blob COMMON */
#ifdef LMAC_CNT
uint32_t lmac_dbg[16];
#define LCNT(n) (lmac_dbg[(n)]++)
#else
#define LCNT(n)
#endif
static uint32_t s_lmac_check_tx_hang[4]; /* .bss, 16 B */
static uint8_t s_set_11b_plcp;		/* .bss */
static uint8_t s_tx_with_long_plcp;	/* .bss */

#define MIB8(off)	(lmacConfMib[(off)])
#define MIB16(off)	(*(uint16_t *)(lmacConfMib + (off)))
#define MIB32(off)	(*(uint32_t *)(lmacConfMib + (off)))

#define LMAC_ASSERT(line) do { \
		ets_printf("%s %u\n", "mac", (unsigned)(line)); \
		for (;;) { } \
	} while (0)

/* blob-internal helpers */
static void lmacTxDone(PpNode *node, int flag);
static void lmacDiscardFrameExchangeSequence(LmacAc *ac);
static void lmacSetTxFrame(LmacAc *ac);
#ifdef LMAC_SPLIT_EF
void lmacEndFrameExchangeSequence(LmacAc *ac, int cont);
#else
static void lmacEndFrameExchangeSequence(LmacAc *ac, int cont);
#endif
#ifdef LMAC_SPLIT
void lmacProcessShortRetryFail(LmacAc *ac, PpNode *node, int is_rts);
#else
static void lmacProcessShortRetryFail(LmacAc *ac, PpNode *node, int is_rts);
#endif

/* ------------------------------------------------------------------ */

static void
lmacTxDone(PpNode *node, int flag)
{
#ifdef LMAC_DBGP
	ets_printf("TD node=%x fl=%d\n", (unsigned)(uintptr_t)node, flag);
#endif
	ppEnqueueTxDone(node);
	if ((node->ctrl[0] & 8) != 0)
		rcUpdateTxDone((TrcRec *)node->trc, node->ctrl);
	pp_post(4, 0);
	if (flag != 0) {
		uint32_t ac = (node->ctrl[2] >> 0x14) & 7;

#ifdef LMAC_DBGP
		ets_printf("TD>pq ac=%u st=%u\n", ac, our_instances[ac].state);
#endif
		if (our_instances[ac].state == 0) {
			LCNT(8);
			ppProcessTxQ(ac);
		}
	}
}

static void
lmacDiscardFrameExchangeSequence(LmacAc *ac)
{
	PpNode *node;

	if (ac->state != 6)
		LMAC_ASSERT(0x340);
	node = (PpNode *)ac->cur;
	ac->state = 0;
	ac->cur = NULL;
	node->ctrl[1] = (node->ctrl[1] & 0xfc03ffff) | 0x00200000;
	lmacTxDone(node, 1);
}

void
lmacRecycleMPDU(void *ac_ignored, void *node_, uint8_t flag)
{
	PpNode *node = (PpNode *)node_;
	LCNT(9);
#ifdef LMAC_DBGP
	ets_printf("RC node=%x fl=%u\n", (unsigned)(uintptr_t)node, flag);
#endif

	(void)ac_ignored;
	node->ctrl[1] = (node->ctrl[1] & 0xfc03ffff) | 0x00040000;
	lmacTxDone(node, flag);
}

/* Build the per-AC hardware TX registers from the current frame. */
static void
lmacSetTxFrame(LmacAc *ac)
{
	PpNode *node = (PpNode *)ac->cur;
	uint32_t *ctrl = node->ctrl;
	uint32_t ctrl2 = ctrl[2];
	uint32_t acc = ctrl2 >> 0x17;
	uint16_t dur = (uint16_t)acc;
	bool inburst;
	uint32_t ctrl0, cat, v14, v10, v8, flagb, totlen, rateb;

	if ((uint8_t)(ac->state - 2) > 1 && ac->state != 0)
		LMAC_ASSERT(299);

	if (!ac->intxop && !ac->trytxop) {
		if (ac->txop_max == 0) {
			inburst = true;
			if (((uint8_t *)ctrl)[0x0c] > 0x0f)
				*(uint16_t *)(node->hdr + 2) = dur;
		} else {
			*(uint16_t *)(node->hdr + 2) = dur;
			ctrl0 = ctrl[0];
			if ((ctrl0 & 0x402) == 0 &&
			    (ctrl0 & 0x480000) != 0x400000) {
				uint16_t t = (uint16_t)ppCalTxop(
					(ctrl[1] >> 2) & 0xf, acc, ac->txop_max);

				*(uint16_t *)(node->hdr + 2) = t;
			}
			if (acc < *(uint16_t *)(node->hdr + 2)) {
				ac->txop = *(int16_t *)(node->hdr + 2);
				ac->txop_enough = 1;
				ac->txop_delta = 0;
				if ((ctrl[0] & 0x100) != 0)
					(void)RC_GetAckTime(
						((uint8_t *)ctrl)[0x0c]);
			} else {
				ac->txop = 0;
			}
			inburst = true;
		}
	} else {
		int16_t rem = (int16_t)(ac->txop - ac->txop_delta);
		int16_t c14;

		ac->txop = rem;
		c14 = *(int16_t *)((uint8_t *)ctrl + 0x0e);
		ac->txop_delta = c14;
		if ((int)acc < rem - c14)
			*(int16_t *)(node->hdr + 2) = (int16_t)(rem - c14);
		else {
			*(uint16_t *)(node->hdr + 2) = dur;
			ac->txop_enough = 0;
		}
		inburst = false;
	}

	ctrl0 = ctrl[0];
	cat = 0;
	if ((ctrl0 & 0x402) == 0 && (ctrl0 & 0x480000) != 0x400000) {
		cat = 1;
		if ((ctrl0 & 0x100000) != 0)
			cat = 3;
	}

	if (s_set_11b_plcp) {
		uint8_t rb = ((uint8_t *)ctrl)[0x0c];
		bool store = true;
		uint8_t nb;

		if (!s_tx_with_long_plcp) {
			if ((uint8_t)(rb - 1) > 2)
				store = false;
			nb = (uint8_t)(rb + 4);
		} else {
			nb = (uint8_t)(rb - 4);
			if (nb > 3)
				store = false;
		}
		if (store)
			((uint8_t *)ctrl)[0x0c] = nb;
	}

	{
		uint32_t remain = MIB32(0) -
			((LREG(REG_TSF_COUNT) - ctrl[5]) >> 10);

		if (MIB32(0) < remain)
			remain = 10;
		MEMW();
		REG_AC(REG_AC_BASE5, ac->ac) = (remain & 0xfff) << 12;
	}

	flagb = 0x01000000;
	v14 = ((ctrl0 & 0x200) != 0 && inburst) ? 0x10200000 : 0x00200000;
	v10 = ((ctrl0 & 0x100) != 0 && inburst) ? 0x08000000 : 0;
	v8 = (ac->txop < 1 && ((uint8_t *)ctrl)[0x0c] < 0x10)
		? 0x00400000 : 0;
	MEMW();
	REG_AC(REG_AC_BASE1, ac->ac) = (node->f0 & 0x0003ffff) |
		(cat << 24) | v14 | v10 | v8;
	MEMW();
	REG_AC(REG_AC_BASE4, ac->ac) =
		(uint32_t)(*(uint16_t *)(node->hdr + 2)) << 16;

	rateb = ((uint8_t *)ctrl)[0x0c];
	totlen = (uint32_t)node->len + node->len18;
	MEMW();
	REG_AC(REG_AC_BASE2, ac->ac) = ((rateb & 0xf) << 12) |
		(totlen & 0xfff) | (((ctrl[1] >> 6) & 0xff) << 16) |
		(rateb < 0x10 ? 0 : flagb);
	if (rateb > 0x0f) {
		uint32_t v = (rateb < 0x18) ? 0x07000000 : 0x87000000;

		MEMW();
		REG_AC(REG_AC_BASE3, ac->ac) = (rateb & 7) |
			((totlen & 0xffff) << 8) | v;
	}
}

#ifndef LMAC_SPLIT_EF
static void
lmacEndFrameExchangeSequence(LmacAc *ac, int cont)
{
	PpNode *node;
#ifdef LMAC_DBGP
	ets_printf("EF ac=%d st=%u cont=%d cur=%x\n",
	    (int)(ac - our_instances), ac->state, cont,
	    (unsigned)(uintptr_t)ac->cur);
#endif

	if (cont != 0) {
		LCNT(4);
		if ((uint8_t)(ac->state - 1) > 1)
			LMAC_ASSERT(0x3bb);
		ac->state = 3;
		node = (PpNode *)ac->cur;
		if ((node->ctrl[0] & 0x20000) == 0) {
#ifdef LMAC_DBGP
			ets_printf("EF>tx intxop=%u try=%u\n", ac->intxop, ac->trytxop);
#endif
			if (!ac->intxop) {
				if (ac->trytxop) {
					ac->trytxop = 0;
					ac->intxop = 0;
				}
				rcGetRate((TrcRec *)node->trc, node->ctrl);
				lmacTxFrame(node, ac->ac);
			} else {
				uint32_t oldrate =
					((uint8_t *)node->ctrl)[0x0c];

				ac->txop_delta =
					(uint16_t)(node->ctrl[2] >> 0x17);
				rcGetRate((TrcRec *)node->trc, node->ctrl);
				if (((uint8_t *)node->ctrl)[0x0c] !=
				    (uint8_t)oldrate)
					ppCalFrameTimes(node);
				ac->txop_delta = (int16_t)(
					(ac->txop_delta +
					 *(int16_t *)((uint8_t *)node->ctrl +
						      0x0e)) -
					(uint16_t)(node->ctrl[2] >> 0x17));
				lmacSetTxFrame(ac);
				ac->state = 1;
				wDev_EnableTransmit(ac->ac, 0, 0);
			}
		} else {
#ifdef LMAC_DBGP
			ets_printf("EF>final\n");
#endif
			ac->cur = NULL;
			ac->state = 0;
			node->ctrl[1] = (node->ctrl[1] & 0xfc03ffff) |
				0x00200000;
			lmacTxDone(node, 1);
		}
		ac->end_status = 5;
		goto tail;
	}

	if (ac->state == 6) {
		LCNT(6);
		int rl;
		uint32_t bits, flag;

		ac->state = 0;
		node = (PpNode *)ac->cur;
		rl = rcReachRetryLimit(node->ctrl);
		node = (PpNode *)ac->cur;
		if (rl == 0) {
			uint32_t *ctrl = node->ctrl;

			if ((uint32_t)MIB8(0x0d) <=
			    ((ctrl[2] >> 8) & 0x3f))
				goto fail;
			if (((ctrl[2] >> 0x0e) & 0x3f) <
			    (uint32_t)MIB8(0x0c)) {
				if (lmacMSDUAged(node) == 0) {
					flag = (ac->state == 0);
					bits = 0x00240000;
				} else {
					flag = 1;
					if (ac->state != 0)
						flag = (uint32_t)rl;
					bits = 0x001c0000;
				}
			} else {
				flag = (uint32_t)rl;
				if (ac->state == 0)
					flag = 1;
				bits = 0x000c0000;
			}
			ctrl[1] = (ctrl[1] & 0xfc03ffff) | bits;
		} else {
fail:
			flag = (ac->state == 0);
			node->ctrl[1] = (node->ctrl[1] & 0xfc03ffff) |
				0x00080000;
		}
		lmacTxDone(node, (int)flag);
	} else if (ac->state == 5) {
		LCNT(5);
		ac->state = 0;
		if (ac->cur == NULL)
			LMAC_ASSERT(0x3ac);
		if (ac->trytxop) {
			ac->trytxop = 0;
			ac->intxop = 0;
		}
		if (ac->intxop)
			ac->intxop = 0;
		ac->end_status = 3;
		lmacRecycleMPDU(ac, ac->cur, 1);
	}

tail:
	if (ac->trytxop)
		LMAC_ASSERT(0x3c2);
}
#endif /* LMAC_SPLIT */

void
lmacTxFrame(void *node_, uint32_t acidx)
{
	PpNode *node = (PpNode *)node_;
	LmacAc *ac;
	uint32_t backoff;

	acidx &= 0xff;
	ac = &our_instances[acidx];
	LCNT(0);
#ifdef LMAC_DBGP
	ets_printf("TF ac=%u st=%u node=%x\n", acidx, ac->state,
	    (unsigned)(uintptr_t)node);
#endif
	if ((uint8_t)(ac->state - 3) > 1 && ac->state != 0)
		LMAC_ASSERT(0x60b);
	if (ac->state == 4) {
		if (ac->cur != node)
			LMAC_ASSERT(0x60e);
	} else {
		ac->cur = node;
		if (node == NULL)
			LMAC_ASSERT(0x614);
		if ((uint32_t)MIB16(0x0e) <
		    (uint32_t)node->len18 + node->len) {
			uint32_t c0 = node->ctrl[0];

			if ((c0 & 2) == 0)
				node->ctrl[0] = (c0 & 0xffffefff) | 0x100;
		}
		if ((node->ctrl[0] & 0x1000) != 0 && ac->state == 3 &&
		    ((node->ctrl[2] >> 4) & 0xf) > 2) {
			uint32_t c2 = node->ctrl[2];

			node->ctrl[0] = (node->ctrl[0] & 0xffffefff) | 0x100;
			node->ctrl[2] = ((c2 & 0xfff03fff) |
				(((c2 >> 8) & 0x3f) << 14)) & 0xffffc0ff;
		}
		lmacSetTxFrame(ac);
	}
	MEMW();
	backoff = ((((1u << (ac->cw & 0x1f)) - 1) & LREG(REG_RNG)) *
		0x183) / 100;
	ac->state = 1;
	if ((node->ctrl[1] & 2) != 0 &&
	    ((uint8_t *)node->frame)[0] == 8 &&
	    (node->ctrl[0] & 2) != 0)
		backoff = 0x7d;
	wDev_EnableTransmit(ac->ac, ac->aifs, backoff);
}

void
lmacRxDone(void *node)
{
	ppEnqueueRxq((PpNode *)node);
	pp_post(5, 0);
}

int
lmacMSDUAged(void *node_)
{
	PpNode *node = (PpNode *)node_;

	MEMW();
	return (uint32_t)((int32_t)MIB32(0) - 5) * 0x400 <
		(uint32_t)((int32_t)LREG(REG_TSF_COUNT) -
			*(int32_t *)((uint8_t *)node->ctrl + 0x14));
}

void
lmacAdjustTimestamp(void *node_)
{
	PpNode *node = (PpNode *)node_;
	int32_t span = (int32_t)(MIB32(0) - MIB32(4)) * 0x400;

	MEMW();
	if ((uint32_t)span < (uint32_t)((int32_t)LREG(REG_TSF_COUNT) -
			*(int32_t *)((uint8_t *)node->ctrl + 0x14)))
		*(int32_t *)((uint8_t *)node->ctrl + 0x14) =
			(int32_t)LREG(REG_TSF_COUNT) - span;
}

void
lmacProcessTxSuccess(uint32_t acidx, uint8_t arg)
{
	LmacAc *ac = &our_instances[acidx & 0xff];
	PpNode *node;
	uint32_t ctrl0;

	if (ac->state != 2)
		LMAC_ASSERT(0x2fd);
	node = (PpNode *)ac->cur;
	LCNT(1);
#ifdef LMAC_DBGP
	ets_printf("TS ac=%u node=%x c0=%x\n", acidx & 0xff,
	    (unsigned)(uintptr_t)node, node->ctrl[0]);
#endif
	ctrl0 = node->ctrl[0];
	if ((ctrl0 & 0x402) == 0 && (ctrl0 & 0x480000) != 0x400000) {
		ac->cw = ac->cwmin;
		if ((ctrl0 & 0x100) == 0) {
			ac->qsrc = 0;
		} else {
			ac->qsrc = 0;
			ac->qlrc = 0;
		}
	} else {
		ac->cw = ac->cwmin;
		ac->qsrc = 0;
		ac->qlrc = 0;
		arg = 0x7f;
	}
	((uint8_t *)node->ctrl)[0x0d] = arg;
	ac->state = 5;
	lmacEndFrameExchangeSequence(ac, 0);
}

#ifndef LMAC_SPLIT
static void
lmacProcessShortRetryFail(LmacAc *ac, PpNode *node, int is_rts)
{
	uint32_t c8, ns;
	LCNT(3);
#ifdef LMAC_DBGP
	ets_printf("SRF ac=%d st=%u rts=%d qs=%u cw=%u\n",
	    (int)(ac - our_instances), ac->state, is_rts, ac->qsrc, ac->cw);
#endif
	int flag;

	if (node == NULL)
		LMAC_ASSERT(0x442);
	if (ac->qsrc < MIB8(0x0d))
		ac->qsrc++;
	if (ac->qsrc < MIB8(0x0d)) {
		if (ac->cwmax > ac->cw)
			ac->cw = (uint8_t)(ac->cw + 1);
	} else {
		ac->cw = ac->cwmin;
	}
	c8 = node->ctrl[2];
	ns = ((c8 >> 8) & 0x3f) + 1;
	ns &= 0x3f;
	node->ctrl[2] = (c8 & 0xffffc0ff) | (ns << 8);
	if (is_rts == 0 || ((node->ctrl[2] >> 4) & 0xf) < ns)
		node->ctrl[2] = (node->ctrl[2] & 0xffffff0f) |
			((((node->ctrl[2] >> 4) & 0xf) + 1) & 0xf) << 4;
	if ((uint8_t)(ac->state - 1) > 1)
		LMAC_ASSERT(0x45b);
	if (rcReachRetryLimit(node->ctrl) == 0 &&
	    ((node->ctrl[2] >> 8) & 0x3f) < (uint32_t)MIB8(0x0d) &&
	    lmacMSDUAged(node) == 0) {
		node->hdr[1] |= 8;
		flag = 1;
	} else {
		ac->state = 6;
		flag = 0;
	}
	lmacEndFrameExchangeSequence(ac, flag);
}
#endif /* LMAC_SPLIT */

void
lmacProcessAckTimeout(uint32_t acidx)
{
	LmacAc *ac = &our_instances[acidx & 0xff];
	PpNode *node = (PpNode *)ac->cur;
	uint8_t cwmin;
	int flag;

	LCNT(2);
	if (node == NULL)
		LMAC_ASSERT(0x4ee);
#ifdef LMAC_DBGP
	ets_printf("AT ac=%u st=%u c0=%x qs=%u ql=%u\n",
	    acidx & 0xff, ac->state, node->ctrl[0], ac->qsrc, ac->qlrc);
#endif
	if ((node->ctrl[0] & 0x100) != 0) {
		uint32_t c8, v;

		cwmin = ac->cwmin;
		ac->qsrc = 0;
		ac->cw = cwmin;
		if (ac->qlrc < MIB8(0x0c))
			ac->qlrc++;
		if (ac->qlrc < MIB8(0x0c) && cwmin < ac->cwmax)
			ac->cw = (uint8_t)(cwmin + 1);
		c8 = node->ctrl[2];
		v = c8 & 0xfff03fff;
		node->ctrl[2] = (v & 0xffffff0f) |
			((((c8 >> 0x0e) & 0x3f) + 1) & 0x3f) << 0x0e |
			((((v & 0xf0) >> 4) + 1) & 0xf) << 4;
		if ((uint8_t)(ac->state - 1) < 2) {
			if (rcReachRetryLimit(node->ctrl) == 0 &&
			    ((node->ctrl[2] >> 0x0e) & 0x3f) <
				(uint32_t)MIB8(0x0c) &&
			    lmacMSDUAged(node) == 0) {
				node->hdr[1] |= 8;
				flag = 1;
			} else {
				ac->state = 6;
				flag = 0;
			}
			lmacEndFrameExchangeSequence(ac, flag);
			return;
		}
		LMAC_ASSERT(0x4c4);
	}
	lmacProcessShortRetryFail(ac, node, 0);
}

void
lmacProcessCtsTimeout(uint32_t acidx)
{
	LmacAc *ac = &our_instances[acidx & 0xff];

	if (ac->cur == NULL)
		LMAC_ASSERT(0x4e0);
	lmacProcessShortRetryFail(ac, (PpNode *)ac->cur, 1);
}

void
lmacProcessTxError(uint32_t code, uint32_t acidx)
{
	code &= 0xff;
	acidx &= 0xff;
	if (code < 0x12) {
		if (code < 0x10) {
			if (code == 1 || code == 3) {
				lmacProcessAckTimeout(acidx);
				return;
			}
			goto err;
		}
	} else if (code - 0xc0 > 1) {
err:
		esp_log_write(1, "wifi", "tx error 0x%x\n", code);
		LMAC_ASSERT(0x53a);
	}
	our_instances[acidx].state = 6;
	lmacEndFrameExchangeSequence(&our_instances[acidx], 0);
}

void
lmacProcessTxRtsError(uint32_t code, uint32_t acidx)
{
	code &= 0xff;
	acidx &= 0xff;
	if (code < 0x12) {
		if (code < 0x10) {
			if (code == 1 || code == 3) {
				lmacProcessCtsTimeout(acidx);
				return;
			}
			goto err;
		}
	} else if (code != 0xc0) {
err:
		esp_log_write(1, "wifi", "tx rts error 0x%x\n", code);
		LMAC_ASSERT(0x516);
	}
	our_instances[acidx].state = 6;
	lmacEndFrameExchangeSequence(&our_instances[acidx], 0);
}

void
lmacProcessTxTimeout(void)
{
	uint32_t bits;

	MEMW();
	bits = (LREG(REG_TX_TIMEOUT) >> 16) & 0xfff;
	MEMW();
	LREG(REG_TX_TIMEOUT) = LREG(REG_TX_TIMEOUT) & 0xf000ffff;
	if (bits != 0) {
		for (;;) {
			uint32_t low = (~bits + 1) & bits;	/* -bits & bits */
			int clz, bidx;
			LmacAc *ac;
			volatile uint32_t *acreg;

			if (bits == 0)
				break;
			clz = 0;
			{
				uint32_t v = low;

				while ((v & 0x80000000) == 0) {
					clz++;
					v <<= 1;
				}
			}
			bidx = 31 - clz;
			vPortETSIntrLock();
			MEMW();
			LREG(REG_3FF20C74) = LREG(REG_3FF20C74) | 0xc0000000;
			vPortETSIntrUnlock();
			ets_delay_us(0x22);
			vPortETSIntrLock();
			acreg = &REG_AC(REG_AC_BASE1, bidx);
			MEMW();
			*acreg = *acreg & 0xbfffffff;
			MEMW();
			LREG(REG_3FF20C74) = LREG(REG_3FF20C74) & 0x3fffffff;
			ac = &our_instances[bidx];
			ac->fai_cnt++;
			if ((ac->state == 1 && bidx == 7) ||
			    (int32_t)*acreg < 0) {
				MEMW();
				*acreg = *acreg & 0x7fffffff;
				ac->state = 6;
				if (ac->cur != NULL)
					lmacDiscardFrameExchangeSequence(ac);
			} else if (ac->cur != NULL) {
				PpNode *node = (PpNode *)ac->cur;

				node->ctrl[0] = node->ctrl[0] | 0x00020000;
			}
			vPortETSIntrUnlock();
			bits &= ~(1u << bidx);
		}
	}
}

void
lmacClrAc(void)
{
	LmacAc *ac0 = &our_instances[0];
	LmacAc *ac2 = &our_instances[2];
	int i;
	PpNode *node;

	if (ac0->cur != NULL) {
		lmacRecycleMPDU(ac0, ac0->cur, 0);
		ac0->cur = NULL;
	}
	i = 0x33;
	for (;;) {
		node = ppFetchTxQFirstAvail(0);
		if (node == NULL)
			break;
		i--;
		lmacRecycleMPDU(ac0, node, 0);
		if (i == 0)
			break;
	}
	ac0->state = 0;
	if (ac2->cur != NULL) {
		lmacRecycleMPDU(ac2, ac2->cur, 1);
		ac2->cur = NULL;
	}
	i = 0x33;
	for (;;) {
		node = ppFetchTxQFirstAvail(2);
		if (node == NULL)
			break;
		i--;
		lmacRecycleMPDU(ac2, node, 0);
		if (i == 0)
			break;
	}
	ac2->state = 0;
}

void
lmac_set_status(uint32_t ac, int failed)
{
	if (failed == 0)
		our_instances[ac].su_cnt++;
	else
		our_instances[ac].fai_cnt++;
	our_instances[ac].state = 2;
}

void
lmac_check_ac(void)
{
	LmacAc *ac = &our_instances[0];

	vPortETSIntrLock();
	if (ac->state != 0 &&
	    s_lmac_check_tx_hang[0] == (uint32_t)ac->cur &&
	    s_lmac_check_tx_hang[1] == ac->state &&
	    s_lmac_check_tx_hang[2] == ac->su_cnt &&
	    s_lmac_check_tx_hang[3] == ac->fai_cnt) {
		ets_printf("E:AC %d, %d, %d\n", ac->state,
			ac->su_cnt, ac->fai_cnt);
		MEMW();
		ets_printf("CONF:%x, PLCP:%x, rf:%x, rx:%x,%x, tx:%x\n",
			LREG(REG_CONF_DC0), LREG(REG_AC_BASE1),
			LREG(REG_PHY_60000710), LREG(REG_3FF201C0),
			LREG(REG_3FF201BC), LREG(REG_TX_TIMEOUT));
		MEMW();
		if (LREG(REG_PHY_60000710) != 0xfe000000) {
			clockgate_watchdog(1);
			phy_open_rf();
			clockgate_watchdog(0);
		}
		MEMW();
		LREG(REG_3FF20C70) = LREG(REG_3FF20C70) & 0xfffffffd;
		MEMW();
		LREG(REG_3FF20C70) = LREG(REG_3FF20C70) | 2;
		MEMW();
		LREG(REG_PHY_60000710) = 0;
		pm_wakeup_init(8);
		MEMW();
		LREG(REG_3FF20C74) = LREG(REG_3FF20C74) | 0xc0000000;
		ets_delay_us(0x22);
		MEMW();
		LREG(REG_AC_BASE1) = LREG(REG_AC_BASE1) & 0xbfffffff;
		MEMW();
		LREG(REG_3FF20C74) = LREG(REG_3FF20C74) & 0x3fffffff;
		ac->fai_cnt++;
		if (ac->state == 1) {
			MEMW();
			LREG(REG_3FF20C70) = LREG(REG_3FF20C70) & 0x7fffffff;
			ac->state = 6;
			if (ac->cur != NULL)
				lmacDiscardFrameExchangeSequence(ac);
		}
	}
	vPortETSIntrUnlock();
	s_lmac_check_tx_hang[0] = (uint32_t)ac->cur;
	s_lmac_check_tx_hang[1] = ac->state;
	s_lmac_check_tx_hang[2] = ac->su_cnt;
	s_lmac_check_tx_hang[3] = ac->fai_cnt;
}

/* ------------------------------------------------------------------ */

void *
GetAccess(uint32_t ac)
{
	return &our_instances[ac & 0xff];
}

int
lmacIsIdle(uint32_t ac)
{
	return our_instances[ac & 0xff].state == 0;
}

void
lmacInitAc(uint8_t ac, uint8_t aifs, uint8_t cwmin, uint8_t cwmax,
	   uint16_t txop)
{
	LmacAc *a = &our_instances[ac];

	a->ac = ac;
	a->qsrc = 0;
	a->qlrc = 0;
	a->state = 0;
	a->aifs = aifs;
	a->cwmin = cwmin;
	a->cwmax = cwmax;
	a->cw = cwmin;
	a->txop_max = txop;
	a->su_cnt = 0;
	a->fai_cnt = 0;
}

void
lmacInit(void)
{
	MIB32(0) = 0x400;
	MIB32(4) = 0x200;
	MIB32(8) = 0x200;
	MIB8(0x0d) = 0x20;
	MIB8(0x0c) = 0x20;
	MIB16(0x14) = 9;
	MIB16(0x10) = 0x92a;
	MIB16(0x0e) = 0x92a;
	MIB16(0x1c) = 0x15f;
	MIB16(0x1e) = 0x0fff;
	lmacInitAc(2, 3, 4, 10, 0);
	lmacInitAc(3, 7, 4, 10, 0);
	lmacInitAc(1, 2, 3, 4, 0x0bc0);
	lmacInitAc(0, 2, 2, 3, 0x5e0);
	lmacInitAc(4, 2, 4, 10, 0);
	lmacInitAc(7, 2, 5, 10, 0);
	RC_SetBasicRate(MIB16(0x1c), 1);
	rcAttach();
}

void
lmacSetAcParam(uint8_t ac, uint8_t aifs, uint8_t cwmin, uint8_t cwmax,
	       uint16_t txop)
{
	LmacAc *a;

	ac &= 0xff;
	cwmin &= 0xff;
	cwmax &= 0xff;
	vPortETSIntrLock();
	a = &our_instances[ac];
	a->aifs = aifs;
	a->cwmin = cwmin;
	a->cwmax = cwmax;
	if (cwmax < a->cw)
		a->cw = cwmax;
	else if (a->cw < cwmin)
		a->cw = cwmin;
	a->txop_max = txop;
	vPortETSIntrUnlock();
}

void
lmacSetRetryLimit(uint8_t limit)
{
	if (limit >= 0x15)
		limit = 0x14;
	else if (limit < 5)
		limit = 5;
	MIB8(0x0d) = limit;
	MIB8(0x0c) = limit;
}

void
esp_wifi_internal_set_tx_param(int32_t p1, int32_t p2, int32_t p3,
			       int8_t p4, int8_t p5)
{
	if (p2 != 0)
		MIB32(0) = (uint32_t)p2;
	if (p1 != 0)
		MIB32(4) = (uint32_t)p1;
	if (p3 != 0)
		MIB32(8) = (uint32_t)p3;
	if (p4 != 0)
		MIB8(0x0d) = (uint8_t)p4;
	if (p5 != 0)
		MIB8(0x0c) = (uint8_t)p4;	/* blob stores p4 again */
}

void
esp_wifi_set_11b_tx_plcp(uint8_t set_plcp, uint8_t with_long)
{
	s_tx_with_long_plcp = with_long;
	s_set_11b_plcp = set_plcp;
}

/* ------------------------------------------------------------------ */

void
dbg_display_acs(void)
{
	int i;

	printf("Access: \n");
	for (i = 0; i < 8; i++) {
		LmacAc *a = &our_instances[i];

		printf("%d: st:%d aifs:%d cw:%d cw_min:%d cw_max:%d "
			"qsrc:%d qlrc:%d\n", i, a->state, a->aifs, a->cw,
			a->cwmin, a->cwmax, a->qsrc, a->qlrc);
		printf("intxop=%d txop_enough=%d trytxop=%d acm=%d "
			"org=%d state=%d end_status=%d\n", a->intxop,
			a->txop_enough, a->trytxop, a->acm, a->org,
			a->state, a->end_status);
		printf("txop=%d txop_delta=%d txop_max=%d lifetime=%d "
			"su_cnt=%d fai_cnt=%d eb=0x%x\n", a->txop,
			a->txop_delta, a->txop_max, a->lifetime,
			a->su_cnt, a->fai_cnt, (uint32_t)a->cur);
	}
}

void
dbg_lmac_show(void)
{
	uint8_t *base = (uint8_t *)pTxRx;
	int i, q, n;
	PpNode *node;

	printf("T q en: 0x%x\n", *(uint32_t *)base);
	for (i = 0; i < 4; i++)
		printf("<0x%x, %d> ", *(uint32_t *)(base + i * 4 + 4),
			base[i + 0x14]);
	printf("txqs:\n");
	for (q = 0; q < 8; q++) {
		printf("PP_TXQ [%d]: \n", q);
		n = 0;
		node = *(PpNode **)(base + (q + 1) * 0x10 + 8);
		for (; node != NULL; node = node->next)
			printf("[%d]=%p\n", n++, (void *)node);
		printf("tid=%d con=%d ifidx=%d nonqos=%d active=%d\n",
			base[q * 0x10 + 0x20], base[q * 0x10 + 0x21],
			base[q * 0x10 + 0x22], base[q * 0x10 + 0x23],
			base[q * 0x10 + 0x24]);
	}
	printf("wait txq:\n");
	for (i = 0; i < 2; i++) {
		printf("%d wait_txq\n", i);
		n = 0;
		node = *(PpNode **)(base + i * 8 + 0x98);
		for (; node != NULL; node = node->next)
			printf("[%d]=%p\n", n++, (void *)node);
	}
	printf("done q:\n");
	n = 0;
	node = *(PpNode **)(base + 0xa8);
	for (; node != NULL; node = node->next)
		printf("[%d]=%p\n", n++, (void *)node);
	printf("rx q:\n");
	n = 0;
	node = *(PpNode **)(base + 0xb0);
	for (; node != NULL; node = node->next)
		printf("[%d]=%p\n", n++, (void *)node);
	printf("txCBmap=0x%x\n", *(uint32_t *)(base + 0xb8));
	printf("acs:\n");
	dbg_display_acs();
}
