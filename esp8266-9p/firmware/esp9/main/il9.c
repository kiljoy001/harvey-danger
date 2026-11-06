/* il9 - see il9.h. Implemented from sys/src/9/ip/il.c
 * (Harvey) packet/state semantics; no code copied. */
#include "il9.h"

#include <string.h>

/* packet types (il.c) */
enum {
	T_SYNC, T_DATA, T_DATAQUERY, T_ACK, T_QUERY, T_STATE, T_CLOSE
};

#define ILFLOW 0x80u
#define CREDITLEN 8
#define ACK_DELAY_MS 100u
#define REXMIT_INIT_MS 1000u
#define REXMIT_MAX_MS 30000u
#define MAX_REXMIT 16
#define QUERY_MS 10000u
#define MAX_QUERIES 6	/* ~60 s of silence = dead peer */

static uint16_t
get16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t
get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static void
put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* ptclcsum over the IL header+payload (il.c: checksum of
 * ilheader plus data, no IP or pseudo header). */
static uint16_t
il_csum(const uint8_t *p, uint32_t len)
{
	uint32_t sum = 0;

	while (len >= 2) {
		sum += (uint32_t)((p[0] << 8) | p[1]);
		p += 2;
		len -= 2;
	}
	if (len)
		sum += (uint32_t)(p[0] << 8);
	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	return (uint16_t)~sum;
}

static void
send_ctl(Il9 *il, int type, uint32_t id, uint32_t ack, int spec)
{
	uint8_t pkt[IL9_HDRSIZE + CREDITLEN];
	uint32_t len = IL9_HDRSIZE;

	memset(pkt, 0, sizeof(pkt));
	if (il->flowpeer && (type == T_ACK || type == T_STATE)) {
		put32(pkt + IL9_HDRSIZE, (uint32_t)(il->rxconsumed >> 32));
		put32(pkt + IL9_HDRSIZE + 4, (uint32_t)il->rxconsumed);
		len += CREDITLEN;
	}
	put16(pkt + 2, (uint16_t)len);
	pkt[4] = (uint8_t)type;
	pkt[5] = (uint8_t)(type == T_SYNC ? (int)ILFLOW : spec);
	put16(pkt + 6, il->lport);
	put16(pkt + 8, il->rport);
	put32(pkt + 10, id);
	put32(pkt + 14, ack);
	put16(pkt, il_csum(pkt, len));
	il->sendpkt(il->ctx, il->peer_ip, pkt, len);
	il->ack_due = 0;
}

static void
hangup(Il9 *il)
{
	il->state = IL9_CLOSED;
	il->unacked_len = 0;
	il->pending_len = 0;
	il->ack_due = 0;
	if (il->hangup != NULL)
		il->hangup(il->ctx);
}

/* Process an ack value: drop the unacked message if the
 * peer has it, then pump the pending one. */
static void
ackto(Il9 *il, uint32_t ack, uint32_t now)
{
	if (il->unacked_len > 0 && ack >= il->unacked_id) {
		il->unacked_len = 0;
		il->rexmit = 0;
		il->rexmit_ivl = REXMIT_INIT_MS;
	}
	if (il->unacked_len == 0 && il->pending_len > 0) {
		uint32_t len = il->pending_len;
		uint8_t tmp[IL9_BUFMAX];

		/* pending holds a raw payload; move via temp
		 * because sendmsg reuses the pending slot */
		memcpy(tmp, il->pending, len);
		il->pending_len = 0;
		(void)il9_sendmsg(il, tmp, len, now);
	}
}

static int
credit(Il9 *il, const uint8_t *pkt, uint32_t len)
{
	uint64_t read;

	if (!il->flowpeer || len < IL9_HDRSIZE + CREDITLEN)
		return 0;
	read = ((uint64_t)get32(pkt + IL9_HDRSIZE) << 32) |
	       get32(pkt + IL9_HDRSIZE + 4);
	if (read > il->txbytes)
		read = il->txbytes;
	if (read <= il->peerread)
		return 0;
	il->peerread = read;
	return 1;
}

