# B37 - pp.o replaced (libpp queue core), plus a latent kernel bug

Status: DONE. The blob `pp.o` (7,860 B, 25 exported
functions - the packet-processor queues, ppTask,
pp_post ISR plumbing) is replaced by clean-room
[pp.c](../firmware/libpp/pp.c) (1,440 lines), transliterated
function-by-function from the Ghidra dump in
`re/inventory/b37/`. The esp9 map contains zero
`libpp.a(pp.o)` references. On-device: boot, AP join,
plan9port 9P, bench9p 60 iters, and an IL mount from
the real 9front kernel on dev9p all pass. Board
restored to stock after the round (verified AT+GMR).

## What pp.o turned out to be

All 91 functions (25 exported, the rest file-local)
decoded. The structure shared with lmac/wdev by
pointer, so byte-exactness was mandatory:

- `pTxRx` -> 240-byte TxRx context: union enable mask,
  per-context masks, per-context current-queue bytes at
  +0x14 (attach writes 2,3,5,6), eight 16-byte TX
  queues at +0x18 (head, tail-pointer, state +8,
  per-queue bytes +9/+10, disabled +11, active +12),
  wait queues +0x98/+0xa0, TX-done +0xa8, RX +0xb0,
  TX-done callback mask +0xb8 and table +0xbc,
  counters +0xe8/+0xec.
- Nodes are intrusive (next +0x24, pkt ctrl +0x28);
  ctrl word fields drive scheduling (queue bits 2..5,
  AC bits 20..22 of word 2, tick stamp word 5 = the
  WDEV counter at 0x3ff20c00).
- pp_post dedups through `pp_sig_cnt[17]` (type 9
  exempt); types 0..3 go to the ppTask queue (75 x 8B),
  types >=4 to a 32-entry ring drained by
  MacIsrSigPostDefHdl + PendSV(2).
- pp_attach also writes MAC register 0x600011f8 =
  0x000a0700 and creates task "ppT" at maxprio-2.

Open questions from the inventory are closed:
ppProcessTxQ's active flag is queue+12 (the +0x14
form is the same byte addressed from the TxRx base);
pp_noise_test thresholds are 1000/interval (block
counters) and 10000/interval (lmac_check_ac), logging
through esp_log_write(1, "wifi", "LmacRxBlk:%d", ...)
with raw node counts.

## Link mechanics (two traps)

- `pend_flag_noise_check` (exported by pp.o, referenced
  by pm.o) must be a strong definition in our object:
  as a COMMON it does not stop the linker pulling the
  blob member out of libpp.a, which then duplicates
  every pp symbol. `wifi_sta_rx_probe_req` is the
  opposite: ieee80211_api.o carries the strong
  definition, so ours stays a COMMON and merges, like
  the blob's did.
- Loose objects in EXTRA_LDFLAGS bypass the ldgen
  archive-member placement that puts all of libpp.a in
  IRAM. `build-esp9.sh` now renames the clean-room
  objects' `.text.*`/`.literal.*` sections to `.iram1.*`
  with objcopy and relinks, so pp/esf_buf/rate/trc sit
  in IRAM exactly where the blob put them.

## The boot wedge was a kernel bug, not a pp bug

First flashes hung in `esp_event_loop_create_default`,
before pp_attach ever ran. Stub-pp and padded-stub
experiments isolated it to our object's presence; raw
UART marks in the scheduler narrowed it to sys_evt
spinning inside `wait_until`: every `p9_os_delay_ms`
call read as ISR context and refused to yield.

Root cause: `in_isr_context()` (kernel
`p9_os_xtensa.c`) tested SP against a hardcoded "NMI
stack" window, 0x3ffef950..0x3ffefd50, measured
against an old link layout. Our pp.o grew BSS by
~1 KB; the proc stack pool moved up by 0x420 and
sys_evt's stack (pool + 4096 + 4096 + 3072, 6144 B)
landed with its top inside that window. Any future
BSS growth of the wrong size would have wedged boot
the same way - the blob build survived by 540 bytes
of margin.

Fix (kept, in `p9_os_xtensa.c`): a running proc whose
SP lies inside its own stack is in proc context by
construction - the NMI switches SP to _chip_nmi_stk,
so an NMI frame can never alias a proc stack. That
check now precedes the legacy window test. The blob
build's survival margin no longer matters.

All debug marks from the hunt were removed; the only
kernel change is the `in_isr_context` fix. Host tests
were re-run after cleanup.

## Host tests

`firmware/libpp/test/test_ppqueue.c` (-m32,
-PP_HOST_TEST seam stubs the WDEV counter and MAC
register write): attach layout (queue tails
self-pointing, current queues 2/3/5/6, Q0/Q1 state 7
and disabled), FIFO/rollback order, disabled-gate
(fetch/peek gated, dequeue not), callback registry
double-register rejection, pp_post dedup + type-9
exemption + MacIsrSigPostDefHdl ring drain,
ppCalFrameTimes ack/CTS fields, ppCalTxop summing
and limit clamp, ppProcessTxQ rotation and the
broadcast-counter send path, TxNodeNum list walk.
Expected values hand-computed from the dump.
Full libpp host suite green.

## On-device proof (this build)

- Boot: `procs 8/8: uiT tiT TmrSvc sys_evt ppT cons
  srv9p il9p`; scan-ok; 9P TCP + IL listening.
  Memory gate green (build script, heap headroom
  26,768 B; the B36 figure was 27,992 - the delta is
  g_cnxMgr now carried as a strong BSS array).
- Provisioned via UART (creds file was empty again -
  the B36 anomaly; record now stored, re-provisioning
  not needed next round if it persists). Auto-join on
  next boot: `join-ok rssi=-55`.
- plan9port: `9p ls /`, `read /status`, `read
  /wifi/creds` -> `ssid Linksys01314 / key set`
  (never the key); `lastctl wifi:scan` after a scan
  write - no key leak (B33 fix still holds).
- bench9p 60 iters: Tread /status med 14.1 ms,
  scan write via /wifi/ctl counted, BENCH9P: DONE.
- IL from dev9p (real 9front kernel, drawterm -G):
  `srv il!192.168.1.146!564 esp9il9`, mount,
  `cat /n/esp9il9/version` and `/status`
  (ilstate 2 ESTABLISHED, ilrx 20 / iltx 17),
  `echo scan > /n/esp9il9/wifi/ctl`, AP list read
  back. TCP mount from the same machine was the
  control in B33 and is unchanged.

## Blob state after B37

- libpp: esf_buf, rate_control, trc, pp replaced.
  Remaining blob members: pm.o, if_hwctrl.o,
  lmac.o, wdev.o (pm next, then if_hwctrl; lmac/wdev
  last - wdev is blocked on hardware register work).
  libnet80211 and libphy remain wholly blob.
- New rule for the notebook: hardcoded address
  windows in the kernel are link-layout assumptions;
  any of them can go stale when an object is swapped.
  Grep for magic address ranges before the next
  link-swap, not after a wedge.

## Files

- firmware/libpp/pp.c, pp.h (new); test/test_ppqueue.c;
  README test line. Component symlinks pp.c/pp.h;
  esp9 Makefile EXTRA_LDFLAGS += pp.o.
- firmware/esp9/build-esp9.sh: objcopy IRAM rename +
  relink for the clean-room objects.
- firmware/kernel/p9_os_xtensa.c: in_isr_context
  proc-stack check (the wedge fix).
- re/inventory/b37/: pp Ghidra dump + PP-NOTES
  (pre-existing from the inventory step).