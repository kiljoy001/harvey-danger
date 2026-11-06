# B23 - libpp surface inventory (first replacement target)

Status: DONE (inventory). No device work this round.
Method: pinned Xtensa tools (`xtensa-lx106-elf-nm/size/objdump`
from tools/xtensa-lx106-elf-rtos) over the RTOS SDK
`components/esp8266/lib/libpp.a`. Machine output in
`inventory/b23/` (libpp-defined.txt, libpp-undefined.txt,
libpp-size.txt). Confidence labels follow DESIGN 6.1:
observed = directly measured here; inferred = one
corroboration; open = unknown.

## Observed: the 9 objects

Text sizes are named `.text.*` + `.text2.*` sections summed
per object (local functions live in anonymous `.text2.N`
sections, so object text is larger than the exported
functions suggest).

| object | text B | role (evidence) |
| --- | ---: | --- |
| wdev.o | 13436 | WDEV hardware driver: 91 distinct WDEV register literals (0x3ff20000 bank). Only object that programmes the MAC. Exports wDev_Initialize, wDev_ProcessFiq (5206 B alone), wDev_SetBssid/SetMacAddress, key-entry and sniffer control. |
| pm.o | 7924 | Power management / sleep state machine. Only WDEV touch is the TSF timer 0x3ff20c00. Calls phy_open_rf/phy_close_rf. |
| pp.o | 7860 | Frame queues + ppTask: pp_post, MacIsrSigPostDefHdl, ppProcessTxQ, TX-done/RX enqueue. TSF reads only. |
| lmac.o | 5689 | Lower MAC: TX frame exchange sequences, ACK/CTS timeout handling. Touches 13 WDEV addresses (incl. 0x3ff20c74, 0x3ff20dc0-0x3ff20dd4, 0x3ff20e44). |
| trc.o | 3963 | Transmit rate control: rcGetSched/rcGetRate, PER tracking, schedule tables (rc11B/G/N in .data). TSF reads only. |
| esf_buf.o | 1530 | Frame buffer pools. Fully decoded in B22 / SPEC section 5. No MMIO. |
| if_hwctrl.o | 1149 | Interface control glue (ic_*): keys, vif, opmode over g_cnxMgr/if_ctrl. One WDEV literal (0x3ff20004). |
| rate_control.o | 945 | ACK/RTS rate + airtime helpers (RC_GetAckRate etc.), 64-byte our_ack_time table. No MMIO. |
| mac_frame.o | 0 | Empty: no symbols, no text (116 B archive member: comment + xtensa.info only). Nothing to replace. |

Total libpp text: 42496 bytes. Exported symbols: 214
defined (incl. COMMON data such as the 1224-byte g_cnxMgr
shared by pm/if_hwctrl/pp/lmac/trc/wdev).

## Observed: dependency surface

146 unique undefined symbols; 73 are cross-object calls
inside libpp itself, 73 are truly external:

- OSI seam (10): __wifi_task_create, __wifi_queue_create /
  send / send_from_isr / recv / msg_num,
  __wifi_task_suspend_all / resume_all /
  yield_from_isr / get_max_priority. Already implemented
  by our osi_wifi.c (Phase B). Proven: none of these block
  a clean-room libpp - the seam is ours already.
- Critical sections / ISR (5): vPortEnterCritical,
  vPortExitCritical, vPortETSIntrLock, vPortETSIntrUnlock,
  PendSV. Already implemented in shim_xtensa.c.
- Heap (4): _heap_caps_malloc/zalloc/free,
  heap_caps_get_dram_free_size. Already wrapped in
  shim_xtensa.c (pvPortMalloc path + NMI pool).
- ROM / SDK support (~14): ets_printf, ets_delay_us,
  ets_timer_proc, os_timer_* (4), esp_timer_get_time,
  esp_log_write/early_timestamp, esp_sleep_lock/unlock,
  lldesc_build_chain (ROM 0x40004f40), bit_popcount,
  clockgate_watchdog. All already provided or stubbed by
  blob_support.c / shim_xtensa.c (B16 closure).