void
il9_init(Il9 *il, uint32_t start_id, uint8_t *unacked, uint8_t *pending)
{
	memset(il, 0, sizeof(*il));
	il->start = start_id;
	il->next = start_id + 1;
	il->unacked = unacked;
	il->pending = pending;
	il->rexmit_ivl = REXMIT_INIT_MS;
	il->state = IL9_CLOSED;
}

int
il9_state(const Il9 *il)
{
	return il->state;
}

int
il9_sendmsg(Il9 *il, const uint8_t *msg, uint32_t len, uint32_t now)
{
	uint32_t plen;

	if (il->state != IL9_ESTABLISHED || len > IL9_MAXPAYLOAD)
		return -1;
	if (il->unacked_len > 0) {
		if (il->pending == NULL || il->pending_len > 0)
			return -1;
		memcpy(il->pending, msg, len);
		il->pending_len = len;
		return 0;
	}
	plen = IL9_HDRSIZE + len;
	memset(il->unacked, 0, IL9_HDRSIZE);
	put16(il->unacked + 2, (uint16_t)plen);
	il->unacked[4] = T_DATA;
	il->unacked[5] = 0;
	put16(il->unacked + 6, il->lport);
	put16(il->unacked + 8, il->rport);
	il->unacked_id = il->next++;
	put32(il->unacked + 10, il->unacked_id);
	put32(il->unacked + 14, il->recvd);
	memcpy(il->unacked + IL9_HDRSIZE, msg, len);
	put16(il->unacked, il_csum(il->unacked, plen));
	il->unacked_len = plen;
	il->txbytes += len;
	il->rexmit = 0;
	il->rexmit_ivl = REXMIT_INIT_MS;
	il->rexmit_due = now + REXMIT_INIT_MS;
	il->sendpkt(il->ctx, il->peer_ip, il->unacked, plen);
	return 0;
}

void
il9_close(Il9 *il, uint32_t now)
{
	(void)now;
	if (il->state == IL9_ESTABLISHED || il->state == IL9_SYNCEE) {
		send_ctl(il, T_CLOSE, il->next, il->recvd, 0);
		il->state = IL9_CLOSING;
		il->rexmit_due = now + REXMIT_INIT_MS;
	}
}

