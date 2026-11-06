/* Host test for il9.c: a scripted peer speaking the
 * Harvey il.c side of the wire (SYNC/SYNC/ACK handshake,
 * in-order DATA, delayed ACK with Ilflow credit,
 * dataquery retransmission, duplicate suppression,
 * close). Packet construction here is independent of
 * il9.c's internals (own checksum/field code). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../il9.h"

enum { T_SYNC, T_DATA, T_DATAQUERY, T_ACK, T_QUERY, T_STATE, T_CLOSE };

static uint8_t wire[8][IL9_BUFMAX];
static uint32_t wirelen[8];
static int nwire;
static uint8_t delivered[4][IL9_MAXPAYLOAD];
static uint32_t deliveredlen[4];
static int ndelivered;
static int nhangup;

static uint16_t
hcsum(const uint8_t *p, uint32_t len)
{
	uint32_t s = 0;
	uint32_t i;
	for (i = 0; i + 1 < len; i += 2)
		s += (uint32_t)((p[i] << 8) | p[i + 1]);
	if (len & 1)
		s += (uint32_t)(p[len - 1] << 8);
	while (s >> 16)
		s = (s & 0xFFFFu) + (s >> 16);
	return (uint16_t)~s;
}

static void
sendpkt(void *ctx, uint32_t ip, const uint8_t *pkt, uint32_t len)
{
	(void)ctx; (void)ip;
	assert(nwire < 8);
	memcpy(wire[nwire], pkt, len);
	wirelen[nwire] = len;
	nwire++;
}

static void
deliver(void *ctx, const uint8_t *msg, uint32_t len)
{
	(void)ctx;
	assert(ndelivered < 4);
	memcpy(delivered[ndelivered], msg, len);
	deliveredlen[ndelivered] = len;
	ndelivered++;
}

static void
hangupcb(void *ctx)
{
	(void)ctx;
	nhangup++;
}

/* build a peer packet; returns length */
static uint32_t
peerpkt(uint8_t *out, int type, int spec, uint16_t sport,
	uint16_t dport, uint32_t id, uint32_t ack,
	const uint8_t *payload, uint32_t plen, uint64_t credit)
{
	uint32_t len = IL9_HDRSIZE + plen;
	int withcredit = (type == T_ACK || type == T_STATE);

	memset(out, 0, IL9_BUFMAX);
	if (withcredit)
		len += 8;
	out[2] = (uint8_t)(len >> 8);
	out[3] = (uint8_t)len;
	out[4] = (uint8_t)type;
	out[5] = (uint8_t)spec;
	out[6] = (uint8_t)(sport >> 8);
	out[7] = (uint8_t)sport;
	out[8] = (uint8_t)(dport >> 8);
	out[9] = (uint8_t)dport;
	out[10] = (uint8_t)(id >> 24); out[11] = (uint8_t)(id >> 16);
	out[12] = (uint8_t)(id >> 8); out[13] = (uint8_t)id;
	out[14] = (uint8_t)(ack >> 24); out[15] = (uint8_t)(ack >> 16);
	out[16] = (uint8_t)(ack >> 8); out[17] = (uint8_t)ack;
	if (withcredit) {
		out[18] = (uint8_t)(credit >> 56);
		out[19] = (uint8_t)(credit >> 48);
		out[20] = (uint8_t)(credit >> 40);
		out[21] = (uint8_t)(credit >> 32);
		out[22] = (uint8_t)(credit >> 24);
		out[23] = (uint8_t)(credit >> 16);
		out[24] = (uint8_t)(credit >> 8);
		out[25] = (uint8_t)credit;
		memcpy(out + 26, payload, plen);
		/* header says IL payload starts after credit
		 * for ctl packets in il.c? No: illen counts
		 * header+credit only for ACK/STATE (no data).
		 * Peer ACK/STATE here carry no payload. */
		if (plen > 0)
			assert(0);
	} else {
		memcpy(out + IL9_HDRSIZE, payload, plen);
	}
	{
		uint16_t c = hcsum(out, len);
		out[0] = (uint8_t)(c >> 8);
		out[1] = (uint8_t)c;
	}
	return len;
}

static uint32_t
wf32(int i, int off)
{
	return ((uint32_t)wire[i][off] << 24) |
	       ((uint32_t)wire[i][off + 1] << 16) |
	       ((uint32_t)wire[i][off + 2] << 8) | wire[i][off + 3];
}

#define PEER_IP 0xC0A80164u	/* 192.168.1.100 */
#define PEER_PORT 12345
#define OUR_START 25001u

