# B17 - Blob-free bring-up attempt: support layer and link close, boot blocked

Status: support layer implemented and the link closes, but
the blobtest image does not boot to user_init. This stage is
therefore NOT a bring-up. Board restored to stock AT.

## What was built

`firmware/kernel/blob_support.c` implements the non-OSI part
of the B16 closure for a blobtest image:

- Critical sections (`vPortEnterCritical`/`ExitCritical`,
  `vPortETSIntrLock`/`Unlock`, `_xt_isr_mask`/`unmask`,
  `PendSV` -> `p9_yield`) over Xtensa PS interrupt masking
  with a nesting counter. This is also the ISR-safety layer
  owed since B11.
- `heap_caps_*` over the NONOS heap, `esp_get_free_heap_size`,
  `esp_timer_get_time`, `esp_log_write` via vsnprintf,
  random via `os_random`, and success-returning stubs for
  phy/log/event/sleep companions (documented as first
  suspects, in order, if init misbehaves).
- A RAM NVS shim (32 keys, ints and small blobs; not
  persistent, by design at this stage).
- Newlib reent/syscall layer (`__getreent`, `_sbrk_r` over a
  4 KB static arena, `_write_r` sink, etc.), because the
  blobs call the printf family.
- Crypto placeholders returning error (ccmp/omac/pbkdf2),
  to be replaced by the SDK's open wpa objects before any
  WPA2 traffic.
- `os_timer_*` over NONOS ets timers in NONOS images.

OSI + kernel + support + `libpp.a libnet80211.a libphy.a`
links with **zero undefined symbols** (RTOS toolchain test
link, 238 KB text). The blobtest image
(`firmware/sdkbuild/blobtest`, builder `build-blobtest.sh`)
goes further: NONOS boot and libmain, RTOS blob archives
instead of the NONOS pp/net80211/phy, our substrate, and the
support layer. It links and packages (195 KB class image).

Link surgery that was needed, all reproducible in the build
scripts:

- NONOS libmain defines the same `wifi_*` API names as the
  RTOS blobs; `--allow-multiple-definition` with a chosen
  order resolves it (both orders were tried; see below).
- 53 residual symbols were NONOS glue the RTOS blobs do not
  provide (`dhcp_*`, `netif_*`, `pm_*`, `fpm_*`, ...);
  `blob_glue_stubs.c` closes them. The blobtest never calls
  NONOS Wi-Fi APIs, so these are link-only.
- Section placement: the blobs' code sections (`.text2.*`,
  `.text.<fn>`) need flash placement in the NONOS ld script;
  a blob-specific ld script
  (`ld/eagle.app.v6.new.2048.blob.ld`) puts blob and app code
  in irom while keeping NONOS IRAM rules.
- `fix_cache_bug` disables the flash cache and calls
  `phy_get_bb_evm` while it is off, so those two functions
  must be in IRAM (the RTOS build puts all of libphy there;
  only these two need it). The build renames their sections
  with objcopy in a *copy* of libphy.a (SDK archive untouched)
  so the ld script can place just them in IRAM. An earlier
  attempt that placed a new section before `.text` shifted
  the exception vectors and crashed immediately; the rename
  approach keeps vectors intact (`call_user_start` verified
  at 0x40100004).
- DRAM pressure: proc stacks went 4096 -> 3072 bytes
  (memwatch headroom justified it) and the _sbrk arena
  shrank to 4 KB to fit .bss.

## Where it stops

The image resets repeatedly with
`Fatal exception 0 (IllegalInstructionCause)` and a garbage
EPC (e.g. 0xf9c1e9f1, 0x57134764 - not a mapped address),
before any `user_init` output. It behaves identically with
RTOS-first or NONOS-first symbol resolution, and with the
known-good kerntest main substituted for the blobtest main,
so the failure is in the startup/init path with the RTOS
blobs linked, not in our bring-up code and not in link order.

Working hypotheses, in order:

1. SDK-generation ABI mismatch in libmain's early init:
   NONOS libmain drives NONOS-era phy/wifi glue during
   system init; functions and function tables (g_phyFuns
   assignment, phy cal entry conventions) differ in the
   RTOS blobs, so an early call goes through a bad pointer.
2. Newlib reent: blob code calling printf-family functions
   through our zeroed static `_reent` before libc init may
   corrupt state. Testable by making the blobs' log path
   avoid stdio entirely.
3. PHY cal path needs more than fix_cache_bug in IRAM in
   this generation mix (e.g. its caller chain also runs
   cache-off in RTOS placement).

## Restore

Full stock backup rewritten at 0x0 (hash verified), AT probe
returns original strings. The provisioning key was removed
from blobtest's user_config.h and the default blobtest image
rebuilt unflashed (SHA-256
`99f905a5e318cd6d98825368da153613aefcab00936ccdae139dfe8aa9ee9b2f`).

## Next steps (revised)

1. Do not mix generations in the init path: either (a) use
   the RTOS bootloader/startup with our own no-FreeRTOS
   entry replacing startup.c, so every layer below the blobs
   is RTOS-generation, or (b) interpose at libmain's init:
   identify the exact early call that jumps to garbage by
   UART-tracing call_user_start_local stages.
2. Kill hypothesis 2 cheaply: stub the blobs' printf path
   (wrap sprintf/printf to ets_sprintf) and retest boot.
3. Only after user_init prints: init -> pp task -> scan ->
   join milestones as originally planned.
