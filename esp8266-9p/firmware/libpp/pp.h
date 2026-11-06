/* pp - clean-room replacement for the blob pp.o in
 * libpp.a (B37). Packet-processor queues + ppTask.
 *
 * Byte-exact by necessity: the TxRx context (240 bytes,
 * pTxRx) and the esf node layout are shared with
 * lmac/wdev by pointer. Layout from pp_attach's init
 * writes and the queue functions; see
 * re/inventory/b37/PP-NOTES.md.
 *
 * TxRx (byte offsets):
 *   +0x00 u32 union queue-enable mask
 *   +0x04.. u32 per-context queue masks [ctx+1]
 *   +0x14 ctx current-queue byte (per msg ctx 0..3)
 *   +0x18 eight 16-byte TX queues:
 *        +0 head, +4 tail (-> next-field), +8 state,
 *        +11 disabled flag, +12 active flag
 *   +0x98/+0xa0 wait queues (head, tail), per AC bit
 *   +0xa8 TX-done queue, +0xb0 RX queue
 *   +0xb8 TX-done callback mask, +0xbc callback table
 *   +0xe8/+0xec TX counters
 */
#ifndef PP_H
#define PP_H

#include <stdint.h>

typedef struct PpNode {
	uint32_t f0;		/* 0x00 */
	void *desc;		/* 0x04: first lldesc */
	uint32_t f8;		/* 0x08 */
	void *frame;		/* 0x0c */
	void *info;		/* 0x10 */
	uint8_t *hdr;		/* 0x14: 802.11 header */
	uint16_t len18;		/* 0x18 */
	uint16_t len;		/* 0x1a */
	uint32_t f1c;		/* 0x1c */
	void *trc;		/* 0x20: trc record or 0 */
	struct PpNode *next;	/* 0x24 */
	uint32_t *ctrl;		/* 0x28: pkt control */
} PpNode;

extern void *pTxRx;
extern uint16_t NoiseTimerInterval;

int MacIsrSigPostDefHdl(void);
void pp_attach(void);
void ppCalFrameTimes(PpNode *node);
uint32_t ppCalTxop(uint32_t q, uint32_t acc, uint32_t limit);
uint32_t ppCheckTxIdle(uint32_t lockflag);
PpNode *ppDequeueTxQ(uint32_t q);
void pp_disable_noise_timer(void);
void ppEnqueueRxq(PpNode *node);
void ppEnqueueTxDone(PpNode *node);
PpNode *ppFetchTxQFirstAvail(uint32_t q);
PpNode *ppGetTxQFirstAvail_Locked(uint32_t q);
void pp_noise_test(void);
int32_t pp_post(uint32_t type, uint32_t arg);
int32_t pp_post_from_isr(uint32_t type, uint32_t arg);
void ppProcessRxPktHdr(void *hdr);
int32_t ppProcessTxQ(uint32_t ctx);
void ppProcessWaitQ(void);
void ppRecycleRxPkt(PpNode *node);
void ppRollBackTxQ(PpNode *node);
int ppRegisterTxCallback(void *fn, uint32_t idx);
void ppTask(void);
int32_t ppTxPkt(PpNode *node);
void ppUnregisterTxCallback(uint32_t idx);
void reset_noise_timer(uint32_t ms);
int32_t TxNodeNum(void);

#endif
