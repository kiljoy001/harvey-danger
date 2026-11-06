# trc stage 2 - decode notes (in progress)

Working corpus: `trc-objdump.txt` (objdump -dr of
trc.o from libpp.a, with relocations),
`rcGetSched_00010844.c` (Ghidra), plus 33 decompiled
functions from the same Ghidra run (regenerable:
/opt/ghidra analyzeHeadless on trc.o, processor
Xtensa:LE:32:default, DecompileAll script).

## Established (from decompiled accessors)

- Per-station record size is 0x48 = 72 bytes;
  .bss.trcs is 0x288 = 648 = 9 records. (B31's
  "54-byte" guess was wrong.)
- Records are indexed by BIT POSITION in a mask:
  rc_get_sta_trc(maskbyte) requires popcount == 1 and
  returns trcs + (31 - lzcount(lowbit)) * 0x48.
  rc_get_trc_by_index(i) tests mask bit i.
  rc_get_trc iterates set bits, memcmp of 6-byte MAC
  at record + 0x0c; that mask byte is likely the
  `trc_mask` BSS byte (interface_trc_mask is 2 B).
- rcGetTrc() returns the trcs base pointer itself.
- rcGetSched's first argument is (among other uses)
  the 72-byte record: +0x34 = pointer to current
  12-byte schedule entry, +0x3c = schedule table base
  pointer, +0x30 = phy type/mode byte used via
  phytype2mode, +0x45 = a rate/mode index byte,
  +0x14/+0x18/+0x28 = counters rcGetSched zeroes on
  schedule step, +0x1c/+0x20/+0x24/+0x2c = TSF
  timestamps (compared against a global tick
  DAT_000107f0, wraparound-aware (x-1 if x>now idiom)),
  +0x0a/+0x0b = smoothed bytes (running averages,
  `(old+new)>>1` shapes), +0x06/+0x07 = flag/rate
  bytes consumed at the tail (sig_max_rate2 /
  should_update_rate interplay).