void
il9_input(Il9 *il, uint32_t src_ip, uint16_t src_port,
	  const uint8_t *pkt, uint32_t len, uint32_t now)
{
	uint32_t illen, id, ack;
	int type, spec;

	if (len < IL9_HDRSIZE)
		return;
	illen = get16(pkt + 2);
	if (illen < IL9_HDRSIZE || illen > len)
		return;
	if (il_csum(pkt, illen) != 0)
		return;
	type = pkt[4];
	spec = pkt[5];
	if (type > T_CLOSE)
		return;
	/* our listen port is pkt dst (network order fields) */
	if (get16(pkt + 8) != il->lport && il->state == IL9_CLOSED)
		return;
	id = get32(pkt + 10);
	ack = get32(pkt + 14);

	if (il->state == IL9_CLOSED) {
		if (type != T_SYNC)
			return;	/* no listener for this peer yet */
		il->peer_ip = src_ip;
		il->rport = src_port;
		il->next = il->start + 1;
		il->rstart = id;
		il->recvd = id;
		il->flowpeer = (spec & ILFLOW) != 0;
		il->rxconsumed = 0;
		il->peerread = 0;
		il->txbytes = 0;
		il->unacked_len = 0;
		il->pending_len = 0;
		il->queries = 0;
		il->qtspec = 1;
		il->state = IL9_SYNCEE;
		il->lastrecv = now;
		il->query_due = now + QUERY_MS;
		/* Arm the Sync-reply retransmit timer (il.c
		 * Ilsyncee/Ilsyncer in the ackproc: resend Sync
		 * on timeout with backoff, hang up after
		 * MaxRexmit). Without it a peer that dies
		 * mid-handshake wedges this one connection
		 * forever (B33). rexmit fields are idle here:
		 * no DATA can be unacked before ESTABLISHED. */
		il->rexmit = 0;
		il->rexmit_ivl = REXMIT_INIT_MS;
		il->rexmit_due = now + REXMIT_INIT_MS;
		send_ctl(il, T_SYNC, il->start, il->recvd, 0);
		return;
	}

	/* established/syncee/closing: only our peer talks */
	if (src_ip != il->peer_ip || src_port != il->rport)
		return;
	il->lastrecv = now;
	il->queries = 0;
	il->query_due = now + QUERY_MS;

	switch (il->state) {
	case IL9_SYNCEE:
		switch (type) {
		case T_SYNC:
			if (id != il->rstart || ack != 0)
				hangup(il);
			else {
				/* peer is alive and still syncing:
				 * re-reply and restart the timer */
				send_ctl(il, T_SYNC, il->start, il->recvd, 0);
				il->rexmit = 0;
				il->rexmit_ivl = REXMIT_INIT_MS;
				il->rexmit_due = now + REXMIT_INIT_MS;
			}
			break;
		case T_ACK:
			if (ack == il->start) {
				il->state = IL9_ESTABLISHED;
				il->rexmit = 0;
				il->rexmit_ivl = REXMIT_INIT_MS;
			}
			break;
		case T_DATA:
			if (ack == il->start) {
				il->state = IL9_ESTABLISHED;
				il->rexmit = 0;
				il->rexmit_ivl = REXMIT_INIT_MS;
				goto established_data;
			}
			break;
		case T_CLOSE:
			if (ack == il->start)
				hangup(il);
			break;
		}
		break;
	case IL9_ESTABLISHED:
		switch (type) {
		case T_SYNC:
			if (id != il->rstart)
				hangup(il);
			else
				send_ctl(il, T_ACK, il->next, il->rstart, 0);
			break;
		case T_DATA:
		established_data:
			ackto(il, ack, now);
			if (id == il->recvd + 1) {
				il->recvd = id;
				il->rxconsumed += illen - IL9_HDRSIZE;
				il->ack_due = now + ACK_DELAY_MS;
				if (il->deliver != NULL)
					il->deliver(il->ctx,
						    pkt + IL9_HDRSIZE,
						    illen - IL9_HDRSIZE);
			} else {
				/* duplicate or out of order:
				 * re-ACK immediately what we hold */
				send_ctl(il, T_ACK, il->next, il->recvd, 0);
			}
			break;
		case T_DATAQUERY:
			ackto(il, ack, now);
			if (id == il->recvd + 1) {
				il->recvd = id;
				il->rxconsumed += illen - IL9_HDRSIZE;
				if (il->deliver != NULL)
					il->deliver(il->ctx,
						    pkt + IL9_HDRSIZE,
						    illen - IL9_HDRSIZE);
			}
			send_ctl(il, T_STATE, il->next, il->recvd, spec);
			break;
		case T_ACK:
			(void)credit(il, pkt, illen);
			ackto(il, ack, now);
			break;
		case T_QUERY:
			ackto(il, ack, now);
			send_ctl(il, T_STATE, il->next, il->recvd, spec);
			break;
		case T_STATE:
			/* peer says it has acked up to qt[spec];
			 * if that predates our unacked, rexmit */
			(void)credit(il, pkt, illen);
			ackto(il, ack, now);
			/* il.c: qt[spec] holds our highest sent
			 * id when the query went out; if the
			 * peer's ack is still behind it, loss is
			 * confirmed and we retransmit. */
			if (spec >= 0 && spec <= 8 &&
			    il->unacked_len > 0 &&
			    il->qt[spec] > ack) {
				il->rexmit_due = now;	/* force in poll */
			}
			break;
		case T_CLOSE:
			if (ack >= il->start && ack <= il->next) {
				il->recvd = id;
				send_ctl(il, T_CLOSE, il->next, il->recvd, 0);
				il->state = IL9_CLOSING;
				il->rexmit_due = now + REXMIT_INIT_MS;
			}
			break;
		}
		break;
	case IL9_CLOSING:
		if (type == T_CLOSE) {
			il->recvd = id;
			send_ctl(il, T_CLOSE, il->next, il->recvd, 0);
			if (ack == il->next)
				hangup(il);
		}
		break;
	}
}

