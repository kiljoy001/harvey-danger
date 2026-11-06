# B16 - Blob-free link experiment: undefined-symbol closure

Status: closure enumerated and sized. Radio bring-up on the
Plan 9 scheduler has not been attempted; this report is the
map for that campaign. No device was flashed in this stage.

## Method

Three reductions, all with the pinned Xtensa tools:

1. `nm -u` union over `libpp.a` + `libnet80211.a`: 399 undefined
   references, 111 external after removing symbols the two
   archives define themselves.
2. Reduced against every archive built for the B14 RTOS
   server plus `libphy.a`: residual 36.
3. A real test link: our kernel (`p9.c`, `osi_wifi.c`,
   `p9_os_xtensa.c`, `memwatch.c`, compiled with the RTOS GCC
   8.4) plus a reference object calling
   `esp_wifi_init_internal`, linked `--start-group` against
   `libpp.a libnet80211.a libphy.a -lgcc`: **86 undefined
   symbols**. That is the honest minimal closure for running
   the blobs on our substrate with nothing else in the image.

## Headline result

Zero of the 86 are `__wifi_*`. The fixed OSI surface
(B15) satisfies every OSI call the blobs make in an actual
link, not just in the symbol census. The closure is SDK
support code, not OS interface.

## The 86, categorized

- Toolchain/libc (12): `memcmp memcpy memmove memset strlen
  strncmp strncpy sprintf printf free setjmp longjmp`.
  Resolved by linking newlib; `free` pulls in malloc, which
  needs `_sbrk_r` and syscall stubs (`_close_r` etc.).
- ROM / NONOS SDK (16): `ets_delay_us ets_printf
  ets_timer_arm_us ets_timer_disarm ets_timer_proc
  ets_timer_setfn ets_update_cpu_frequency system_get_time
  os_delay_us os_printf_plus Cache_Read_Disable
  Cache_Read_Enable_New gpio_output_set Uart_Init
  uart_buff_switch uart_tx_flush`. In a NONOS-based image
  these come from ROM addresses and libmain, as they do today.
- esp_* companion source (22): `esp_event_send
  esp_get_free_heap_size esp_log_early_timestamp esp_log_write
  esp_now_deinit esp_phy_load_cal_and_init esp_random
  esp_set_cpu_freq esp_sleep_lock esp_sleep_unlock
  esp_timer_get_time esp_wifi_scan_get_ap_num_max
  esp_wifi_try_rate_from_high phy_open_rf wifi_timer_init
  pp_soft_wdt_restart pp_soft_wdt_stop` plus data
  `default_ssid _g_esp_wifi_connect_open_router_when_pwd_is_set
  _g_esp_wifi_ppt_task_stk_size g_log_level g_log_mod`.
  These live in the RTOS SDK's open `esp_wifi.c`/`esp_common`/
  log/phy-init sources; they must be ported or stubbed.
- heap_caps (4): `_heap_caps_malloc _heap_caps_free
  _heap_caps_zalloc heap_caps_get_dram_free_size`. Wrap over
  the NONOS heap (`pvPortMalloc`/`vPortFree`) in a blob-free
  image; the blobs allocate esf/DMA-adjacent buffers through
  these, so this needs a real allocator, not an arena.
- NVS (13): `nvs_open nvs_close nvs_commit nvs_get_blob
  nvs_get_i8 nvs_get_u16 nvs_get_u8 nvs_set_blob nvs_set_i8
  nvs_set_u16 nvs_set_u8 misc_nvs_init misc_nvs_restore`
  (plus data `g_misc_nvs`). A minimal flash-backed or
  RAM-backed shim suffices for bring-up; persistence can come
  later.
- Crypto/WPA (5): `ccmp_encrypt ccmp_decrypt omac1_aes_128
  pbkdf2_sha1 hexstr2bin`. Available as open source in the
  SDK's wpa_supplicant/crypto components; compile those files
  rather than reimplementing.
- FreeRTOS port primitives (6): `PendSV vPortEnterCritical
  vPortExitCritical vPortETSIntrLock vPortETSIntrUnlock
  _xt_isr_mask`. Small and security-critical: these are the
  critical-section/ISR-mask layer. Implement over Xtensa
  `rsr ps`/INTENABLE save-restore with a nesting counter.
  This is also the missing ISR-safety piece flagged since B11.
- Misc (2+): `bit_popcount` (trivial), `lldesc_build_chain`
  (our clean-room `pp_lldesc` already builds descriptor
  chains; the blob expects the SDK's exact contract from
  SPEC-WDEV-DMA, so this is an adapter, not a stub).

## Bring-up sequence (next campaign)

1. `blob_support.c`: implement the portable groups above
   (critical sections first, heap_caps over pvPortMalloc,
   log/event stubs, NVS RAM shim, bit_popcount).
2. Compile the SDK's open crypto/WPA objects for ccmp/omac/
   pbkdf2 instead of writing crypto.
3. Link a kerntest-style NONOS image (stock boot, libmain for
   the ROM/ets group) with the three blob archives and
   libp9.a, entry calling `esp_wifi_init_internal`.
4. Boot milestones, in order: init returns without crash;
   pp task created on a p9 proc (the 75-entry queue path);
   scan results over UART; station join; first TX/RX through
   pp on our scheduler.
5. Only then replace the NONOS init underneath (Phase C
   proper).

Risks, honestly: the blobs were compiled with GCC 8.4/newlib
against FreeRTOS headers, so struct layouts baked into blob
code (QueueHandle internals are opaque, good; ETSTimer and
wifi structs are not) may not match a NONOS-flavoured support
layer. The link closing is necessary but not sufficient;
milestone 4.1 is where that gets proven.

## Next steps

1. Write `blob_support.c` groups in the order above, host-test
   what is host-testable (critical-section nesting, heap shim).
2. Link milestone: full image links with zero undefined.
3. Flash-and-boot milestone behind the usual restore gate.
