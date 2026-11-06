# B36 - trc stage 2: the PER state machine, clean-room

Status: DONE. firmware/libpp/trc.c replaces the blob
trc.o (743-byte rcGetSched and all). esp9 links ours;
the map has 0 `libpp.a(trc.o)` references. On device:
Wi-Fi join, scan, plan9port, bench9p (60 iters), and
an IL mount from dev9p all pass with our trc in the
TX path. Host tests pass. Board restored to stock
(AT verified).

## What the decode pinned (evidence: b36 notes)

- Records: 72 bytes x 9 stations, mask-bit indexed,
  MAC at +0x0c. Full field map in trc.h (RSSI EMAs,
  PER numerator/attempts at +0x14/+0x18, budget
  500000 at +0x2c, tick stamps, table pointers).
- Entries: four (rate, retries) stages, +8 = total
  retry budget (32 = 2+2+3+25 in every row - the
  stage-1 "budget word" reading was wrong), +9 =
  PER-quality threshold, +10 = own index (rcAttach
  stamps it; tables are writable for this reason),
  +11 = step flag.
- rcGetSched evaluation: quality =
  (attempts << 7) / weighted; step-up when quality
  beats the 90/100/110-minus-RSSI-margin gate and the
  tick budget (WDEV_COUNT_REG at 0x3ff20c00, eagle_soc
  name found in SDK headers) has elapsed; step toward
  faster entries, clamped by user_rc_limit pairs;
  rcLowerSched steps the other way on bad-PER streaks
  (>10 frames at >=6 retries) or the 100000-tick
  low-water path.
- TxDone outcome masks: success
  (desc[4] & 0x03fc0000) == 0x00040000, fail
  (desc[4] & 0x03f80000) == 0x00080000.
- rssi_margin is a per-rate (K1, K2, limit) formula,
  min(3*((rssi-K2)>>1), 32) or 0; its switch tree was
  traced branch by branch for all rates, including
  the degenerate default that returns the caller's
  live a4 (PER quality) clamped to 32 - passed
  explicitly in our version, documented in the code.
- g_ic / lmacConfMib / sig_max_rate2 are referenced
  at the blob's own byte offsets (+0x161/+0x162/
  +0x165, MIB +0x0c/+0x0e); semantics were not needed
  because the replacement shares the blob's view.

Host test (firmware/libpp/test/test_trc.c, -m32 so
descriptor pointers match the target ABI): vectors
hand-computed from the decoded arithmetic - attach
indices, limit-pair writes and error codes, enable/
lookup/disable, PhyMode N/G/B selection, N
rate2schedidx values, TxDone PER accumulation and
RSSI EMA, bad-PER LowerSched step-down, RxDone EMA,
GetRate stage walk (including the retries-nibble
wrap), GetSched evaluation and step-up with a
scripted tick, NeedRTS thresholds, retry limit.
Two test expectations were wrong during development
(nibble wrap; only-sta return) and were fixed in the
test after re-reading the asm - the code matched the
blob in both cases.

One ABI note: our record layout is a C struct, not
the blob's byte layout. That is safe here because
`trcs` is a local symbol in trc.o - every other
object reaches records only through these functions.
(The blob's own record offsets were still decoded
and are recorded in trc.h, in case that ever changes.)

## libpp scoreboard

Clean-room linked: esf_buf (B24), rate_control (B31),
trc (B36). Still blob: pp (queues), if_hwctrl, lmac,
pm, wdev; libnet80211 and libphy whole. Record layout
risk noted above applies to pp next: its queue
structures ARE shared with lmac by pointer, so pp
will need the byte-exact treatment esf_buf got.
