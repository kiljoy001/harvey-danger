# B40 inventory: lmac.o (libpp)

Extracted from RTOS SDK `libpp.a`: 31,552 bytes, 10,194 bytes of
code/rodata/bss. Decompiles in this directory (Ghidra 12.2,
Xtensa:LE:32:default); raw disassembly in `lmac-objdump.txt`.

## Surface

- 25 real exports (`lmac-exports.txt`) plus the three junk commons
  seen in every libpp member (`g_cnxMgr`, `__packed__`,
  `wifi_sta_rx_probe_req` — not our responsibility, cf. B39).
- 24 undefineds (`lmac-undefined.txt`). All are already satisfied:
  our clean-room pp (`ppCalFrameTimes`, `ppCalTxop`, `ppEnqueueRxq`,
  `ppEnqueueTxDone`, `ppFetchTxQFirstAvail`, `pp_post`,
  `ppProcessTxQ`, `pTxRx`), our rate_control (`rcAttach`,
  `RC_GetAckTime`, `rcGetRate`, `rcReachRetryLimit`,
  `RC_SetBasicRate`, `rcUpdateTxDone`), our pm (`pm_wakeup_init`),
  wdev (`wDev_EnableTransmit` — the one hardware call), ROM/libc
  (`ets_*`, `printf`, `__udivsi3`, intr lock), `phy_open_rf`,
  `clockgate_watchdog`, `esp_log_write`.

## What lmac is

The MAC state machine between pp's queues and wdev's transmitter:
per-access-category (AC) EDCA state, frame-exchange sequences
(TXOP), ACK/CTS timeout and retry handling. It owns no hardware
registers except through `wDev_EnableTransmit`.

Five local (static) functions, identified by callee sets and
renamed in this directory:

- `lmacTxDone` — enqueue TX-done, rate-control update, re-run TXQ
- `lmacDiscardFrameExchangeSequence` — asserts state==6, clears AC
- `lmacSetTxFrame$constprop$5` — 747 B, builds the TX frame
  descriptor (ppCalTxop, RC_GetAckTime)
- `lmacEndFrameExchangeSequence` — 616 B, retry-or-recycle decision
  (rcReachRetryLimit, lmacTxFrame retransmit, wDev_EnableTransmit)
- `lmacProcessShortRetryFail` — short-retry counter vs MIB limit

## Structures

`our_instances`: 288 bytes = 8 AC slots x 36 bytes (`GetAccess`
returns `base + (ac & 0xff) * 0x24`). AC fields so far:

| off | field |
|-----|-------|
| +0x04 | ac index |
| +0x05 | aifs |
| +0x06 | cw current |
| +0x07 | cw min |
| +0x08 | cw max |
| +0x09 | short-retry count (vs `lmacConfMib+0x0d`) |
| +0x0a | long-retry count |
| +0x11 | state (0 idle; 6 = discard; `lmacIsIdle` tests ==0) |
| +0x18 | txop limit (u16) |
| +0x1c,+0x20 | counters (zeroed at init) |

`lmacConfMib`: 36-byte common. `lmacInit` defaults: +0 0x400,
+4/+8 0x200, +0x0c/+0x0d retry limits 0x20, +0x14 9,
+0x1c basic-rate mask 0x15f. `lmacInit` inits 6 ACs
(VO/VI/BE/BK + 2 extra slots: ac 4 and 7), then
`RC_SetBasicRate(mib+0x1c, 1)`, `rcAttach()`.

Other bss: `s_lmac_check_tx_hang` 16 B, `s_set_11b_plcp` 1 B,
`s_tx_with_long_plcp` 1 B.

Debug exports `dbg_lmac_show`/`dbg_display_acs` print the AC table
(the rodata format strings document field names: aifs, cw, cw_min,
cw_max, qsrc, qlrc, txop, lifetime, su_cnt, fai_cnt).

## Risk notes

- Same class as B37/B39: structures are shared by pointer with pp
  (frame at +0x20 rate ptr, +0x28 flags/queue ptr) — byte-exact
  layout matters, transliterate field-for-field.
- Check every multi-arg call for flow-through register/stack args
  before writing C (B39 lesson). Prime suspects: `lmacTxFrame`
  callers and `wDev_EnableTransmit`.
- Asserts (`ets_printf("%s:%u", ..., line)` + hang) appear in the
  discard/end-exchange paths; keep them — they are the blob's own
  loud-failure behaviour.
