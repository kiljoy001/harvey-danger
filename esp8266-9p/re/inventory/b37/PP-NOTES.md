# pp.o stage 1 - inventory and queue architecture

pp.o: 7860 bytes, 24 exports + internal statics.
Corpus: pp-objdump.txt (objdump -dr), Ghidra
decompiles of all 91 functions (regenerable exactly
as trc: analyzeHeadless on pp.o, Xtensa:LE:32:default).

## TxRx struct (pointed to by global pTxRx)

From pp_attach's initialisation writes:

- +0x00 u32: interface/queue bitmask word 0; +0x04..
  +0x13: four u32 per-interface queue masks
  (attach zeroes words 1..4); bytes +0x14+ifidx:
  per-interface current-queue byte (attach sets
  bytes 0x15,0x16,0x17 = 3,5,6); byte +0x20 = 7,
  +0x30 = 7 (two more bytes at word offsets 8, 12);
  byte +0x23 = 1, byte +0x33 = 1.
- +0x18: eight 16-byte TX queues (to +0x98). Queue:
  +0 head, +4 tail (pointer to the next-field to
  fill; initially &head), +8 state byte (mapped to
  access category by FUN_00010188: 0->2, 3->2,
  4..5->3->... code 1, >5->0), +11 disabled flag
  (0 = fetchable, checked by ppFetchTxQFirstAvail /
  ppGetTxQFirstAvail_Locked), +12 active flag
  (set/cleared by ppProcessTxQ).
- +0x98, +0xa0: two 8-byte wait queues (head/tail),
  one per interface - drained by FUN_00010be8, which
  reclassifies each node (AC bits in the word at
  *(node+0x28)+4, bits 2..5) and enqueues it into
  the matching TX queue, or requeues it to the wait
  queue when FUN_00010bc4 (4-arg, likely an rcGetSched
  relative - resolve from asm) returns 8.
- +0xa8: TX-done queue (ppEnqueueTxDone tail +0xac).
- +0xb0: RX queue (ppEnqueueRxq tail +0xb4).
- +0xe8/+0xec: TX counters (ppProcessTxQ bumps
  +0xe8 and decrements +0xec for broadcast-flagged
  frames, **(node+0x28) & 0x800000).

Nodes are intrusive: next pointer at node+0x24,
packet-control pointer at node+0x28 (its +4 word
carries queue index bits 2..5 - ppRollBackTxQ pops
(q = word>>2 & 0xf) and push-fronts), node+0x20
optional secondary pointer (trc record? - the wait
queue passes *(node+0x20)+0x44 byte into the 4-arg
call).

## Function semantics so far

- ppEnqueueTxDone(node): tail-append to +0xa8 queue;
  NULL node = ets_printf + hang (blob assert style).
- ppEnqueueRxq(node): tail-append to +0xb0 queue.
- ppDequeueTxQ(q<8): pop head under critical section,
  tail resets to &head slot when queue empties;
  clears node next.
- ppFetchTxQFirstAvail(q): same, but only when
  queue+11 flag == 0 (not disabled).
- ppGetTxQFirstAvail_Locked(q): peek head under
  ETS intr lock, same flag gate at queue+11 (Ghidra
  byte +0x13 relative to (q+1)*16 base = +11 here).
- ppRollBackTxQ(node): push-front onto its home
  queue (index from pkt ctrl word).
- ppProcessTxQ(ifidx): round-robin over the
  interface's queue mask, current-queue byte rotates
  via lowest-set-bit extraction; calls
  FUN_00010188-shaped dequeue (GetAccess + dequeue +
  lmacAdjustTimestamp) and lmacTxFrame(node, ifidx).
  Returns 0 sent / -1 lmac busy / -2 nothing.
- ppCheckTxIdle(lockflag): lmacIsIdle(0..3) and all
  queues (0, 1, wait A/B heads) empty -> 0 else 1;
  per set interface bit it re-derives an op code
  from queue+8 state and calls FUN_00010dc0 (the
  pp_post(8,0) bit-clear helper region - resolve).
