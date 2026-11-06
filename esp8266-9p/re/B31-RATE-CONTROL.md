# B31 - rate_control.o replaced; trc tables extracted (stage 1)

Status: DONE (rate_control swapped and proven on
device; trc schedule tables in source with host tests;
the trc PER state machine is stage 2, not started).

## Inventory (re/inventory/b31)

rate_control.o (945 B text): six exports over a
256-byte BSS table `our_controls` (32 records x 8 B).
Only external caller is lmac.o (RC_SetBasicRate at
attach). trc.o (3963 B): 20 exports, schedule tables
in .data (rc11B 4 entries, rc11G 11, rc11N 12,
BasicOFDM 1, all 12-byte records), 648 B of per-station
state, undefined on g_ic/lmacConfMib/sig_max_rate2.

## rate_control replacement (firmware/libpp)

Record layout decoded from the getter assembly:
+0 u32 duration factor, +4 RTS rate index, +5 ACK rate
index (also indexes a 32-entry ACK-time table), +6
preamble class. RC_GetAckRate/RC_GetRtsRate are byte
reads; RC_GetAckTime is a table lookup;
RC_GetCtsTime/RC_GetBlockAckTime are airtime formulas
(preamble by rate class + SIFS + ceil(bits*factor /
2^18), 4-aligned for OFDM+) - transliterated from the
IRAM assembly instruction by instruction.

RC_SetBasicRate's default table was reconstructed
from the Ghidra decompilation of its store sequence
plus the literal-pool constants, and is embedded as
data. Its partial-mask branch (RTS-byte retune from
an ERP flag over eight OFDM records) is implemented
as the blob's else path. Host test vectors were
computed independently in Python from the extracted
arithmetic (test_rate: all pass, incl. CTS/BlockAck
airtime spot values).

Swap mechanics identical to esf (B24): symlinked into
components/freertos, linked directly via
EXTRA_LDFLAGS. Map check: zero libpp.a(rate_control.o)
references; ours at 0x402122a0.

On device (this build also carries esf + IL + stall
fix): join-ok, plan9port ls/read pass, and a full
300-iteration bench9p DONE with Tread median 15.7 ms
(blob: 15.4 ms). Every transmitted frame's ACK/CTS
timing now comes from our table and formulas.

## trc stage 1 (tables only)

firmware/libpp/trc_tables.{h,c}: the four schedule
tables, user_rc_limit, and the B/G rate->schedule
index tables (from .rodata.CSWTCH$43/$41), byte-exact
against the archive dumps (test_trc_tables). Only two
record fields are claimed so far: +0 (rate byte
rcGetRate returns) and +8 (budget word). The stage-2
state machine (rcGetSched/rcUpdateTxDone PER logic,
per-station 54-byte state, TSF-driven schedule
stepping, g_ic interactions) is a bigger decode and
remains blob until it can be done without guessing.

## Libpp scoreboard

Replaced: esf_buf.o, rate_control.o (2/9).
Remaining: trc (stage 2), pp, if_hwctrl, lmac, pm,
wdev. mac_frame.o is empty - nothing to replace.