- libphy boundary (7): phy_open_rf, phy_close_rf,
  phy_get_bb_freqoffset, read_hw_noisefloor,
  tx_pwctrl_background, g_phyFuns, plus pm_* RTC helpers
  (pm_rtc_clock_cali, pm_usec2rtc*, pm_set_sleep_cycles,
  pm_wakeup_init) whose implementations live outside
  libpp. This is the libpp->libphy seam: a function-pointer
  table (g_phyFuns) plus ~6 direct calls, all in pm.o /
  wdev.o / pp.o noise paths. Narrow and enumerable.
- libnet80211 boundary (~10): g_ic (the 1704-byte softc
  global, SPEC section 3), sta_input, hostap_input,
  ieee80211_getmgtframe, ieee80211_send_setup,
  ieee80211_recv_action, ieee80211_ethbroadcast,
  chm_get_current_channel, promiscuous_cb,
  TmpSTAAPCloseAP, PendFreeBcnEb, noise_check_loop,
  sig_max_rate2, esp_wifi_try_rate_from_high,
  wifi_nvs_get_sta_listen_interval, g_wifi_nvs,
  g_wifi_menuconfig (config data, already ours to fill).
  This is the upward seam: management frames and RX
  delivery cross it. Replacing libpp first means keeping
  these exact entry points so blob net80211 still links.
- libc/compiler (9): memcpy/memset/memcmp/printf +
  __divsi3/__muldi3/__udiv*/__umod*. newlib + libgcc,
  already linked.

## Inferred: replacement order inside libpp

DESIGN 6.1 already fixes frame plumbing first. Inside
libpp that decomposes by risk, lowest first:

1. esf_buf - DONE as decode (B22); clean-room pp_esf
   exists. Remaining: link-level swap behind the pp queue.
2. rate_control + trc - pure rate tables and arithmetic,
   no MMIO, host-testable against recorded blob outputs.
   The schedule tables are .data constants measurable
   from the archive itself.
3. pp.o queues/task - pure logic over our OSI seam;
   MacIsrSigPostDefHdl/pp_post behaviour is already
   instrumented on hardware (B21 g_pp_posts).
4. if_hwctrl - thin glue over g_cnxMgr; needs the
   g_cnxMgr layout (1224 B COMMON, fields open).
5. lmac - TX exchange sequences; touches 13 registers
   whose roles are partly open (SPEC section 6).
6. pm - sleep/RF power sequencing; calls libphy RF
   open/close, so it cannot be proven without the PHY
   blob still present underneath. Keep blob PHY for this.
7. wdev - the hardware driver proper, last in libpp:
   wDev_ProcessFiq (5206 B) plus init. Blocked on SPEC
   section 6 open registers (0x3ff2006c, 0x3ff20c74,
   0x60009800 baseband bank) and the wdev control struct
   past +0x34.

## Open (blocks wdev/lmac replacement, not the rest)

- g_cnxMgr (1224 B) and wDevCtrl (44 B exported COMMON,
  struct past +0x34 open per SPEC section 6).
- Which of wdev.o's 91 WDEV literals are configuration
  vs per-frame hot path; the B4 direction split
  (inventory/b4/wdev-direction.csv, linked image) needs
  redoing at object granularity for the RTOS build.
- cfg[3]/cfg[17] Kconfig names (carried from B22).
- Whether trc schedule tables vary by phy mode at
  runtime (rcUpdatePhyMode suggests yes; table swap
  mechanism open).

## Next step

Swap order 1-3 are all provable without touching a
register: implement pp_esf as a link-compatible
esf_buf replacement (exact exported names
esf_buf_setup/alloc/recycle, esf_rx_buf_alloc,
esp_wifi_{get,set}_rx_pbuf_mem_type) so blob pp.o links
against our buffer layer, host-test it against the B22
spec vectors, then flash esp9 with that one object
replaced and run the bench. That is the first
blob-object-for-ours swap in the project.