- FUN_00010000: ieee80211_hdrsize (24/30 +2 by
  frame-control bits). FUN_0001002c: queue config
  writer (queue+8/+12-ish bytes + mask bits).
  FUN_00010d80: mask-bit clear + pp_post(8, 0).
  FUN_00010188: GetAccess-mapped dequeue (see above).
- pp_attach: queue init (above), esf_buf_setup(),
  creates the pp queue (0x4b entries of 8) and ppTask
  via the OSI seam, arms the noise timer, suspends/
  resumes all tasks around it, logs LmacRxBlk/LmacTxBlk
  when RxNodeNum<3 or TxNodeNum<2.

## Decoded since (second pass)

Node (esf buffer) fields used by pp: +0x04 first
lldesc ptr, +0x0c frame ptr, +0x10 RX/TX info ptr
(word0 flags: 0x402 ack-time gate, bits 16..27 len;
word+8 in CalFrameTimes), +0x14 header ptr (byte0 =
802.11 frame control), +0x18/+0x1a u16 lens,
+0x20 trc record (or 0), +0x24 next, +0x28 pkt ctrl:
word0 (0x800000 bcast, 0x2000, 0x4000, bit3, bit8 =
0x800 rate-class), +4 (queue idx bits 2..5, bit1),
+8 (rate nibble bits 0..3, AC bits 20..22), +0x0e
u16 CTS time (out), +0x10 callback bitmask (TX-done:
bit i -> fn ptr at TxRx +0x138 - 4*clz... called with
node), +0x14 tick stamp.

pp_post(type, arg): dedup via a posted[] byte array
(indexed by type; type 9 exempt). Types 0..3 go to
the ppTask queue (0x4b x 8 messages {type, arg}) if
queue_msg_num < 0x4b. Types >= 4 go into a 32-entry
ring (2 words each) consumed by MacIsrSigPostDefHdl,
which signals PendSV(2); ring full or queue full
clears the posted byte and returns 1, else 0.
ppTask dispatch on message type:
  0..3  ppProcessTxQ(type)
  4     TX-done drain (queue +0xa8): per node, run
        set-bit callbacks from ctrl+0x10 against the
        TxRx+0xb8 callback mask / +0xbc table
        (ppRegisterTxCallback), tx_pwctrl accounting
        (ctrl+8 rate nibble + a shared byte vs limit
        5, or 1 when g_ic[0x141]==2), recycle class by
        ctrl word0: 0x2000->1, 0x4000->4, bit
        (DAT pool, 0x800-ish)->5, else 0; bcast bit
        breaks the drain after keepalive reset;
        non-bcast nodes with ctrl bit3 len-adjust
        their first desc (len -= 4) before
        esf_buf_recycle(node, class).
  5     RX drain (queue +0xb0): the big path -
        noisefloor sample, PM (pm_recv_all_data /
        pm_have_more_data_to_recv / pm_sleep_for from
        beacon), rc_get_trc + rcUpdateRxDone,
        ic_is_pure_sta + pm_add_aid/pm_set_addr,
        ptk/gtk alg fetch, then either recycle,
        hostap_input, sta_input, or the fragment
        reassembly block (static fragment node +
        length at TxRx-adjacent DAT pair; seq compare
        on header +0x16 low bits, memcmp of 12 header
        bytes, memcpy append, desc ctrl word len
        fixups), ieee80211_recv_action for action
        frames, g_ic callback hooks at +0x144/+0x148.
  8     FUN_000111cc (no-arg; resolve = likely
        ppProcessWaitQ's FUN_00010d60 or the noise
        kick) after clearing posted[8]
  9     ppProcessRxPktHdr(arg) (promiscuous cb +
        heap_caps_free of the header struct)
  10    clear posted[10] only
  11    g_ic+0x148 callback (+4 fn ptr) if set
  12    lmacProcessTxTimeout()
  16    ets_timer_proc()
ppTask never returns; messages are 2 words.

