/* trc_tables - clean-room schedule tables for the blob
 * trc.o rate adaptation (B31, stage 1: tables only).
 *
 * The blob's per-mode TX rate schedules are 12-byte
 * records: four (rate, retries) stages at +0..+7, the
 * total retry budget at +8, the PER-quality threshold
 * at +9, the entry's own index at +10 (written by
 * rcAttach at boot), and a flag byte at +11. Field
 * semantics were pinned in stage 2 (B36); see trc.h
 * and re/inventory/b36/STAGE2-NOTES.md.
 * Tables below are extracted verbatim from libpp.a
 * (.data.rc11{B,G,N}SchedTbl, .data.BasicOFDMSched,
 * .data.user_rc_limit, .rodata.CSWTCH index tables) -
 * they are the blob's data constants, re-expressed as
 * source. They are writable (not const): rcAttach
 * stamps the +10 indices and rc_set_rate_limit_id
 * rewrites user_rc_limit, exactly as in the blob.
 * Linked into esp9 since B36 via trc.c.
 */
#ifndef TRC_TABLES_H
#define TRC_TABLES_H

#include <stdint.h>

#define TRC_SCHED_ENTRIES_B 4
#define TRC_SCHED_ENTRIES_G 11
#define TRC_SCHED_ENTRIES_N 12

extern uint8_t rc11b_sched[TRC_SCHED_ENTRIES_B][12];
extern uint8_t rc11g_sched[TRC_SCHED_ENTRIES_G][12];
extern uint8_t rc11n_sched[TRC_SCHED_ENTRIES_N][12];
extern uint8_t basic_ofdm_sched[1][12];
extern uint8_t user_rc_limit[12];

/* Rate-index -> schedule-entry index, blob semantics:
 * 0xFF when the rate has no schedule entry. */
uint8_t rc11b_rate2schedidx(uint32_t rate);
uint8_t rc11g_rate2schedidx(uint32_t rate);

#endif