void
il9_poll(Il9 *il, uint32_t now)
{
	if (il->state == IL9_CLOSED)
		return;
	/* Syncee: retransmit our Sync reply on the doubling
	 * schedule; hang up after MAX_REXMIT so a peer that
	 * died mid-handshake cannot wedge the listener
	 * (il.c ackproc Ilsyncee case). */
	if (il->state == IL9_SYNCEE &&
	    (int32_t)(now - il->rexmit_due) >= 0) {
		if (il->rexmit >= MAX_REXMIT) {
			hangup(il);
			return;
		}
		send_ctl(il, T_SYNC, il->start, il->recvd, 0);
		il->rexmit++;
		il->rexmit_ivl *= 2;
		if (il->rexmit_ivl > REXMIT_MAX_MS)
			il->rexmit_ivl = REXMIT_MAX_MS;
		il->rexmit_due = now + il->rexmit_ivl;
	}
	if (il->ack_due != 0 && (int32_t)(now - il->ack_due) >= 0) {
		send_ctl(il, T_ACK, il->next, il->recvd, 0);
	}
	/* retransmit the unacked DATA as a dataquery
	 * (ilrexmit), with backoff; hang up after MAX_REXMIT */
	if (il->unacked_len > 0 &&
	    (int32_t)(now - il->rexmit_due) >= 0) {
		if (il->rexmit >= MAX_REXMIT) {
			hangup(il);
			return;
		}
		il->unacked[4] = T_DATAQUERY;
		il->qtspec = (uint8_t)(il->qtspec % 8 + 1);
		il->unacked[5] = il->qtspec;
		il->qt[il->qtspec] = il->unacked_id;	/* highest sent */
		il->qt[0] = il->qt[il->qtspec];
		put32(il->unacked + 14, il->recvd);
		il->unacked[0] = 0;
		il->unacked[1] = 0;
		put16(il->unacked, il_csum(il->unacked, il->unacked_len));
		il->sendpkt(il->ctx, il->peer_ip, il->unacked,
			    il->unacked_len);
		il->rexmit++;
		il->rexmit_ivl *= 2;
		if (il->rexmit_ivl > REXMIT_MAX_MS)
			il->rexmit_ivl = REXMIT_MAX_MS;
		il->rexmit_due = now + il->rexmit_ivl;
	}
	/* peer silence probing (ilquery / DeathTime, shrunk
	 * to ~60 s so a dead peer frees the one connection) */
	if (il->state == IL9_ESTABLISHED &&
	    (int32_t)(now - il->query_due) >= 0) {
		if (il->queries >= MAX_QUERIES) {
			hangup(il);
			return;
		}
		il->qtspec = (uint8_t)(il->qtspec % 8 + 1);
		il->qt[il->qtspec] = il->next - 1;	/* highest sent */
		il->qt[0] = il->qt[il->qtspec];
		send_ctl(il, T_QUERY, il->next, il->recvd, il->qtspec);
		il->queries++;
		il->query_due = now + QUERY_MS;
	}
	if (il->state == IL9_CLOSING &&
	    (int32_t)(now - il->rexmit_due) >= 0) {
		/* peer never finished the close; drop it */
		hangup(il);
	}
}