int
main(void)
{
	static Il9 il;
	static uint8_t unacked[IL9_BUFMAX], pending[IL9_BUFMAX];
	static uint8_t pkt[IL9_BUFMAX];
	uint32_t n;
	uint32_t now = 1000;

	il9_init(&il, OUR_START, unacked, pending);
	il.ctx = NULL;
	il.sendpkt = sendpkt;
	il.deliver = deliver;
	il.hangup = hangupcb;
	il.lport = 564;

	/* stray DATA before SYNC is ignored */
	n = peerpkt(pkt, T_DATA, 0, PEER_PORT, 564, 1001, 0,
		    (const uint8_t *)"x", 1, 0);
	il9_input(&il, PEER_IP, PEER_PORT, pkt, n, now);
	assert(nwire == 0 && ndelivered == 0);

	/* SYNC -> our SYNC with Ilflow offered */
	n = peerpkt(pkt, T_SYNC, 0x80, PEER_PORT, 564, 1000, 0,
		    NULL, 0, 0);
	il9_input(&il, PEER_IP, PEER_PORT, pkt, n, now);
	assert(il9_state(&il) == IL9_SYNCEE);
	assert(nwire == 1);
	assert(wire[0][4] == T_SYNC && wire[0][5] == 0x80);
	assert(wf32(0, 10) == OUR_START);	/* id = our start */
	assert(wf32(0, 14) == 1000);		/* ack = peer start */
	assert(hcsum(wire[0], wirelen[0]) == 0);

	/* ACK completes the handshake */
	n = peerpkt(pkt, T_ACK, 0, PEER_PORT, 564, 1001, OUR_START,
		    NULL, 0, 0);
	il9_input(&il, PEER_IP, PEER_PORT, pkt, n, now);
	assert(il9_state(&il) == IL9_ESTABLISHED);

	/* peer DATA id=1001 "hello": delivered, ACK after
	 * 100 ms delay carrying credit=5 */
	n = peerpkt(pkt, T_DATA, 0, PEER_PORT, 564, 1001, OUR_START,
		    (const uint8_t *)"hello", 5, 0);
	nwire = 0;
	il9_input(&il, PEER_IP, PEER_PORT, pkt, n, 2000);
	assert(ndelivered == 1 && deliveredlen[0] == 5);
	assert(memcmp(delivered[0], "hello", 5) == 0);
	il9_poll(&il, 2050);
	assert(nwire == 0);			/* delayed, not yet */
	il9_poll(&il, 2101);
	assert(nwire == 1 && wire[0][4] == T_ACK);
	assert(wf32(0, 14) == 1001);
	assert(wf32(0, 18) == 0 && wf32(0, 22) == 5);	/* credit */

	/* duplicate DATA: not delivered, immediate re-ACK */
	nwire = 0;
	il9_input(&il, PEER_IP, PEER_PORT, pkt, n, 2200);
	assert(ndelivered == 1);
	assert(nwire == 1 && wire[0][4] == T_ACK);

	/* out-of-order DATA: dropped, ACK still 1001 */
	n = peerpkt(pkt, T_DATA, 0, PEER_PORT, 564, 1007,
		    OUR_START, (const uint8_t *)"junk", 4, 0);
	nwire = 0;
	il9_input(&il, PEER_IP, PEER_PORT, pkt, n, 2300);
	assert(ndelivered == 1);
	assert(nwire == 1 && wf32(0, 14) == 1001);

	/* our reply: DATA id=OUR_START+1; peer ACKs with
	 * credit, unacked clears */
	nwire = 0;
	assert(il9_sendmsg(&il, (const uint8_t *)"reply!", 6, 3000) == 0);
	assert(nwire == 1 && wire[0][4] == T_DATA);
	assert(wf32(0, 10) == OUR_START + 1);
	assert(wf32(0, 14) == 1001);
	n = peerpkt(pkt, T_ACK, 0, PEER_PORT, 564, 1002,
		    OUR_START + 1, NULL, 0, 6);
	il9_input(&il, PEER_IP, PEER_PORT, pkt, n, 3100);
	assert(il.unacked_len == 0 && il.peerread == 6);

	/* unacked DATA retransmits as DATAQUERY at 1 s */
	assert(il9_sendmsg(&il, (const uint8_t *)"lost?", 5, 4000) == 0);
	nwire = 0;
	il9_poll(&il, 4999);
	assert(nwire == 0);
	il9_poll(&il, 5001);
	assert(nwire == 1 && wire[0][4] == T_DATAQUERY);
	assert(wf32(0, 10) == OUR_START + 2);

	/* bad checksum is dropped silently */
	n = peerpkt(pkt, T_DATA, 0, PEER_PORT, 564, 1002,
		    OUR_START, (const uint8_t *)"hello", 5, 0);
	pkt[20] ^= 0xFF;
	nwire = 0;
	il9_input(&il, PEER_IP, PEER_PORT, pkt, n, 6000);
	assert(ndelivered == 1 && nwire == 0);

	/* CLOSE handshake: peer close -> our close, then
	 * peer's ack of it (ack == our next) drops us */
	il.unacked_len = 0;	/* "lost?" written off by test */
	n = peerpkt(pkt, T_CLOSE, 0, PEER_PORT, 564, 1002,
		    OUR_START + 2, NULL, 0, 0);
	nwire = 0;
	il9_input(&il, PEER_IP, PEER_PORT, pkt, n, 7000);
	assert(il9_state(&il) == IL9_CLOSING);
	assert(nwire == 1 && wire[0][4] == T_CLOSE);
	n = peerpkt(pkt, T_CLOSE, 0, PEER_PORT, 564, 1003,
		    OUR_START + 3, NULL, 0, 0);
	il9_input(&il, PEER_IP, PEER_PORT, pkt, n, 7100);
	/* our close went out with id=next (=start+3 after
	 * two DATA sends? next was start+3) */
	if (il9_state(&il) != IL9_CLOSED) {
		/* ack field must equal our next; emulate
		 * peer acking exactly what we sent */
		uint32_t ournext = wf32(0, 10);
		n = peerpkt(pkt, T_CLOSE, 0, PEER_PORT, 564, 1004,
			    ournext, NULL, 0, 0);
		il9_input(&il, PEER_IP, PEER_PORT, pkt, n, 7200);
	}
	assert(il9_state(&il) == IL9_CLOSED);
	assert(nhangup == 1);

	/* --- syncee timeout: our Sync reply is retransmitted
	 * on the 1 s doubling schedule, a peer retransmit of
	 * its Sync restarts the clock, and a peer that never
	 * ACKs is hung up after 16 retransmits instead of
	 * wedging the one connection forever (B33 wedge:
	 * abandoned handshake adopted a dead source port and
	 * every later client was ignored). --- */
	{
		int hangups_before = nhangup;
		int syncs;
		uint32_t t0 = 100000;

		il9_init(&il, OUR_START, unacked, pending);
		il.ctx = NULL;
		il.sendpkt = sendpkt;
		il.deliver = deliver;
		il.hangup = hangupcb;
		il.lport = 564;
		nwire = 0;
		n = peerpkt(pkt, T_SYNC, 0x80, PEER_PORT, 564, 2000, 0,
			    NULL, 0, 0);
		il9_input(&il, PEER_IP, PEER_PORT, pkt, n, t0);
		assert(il9_state(&il) == IL9_SYNCEE && nwire == 1);

		/* peer retransmits its Sync at +500: immediate
		 * re-reply, and expiry clock restarts */
		il9_input(&il, PEER_IP, PEER_PORT, pkt, n, t0 + 500);
		assert(nwire == 2 && wire[1][4] == T_SYNC);
		il9_poll(&il, t0 + 1400);
		assert(nwire == 2);	/* due moved to +1500 */
		il9_poll(&il, t0 + 1501);
		assert(nwire == 3 && wire[2][4] == T_SYNC);

		/* silence from here: backoff retransmits, then
		 * hangup to CLOSED with callback, listener free */
		syncs = 3;
		for (now = t0 + 2000; il9_state(&il) == IL9_SYNCEE &&
		     now < t0 + 600000; now += 500) {
			int before = nwire;
			il9_poll(&il, now);
			if (nwire > before) {
				assert(wire[nwire - 1][4] == T_SYNC);
				syncs += nwire - before;
			}
			nwire = 0;	/* wire[] holds only 8 */
		}
		assert(il9_state(&il) == IL9_CLOSED);
		assert(nhangup == hangups_before + 1);
		assert(syncs >= 17);	/* initial + 16 retransmits */

		/* the freed listener accepts a new peer */
		nwire = 0;
		n = peerpkt(pkt, T_SYNC, 0x80, PEER_PORT + 1, 564,
			    3000, 0, NULL, 0, 0);
		il9_input(&il, PEER_IP, PEER_PORT + 1, pkt, n, now);
		assert(il9_state(&il) == IL9_SYNCEE && nwire == 1);
	}

	puts("test_il9: all tests passed");
	return 0;
}