ppTxPkt(node): asserts computed AC (from ctrl+8
nibble, the 0/3->2, else 3, >2->1, >5->0 map) equals
ctrl bits 20..22, else ets_printf + hang. Optional
debug hex dump under app_optimize_and_debug_mac.
Sets ctrl bit1 when header[4]&1; mgmt/data ctrl-word
patches by frame type/subtype (data: set/clear bit3
by subtype 0x40; beacon subtype 0x80: TxRx+0xec++
when bcast; probe-resp-ish 0x40/0x50: set pool
constant bits when bit1 clear); adds a per-type
pad (table of 3 bytes indexed (ctrl>>0xe &0xf)-1)
to node+0x1a and into the first desc len field;
sets desc word bits (pool masks); rcGetSched under
ETS lock; ppCalFrameTimes; stamps ctrl+0x14 from a
tick pointer (resolve: DAT_00011b48 points at the
time word - likely &wdev tick or g_ic field);
FUN_00011b74() kick (resolve). Then: no-trc path
pm_allow_tx(1) gate (skipped when ctrl bit3 or ctrl
word4 in {0x20,0x3f}), rewrites ctrl+4 AC bits from
bit1 and ctrl+8 low nibble = 7, enqueues to the
home TX queue + pp_post(ac). With trc: data frames
may go straight to the pm gate; others call
ppSearchTxQueue(bit1, rec->slot(+0x44), rate nibble,
ac) -> queue idx (8 = none): !=8 and pm ok ->
rewrite ctrl+4 queue bits and enqueue; else wait
queue (per bit1 at +0x98/+0xa0 tails).

Helpers: ppCalFrameTimes (ack time into ctrl+8
bits 23.. (RC_GetAckTime(ctrl byte3) & 0x1ff) << 0x17
unless ctrl&0x402; CTS time RC_GetCtsTime(rate byte
at ctrl+0xc, len sum, ctrl>>0x17) -> ctrl+0x0e u16).
ppCalTxop(q, acc, limit): if queue not disabled,
walk queue summing ctrl+0x0e per node until >= limit
(ETS lock held weirdly takes param_2 - replicate).
ppSearchTxQueue = FUN_00010070 (4-arg: queue-mask
search by (rate-class bytes at queue+0x21/+0x22,
state byte queue+8), midpoint index pick, fallback
return 8). ppGetTxframe = FUN_00010188 (GetAccess
category from queue+8, dequeue, lmacAdjustTimestamp).
ppEnableQueue = FUN_0001002c, ppDisableQueue =
FUN_00010d80 (clear bit in TxRx word[ifidx+1] and
word0, pp_post(8,0)). TxNodeNum counts the wDevCtrl
+0x18 list (5 when ctrl byte set). ppProcessRxPktHdr:
promiscuous_cb(frame-0x60, len) then heap_caps_free.

## Still to resolve in the implementation pass

Pool constants in ppTxPkt - RESOLVED: subtype 0x40
bit 0x800, subtype 0x50 bit 0x08000000, desc mask
0xff000fff, desc set bit 0x40000000, ctrl+8 mask
0xff8fffff, debug masks 0x08000000/0x04000000, and
the tick stamp is *WDEV_COUNT_REG (0x3ff20c00, the
same counter trc uses). FUN_00011154/1116c/11194
are all .text.ieee80211_hdrsize. FUN_000111cc,
FUN_00011b74 and ppProcessWaitQ's target are one
function: .text2.42+0x38, the wait-queue drainer
(FUN_00010be8's body). Still open in the
implementation pass: ppTask DAT masks
(0x110c0/c4/c8, 0x110a8/ac/b0/b4 - read from the
ppTask literal pool values, not relocations),
pp_noise_test/FUN_00010070 already identified as
ppSearchTxQueue, g_ic offsets used by ppTask
(+0x141, +0x144, +0x148, +0x160), and the exact
current-queue byte offsets in ppProcessTxQ
(queue+12 vs +0x14 confusion in Ghidra - read asm).
Then implement pp.c byte-exact (TxRx/node layouts
are shared with lmac/wdev by pointer - no layout
freedom, unlike trc).
