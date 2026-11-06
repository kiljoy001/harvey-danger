# B38: pm.o replaced (clean-room power management)

## Result

`libpp.a(pm.o)` (49 KB blob, 45 exports) is replaced by clean-room
[pm.c](../firmware/libpp/pm.c)/[pm.h](../firmware/libpp/pm.h)
(~1,240 lines), transliterated from the disassembly and Ghidra
decompiles in [inventory/b38](inventory/b38/). The linked image has
0 `libpp.a(pm.o)` references; export set is byte-identical to the
blob (diffed with xtensa nm), undefined set differs only by libgcc
`__moddi3`.

Remaining libpp blob members: `if_hwctrl.o`, `lmac.o`, `wdev.o`.
libnet80211 and libphy remain whole blob.

## What pm is

Modem-sleep power management for the station: an 8-state machine
(pmc state block, 0x90 bytes, byte-exact layout) driven by three
ETSTimer callbacks inside pmc itself (wait_bcn, open_rf_for_bcn,
wait_tx_rx_idle), beacon TIM/DTIM parsing in `pm_onBcnRx`, an
early/timeout histogram (0x177 bins) that adapts RF wake timing,
null-data keepalives (10 s threshold against the WDEV_COUNT tick),
and the `esp_wifi_*pm_config` surface. Defaults after `pm_attach`:
sleep type 1, wait_time 20 ms, early 4 ms, timeout 24 ms.

## On-device proof (ESP8266EX, /dev/ttyUSB2)

- Boot: `procs 8/8: uiT tiT TmrSvc sys_evt ppT cons srv9p il9p`
- Auto-join from stored creds: `join-ok rssi=-57` (after 2 retries,
  the usual shim frame loss)
- plan9port `9p` ls/read /status over TCP 564
- bench9p 60 iters: BENCH9P: DONE
- 9P scan write + AP list read while associated
- Real 9front (dev9p) IL mount: `srv il!192.168.1.146!564`,
  ilstate 2, ilrx 27 / iltx 25 / iltxerr 0, scan over IL
- Memory gate green, heap headroom 26768 (unchanged from B37)

Host tests: `test_pm` (attach defaults, config clamps, TIM/AID
logic, state transitions, keepalive, nullfunc frame fill) — all pass.

## Bugs found by the swap (blob semantics I first got wrong)

The first flash failed association (reason 202/203 loop). Control
build with blob pm joined immediately. Three functions had been
written from inventory notes instead of their decompiles:

1. `pm_set_addr(ctx, mac)` — the MAC is the **second** argument;
   no tsf_diff write. I had copied 6 bytes from the wrong pointer.
2. `pm_add_aid(conninfo)` — takes a struct pointer: returns
   `u16@+2 == 0` and only then stores the AID from `u16@+4`.
   I had stored the argument value itself.
3. `esp_wifi_set_pm_config` — the blob **clamps, never errors**
   (early 2→3, timeout 12→13, wait 5→10, zeros→1) and stores the
   8th byte into `recv_multi`. My `-1` returns aborted the SDK's
   init-time call. `esp_wifi_ps_lock` is likewise just bit 0 of
   `app_optimize_and_debug_mac`, not an esp_sleep_lock wrapper.

Plus one from the summary notes: `wdev_get_tsf_time(use_hw)`
returns a 64-bit TSF (`use_hw==0`: WdevTimOffSet + tsf_diff +
WDEV_COUNT; else the 0x3ff2104c/0x3ff21048 register pair). My
no-arg 32-bit version would have poisoned every caller's high word.

Lesson (again, after B37): never implement from the inventory
summary — read the decompile. Every function above is now a direct
transliteration, quirks included (the unreachable `recv_bdata+1`
branch in pm_wait_bcn_fun, the inverted timeout floor).

## Link trap

`app_optimize_and_debug_mac` must be a **strong** definition in
pm.c (`= 0`, .data). As a common, ld extracts `libpp.a(pm.o)` to
upgrade it and every pm symbol then collides — the B37
pend_flag_noise_check trap with the polarity reversed.

## Notes

- `pm_sleep_for(mac)` is, in the blob, a 6-byte memcmp against the
  AP MAC. Kept as-is under its exported name.
- `esp_power_consumption_info` transliterated with u64 arithmetic;
  the blob's `__muldi3/__udivdi3` constant set (297/4620/23100 mA-ish
  weights) is preserved. Diagnostic only.
- The B36/B37 creds-empty-after-flash anomaly recurred (provisioning
  record did not survive the flash round; re-provisioned by UART).
  Cause still unresolved.

Stock firmware restored and verified after the round (AT / AT+GMR).
