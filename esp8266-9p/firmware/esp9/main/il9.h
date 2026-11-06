/* il9 - minimal improved-IL server endpoint (DESIGN 4.1).
 *
 * One connection, server side only, written against Harvey
 * sys/src/9/ip/il.c semantics: 18-byte header, packet types
 * Ilsync..Ilclose, SYNC offers Ilflow (0x80) both ways,
 * ACK/STATE carry an 8-byte cumulative consumed-bytes
 * credit once both SYNCs offered it, delayed ACK (100 ms),
 * retransmit of the single unacked DATA (as Ildataquery),
 * query after 10 s of silence, close handshake.
 *
 * Deliberate subset (DESIGN 4.1): no congestion window to
 * speak of (one unacked message, one pending), no
 * out-of-order queue (drop + ACK what we have), no RTT
 * adaptation (fixed 1 s retransmit, doubling to 30 s,
 * 16 retransmits then hang up - for DATA, and for the
 * Sync reply while SYNCEE, so a peer that dies
 * mid-handshake cannot wedge the listener). A Harvey 9P client issues
 * one RPC at a time per channel in practice; anything it
 * pipelines beyond that is retransmitted to us.
 *
 * The engine is transport-free: the caller supplies a
 * send hook (one IP-protocol-40 payload starting at the
 * IL header), a clock, and message buffers. IL payloads
 * are whole 9P messages; framing is the caller's job on
 * the byte-stream side.
 */
#ifndef IL9_H
#define IL9_H

#include <stdint.h>

#define IL9_HDRSIZE 18
#define IL9_PROTO 40
#define IL9_MAXPAYLOAD 2048
#define IL9_BUFMAX (IL9_HDRSIZE + IL9_MAXPAYLOAD)

enum {
	IL9_CLOSED = 0,		/* listening */
	IL9_SYNCEE,
	IL9_ESTABLISHED,
	IL9_CLOSING
};

typedef struct Il9 Il9;
struct Il9 {
	/* hooks (set by caller before il9_listen) */
	void *ctx;
	void (*sendpkt)(void *ctx, uint32_t dst_ip,
			const uint8_t *pkt, uint32_t len);
	void (*deliver)(void *ctx, const uint8_t *msg, uint32_t len);
	void (*hangup)(void *ctx);	/* connection dropped/reset */

	uint16_t lport;		/* our port (564) */
	uint16_t rport;		/* peer port */
	uint32_t peer_ip;	/* host-order IPv4 of peer */
	uint32_t start;		/* our initial sequence id */
	uint32_t rstart;	/* peer initial sequence id */
	uint32_t next;		/* next id to send */
	uint32_t recvd;		/* last in-order id received */
	int state;
	int flowpeer;		/* peer offered Ilflow on SYNC */
	uint64_t rxconsumed;	/* bytes delivered to the 9P side */
	uint64_t peerread;	/* peer's advertised consumed count */
	uint64_t txbytes;	/* payload bytes sent (first tries) */

	/* one unacked DATA plus one pending message; the
	 * caller owns this storage (2 * IL9_BUFMAX bytes). */
	uint8_t *unacked;	/* full packet incl. header */
	uint32_t unacked_len;
	uint32_t unacked_id;
	int rexmit;		/* retransmissions of unacked */
	uint32_t rexmit_due;	/* ms timestamp */
	uint32_t rexmit_ivl;	/* current interval ms */
	uint8_t *pending;
	uint32_t pending_len;

	uint32_t ack_due;	/* delayed-ACK deadline, 0 = none */
	uint32_t lastrecv;	/* ms of last packet received */
	uint32_t query_due;	/* next Ilquery time */
	int queries;		/* unanswered queries sent */
	uint8_t qtspec;		/* rotating query tag 1..8 */
	uint32_t qt[9];		/* ack value when query tag sent */
};

/* start_id is our initial sequence number (seed it from
 * anything varying: clock ^ MAC). Buffers are caller
 * storage of IL9_BUFMAX bytes each; pending may be NULL,
 * in which case il9_sendmsg simply refuses (returns -1)
 * while a message is unacked instead of queueing one. */
void il9_init(Il9 *il, uint32_t start_id, uint8_t *unacked,
	      uint8_t *pending);
/* Feed one received IL packet (starting at the IL
 * header, IP header already stripped). A SYNC in CLOSED
 * state is the only way in. */
void il9_input(Il9 *il, uint32_t src_ip, uint16_t src_port,
	       const uint8_t *pkt, uint32_t len, uint32_t now);
/* Drive timers; call regularly (every ~10 ms). */
void il9_poll(Il9 *il, uint32_t now);
/* Queue one complete 9P message for sending. Returns 0
 * if accepted (sent now or held pending), -1 if both
 * slots are busy or not established - the caller must
 * retry later; nothing is dropped silently. */
int il9_sendmsg(Il9 *il, const uint8_t *msg, uint32_t len,
		uint32_t now);
/* Local close: send Ilclose and move to CLOSING. */
void il9_close(Il9 *il, uint32_t now);
int il9_state(const Il9 *il);

#endif