- Schedule entry fields beyond stage 1's +0/+8:
  entry +9 = a threshold byte (compared scaled
  `(entry[9]*3 + 0x80)>>2` against the +0x0b average),
  entry +0x0a = rate-ish byte compared against the
  user_rc_limit-derived cap (PTR_DAT_00010804 table
  indexed [mode*2 + rec[0x45]] pairs), entry +0x0b
  bit 0 = a per-entry flag (set when stepping down:
  `*(entry-1) |= 1` writes previous entry's +0x0b).
  Step-down path calls rcLowerSched (FUN_000103bc
  region) when a time budget (DAT_00010808) expires.

## Decoded since (second pass)

Record layout (72 B, `trcs[i]`), from rc_enable_trc /
rcUpdatePhyMode / rcUpdateTxDone / rcUpdateRxDone /
rcGetSched cross-reads:

- +0x00 s8 TX-RSSI newest (0x7f = unset); +0x01 s8
  TX-RSSI EMA ((3*old + (prev+new)/2) >> 2 shape);
  +0x02/+0x03 same pair for RX (rcUpdateRxDone).
- +0x04 u8 (set by PhyMode; read in GetSched fallback
  when the g_ic interface bit is clear), +0x05 u8 max
  schedule index (B=3, G=10, N=11 by table),
  +0x06 u8 consecutive bad-PER counter (TxUpdatePer:
  n>=6 -> ++, n<=2 -> 0; >10 -> rcLowerSched),
  +0x07 u8 forced rate byte (used when rec flags
  &0x40), +0x08 u32 flags (bit0 gates RX-RSSI update,
  bit2 gates TX-RSSI update, bit6 selects rec[7]).
- +0x09 s8 last PhyMode argument (RSSI-ish),
  +0x0a u8 latest PER quality = (attempts<<7)/weighted,
  +0x0b u8 smoothed PER quality.
- +0x0c MAC[6]; +0x12 u16 (rc_enable_trc arg);
  +0x14 u32 PER weighted numerator; +0x18 u32 TX
  attempts; +0x1c/+0x20/+0x24 u32 tick timestamps
  (lower-sched / step-down / evaluation);
  +0x28 u32 TX-done count; +0x2c u32 budget
  (init 500000 = 0x7a120, cap ~4,000,000, doubled in
  GetSched when entry flag set and budget <= cap).
- +0x30 u32 phytype; +0x34 current entry ptr;
  +0x38 fallback table ptr (G tbl, or B tbl for B
  phytype); +0x3c current table base; +0x40
  rate2schedidx fn/table ptr; +0x44 u8 slot index;
  +0x45 u8 interface index (selects user_rc_limit
  pair and g_ic bit); +0x46 u8 per-mode value
  (B=3, G=7, N=8) used as step-down target index.

Schedule entry (12 B): +0..+7 = four (rate, retries)
stages; +8 u8 fail-path PER weight; +9 u8 PER-quality
threshold byte; +10 u8 schedule index (written by
rcAttach: N entries 0..11, G 0..10, B 0..3);
+11 u8 flags (bit0 set on previous entry when
stepping down).

Functions:
- rcTxUpdatePer(rec, n): attempts++; numerator +=
  (entry[1] < n ? 2 : 1) + n; bad-PER counter rule
  above; >10 -> rcLowerSched(rec).
- rcUpdateTxDone(rec, txdesc): only if rec->entry ==
  txdesc[+0x18]; txdone++; success iff
  (desc[+4] & maskA) == valA (pool constants still to
  resolve in .text.rcUpdateTxDone pool): RSSI update
  (unless flags&4), n = retries nibble (desc[+8]>>4);
  fail path iff (desc[+4] & maskB) == valB: n =
  entry[+8]; then rcTxUpdatePer(rec, n).
- rcLowerSched(rec) (= .text.rssi_margin body):
  clear entry flag + PER stats, budget = 500000;
  target = (g_ic if-bit ? user_rc_limit max :
  (rec[2]<26 && rec[3]<26 ? rec[5] (bottom) :
  rec[0x46])); step one entry toward it or jump;
  stamp +0x1c = tick.
- rcGetRate(rec, txctl): retry-stage walk over entry
  stage counts vs retries-used nibble (txctl[2]>>4),
  rate = stage rate (or last stage, or g_ic+0x162,
  or rec[7] under flags&0x40 + desc&8), then
  sig_max_rate2 cap via should_update_rate.
- should_update_rate(cur, max): 32-byte .rodata rate
  ordering table; returns table[max] < table[cur].
- rcReachRetryLimit(txdesc): reached iff
  (desc[0] & 0x08000000 && retries >= 5) ||
  (lmacConfMib[0x0c] <= retries).
- rc_set_rate_limit_id(mode, id<2, min, max): writes
  user_rc_limit pairs; mode 0=B (max<4, def 0..3),
  1=G (max<11, def 0..10), 2=N (max<12, def 0..11);
  0 ok / -1 range / -2 mode. user_rc_limit is 6
  (min,max) pairs indexed [phytype2mode*2 + ifidx].
- rcGetSched pool constants resolved: fallback
  tables G (rec==NULL) / B / BasicOFDM (+0x38 path);
  flag mask 0x02020002; tick source is a hardware
  register read at 0x3ff20c00; thresholds 99999 /
  100000 ticks; *txctl |= 0x00100000 marks "sched
  evaluated"; initial budget 500000.

## Resolved (third pass, from trc-objdump.txt asm)

- rcUpdateTxDone pool: success iff
  (desc[4] & 0x03fc0000) == 0x00040000; fail iff
  (desc[4] & 0x03f80000) == 0x00080000; callee is
  .text.rcTxUpdatePer (pool +0x10).
- rcGetSched call sites: PTR_FUN_0001082c is
  .text.rssi_margin called as
  rssi_margin(rec[+0x01], entry[+0]) with result
  (ret >> 1) & 0xff; PTR_FUN_00010834 is
  .text.rcLowerSched$constprop$0 + 0x14 called with
  (record) - i.e. rcLowerSched; FUN_00010314's body
  is that LowerSched logic. rcTxUpdatePer's
  threshold callee is the same LowerSched.
- GetSched eval block (asm-verified): fires when
  rec[+0x14] != 0 && (rec[+0x28] > 99 ||
  elapsed(+0x24) > 99999). quality =
  (rec[+0x18] << 7) / rec[+0x14] via __udivsi3;
  rec[+0x0a] = quality; rec[+0x0b] EMA init
  (3*entry[9]+128)>>2 clamped against quality, else
  (quality + old) >> 1 (the +0x174 branch in asm).
  Step-up: when (limit - margin < quality) with
  limit = entry[9]==0 ? 90 : entry[9]<65 ? 100 :
  110-form, and budget rec[+0x2c] < elapsed(+0x20):
  reset stats, budget = 500000, step entry -12 while
  above user_rc_limit min (pair[0] via
  phytype2mode(rec[+0x30]), rec[+0x45]), set stepped
  entry's +11 flag, stamp +0x20. Otherwise the
  budget-doubling / flag-clear hysteresis at
  0x1a6-0x1d3 (updateCounter & 3 gate, cap
  0x3d0900 = 4,000,000). After eval: clamp current
  entry index into the pair [min,max], entry =
  rec[+0x3c] + idx*12, zero +0x14/+0x18/+0x28.
  Non-eval exits: *txctl |= 0x00100000.
- Entry field correction: +8 is the entry's total
  retry budget (2+2+3+25 = 32 in every table row;
  BasicOFDM 2+2+3+25 with stage3 rate 0), passed to
  rcTxUpdatePer on the fail path. +9 thresholds
  decrease with rate (114, 101, 92, ...) as expected
  for step-up quality gates.
- rssi_margin(rssi_byte, rate_byte): a binary-switch
  on rate; each leaf computes
  t = (rssi - K1) mod 256 (unsigned compare against
  LIMIT); if t > LIMIT return 0; else
  v = (int8-ish (rssi - K2) >> 1 arithmetic on the
  32-bit difference), return min((3*v) mod 256, 32).
  Leaf constants seen: (34,33,220) rate 31,
  (30,29,224), (27,26,227), (22,21,232),
  (17,16,237), (14,13,240), (12,11,242),
  (9,8,245), (10,9,244), (15,14,239).
  IMPLEMENTATION RULE: transliterate the switch tree
  branch-for-branch (do not flatten to a rate table);
  the 21 rates in the schedule tables are
  {0,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,
  22,23,31}, but the tree also covers rates the
  tables never emit and fidelity requires all of it.
- g_ic: implementation does not need semantics, only
  the blob's own offsets, which is all the blob uses
  either: byte +0x161 (bit (txctl[1]>>1)&1 selects
  g_ic +0x162 as rate), byte +0x162 (rate), byte
  +0x165 bit rec[+0x45] (per-interface enable),
  lmacConfMib byte +0x0c, sig_max_rate2 byte. All are
  externs owned by net80211/lmac objects we still
  link; trc replacement references the same symbols.

## Remaining before implementation

- Small exports unread: trc_NeedRTS,
  rc_disable_trc / _by_interface, rc_only_sta_trc,
  rc_get_mask, trc_onScanStart / onScanDone,
  trc_onDisconnect, phytype2mode. (Small; read in
  the implementation pass.)
- Link surface: our trc must define the same 20
  exports + trcs/g_cnxMgr COMMONs and reference
  g_ic, lmacConfMib, sig_max_rate2, bit_popcount,
  esp_log_write, __udivsi3 (all already resolved by
  the SDK link in the blob build; verify in map).
- Host test vectors: hand-compute from the decoded
  arithmetic (B31 method: independent Python model
  first, C must match it), covering: PhyMode table
  selection per phytype, TxDone success/fail PER
  accumulation, LowerSched step-down targets,
  GetRate retry-stage walk, GetSched step-up gate
  with a scripted tick source. The tick register
  0x3ff20c00 needs a host seam (function pointer or
  weak symbol) so tests can script time.

Rule from B31 stands: nothing ships until the decode
is complete enough to implement without guessing,
with host tests built from blob behaviour vectors.
