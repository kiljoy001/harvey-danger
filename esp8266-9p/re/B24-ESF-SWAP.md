# B24 - First blob object replaced: clean-room esf_buf

Status: DONE (decode + implementation + host tests + Xtensa
compile + esp9 link + on-device boot/scan/listen + stock
restore verified). This is the first blob-object-for-ours
swap in the project: libpp.a still links, but its
esf_buf.o member is never pulled.

## What was done

1. Resolved the B22 open item cfg[3]/cfg[17].
   wifi_menuconfig_init in libnet80211.a(ieee80211_api.o)
   was disassembled: it validates wifi_init_config_t
   (magic 0x1f2f3f4f at +32) and copies input byte 26 to
   g_wifi_menuconfig[3] and byte 28 to [17]. Against the
   SDK header layout those are rx_pkt_num and tx_buf_num.
   esp9 sdkconfig sets 7 and 6, so esf_buf_setup builds
   8 RX buffers (type 7), 6 TX buffers (type 1), 8 mgmt
   buffers (type 5), 22 esf_buf total. SPEC section 5.3
   and 6 updated. The RX pool being only 8 deep sharpens
   the B21/B22 stall theory: 8 unrecycled buffers kill RX.

2. Implemented firmware/libpp/esf_buf.c (+ esf_buf.h),
   link-compatible with the blob object: same exported
   symbols (esf_buf_setup, esf_buf_alloc, esf_buf_recycle,
   esf_rx_buf_alloc, esp_wifi_{get,set}_rx_pbuf_mem_type),
   same 68-byte pp_esf_blob ABI, same three free lists,
   same setup carving (TX descriptors from the
   wDevCtrl+24 ring in descending order, payload base from
   descriptor word 1; mgmt sbuf word 0 masked |64; RX
   8-byte headers), same ETSIntrLock discipline, heap_caps
   allocation for type 4 with the blob's source-line tags.
   Two documented divergences, both where the blob
   crashes: unknown alloc type returns NULL instead of
   trapping, and type-4 failure cleanup omits the blob's
   DRAM range re-checks. Unlike the blob, ours exports
   g_free_rxblock_eb_cnt and pp_esf_*_free_count() so the
   RX pool level is finally readable (B22 probe).

3. Host test firmware/libpp/test/test_esf_buf.c over
   malloc-backed hooks: pool counts 6/8/8 at esp9 config,
   RX exhaustion at exactly 8, type-1 field semantics,
   16-bit len truncation, type-1 recycle compose checked
   against pp_esf_compose_ctrl, type-4 heap balance,
   mem-type accessor gating. All pass, plus the existing
   test_pp and test_kernel. (Found en route: the 68-byte
   struct size is target-only; pool allocation now uses
   sizeof so host builds are correct too.)

4. esp9 integration: esf_buf.c/pp_esf.c/pp_lldesc.c are
   symlinked into components/freertos like the other
   kernel files, and firmware/esp9/Makefile links the
   three objects directly via EXTRA_LDFLAGS - all archives
   sit in one --start-group, so archive order alone cannot
   stop the linker pulling the blob member. Verified in
   esp9.map: zero references to libpp.a(esf_buf.o);
   esf_buf_setup at 0x40211e14 is ours. Memory gate green:
   bss 68432, heap headroom 27296 (floor 24576).

## On-device verification (/dev/ttyUSB1, then restored)

Flashed bootloader/partitions/esp9.bin with esptool
(write verified). UART at 74880 baud:

    esp9: app_main entered (shim scheduler)
    esp9: MILESTONE init-returns heap=34032
    esp9: wifi_start rc=0
    esp9: MILESTONE pp-task heap=32912
    esp9: scan rc=0 ap_num=8
    esp9: MILESTONE scan-ok
    esp9: MILESTONE 9p-listening tcp 564
    HB rx=00000016 last=2 runs=246755 (repeating, no PANIC)

Scan succeeding matters: beacon/probe-response reception
runs through esf_rx_buf_alloc and type-7 recycle, i.e.
our pools, under blob wdev/lmac. No PANIC, no STACKBLOW.
Stock backup then reflashed (hash verified) and the
board answers AT / AT+GMR (AT 1.3.0.0, SDK 2.0.0).

## Open / next

- Provisioned bench (join + sustained 9P traffic) was not
  run: blob_config.h carries no credentials. That run is
  also the B21 stall reproducer, and pp_esf_rx_free_count
  is the probe B22 asked for; do both together next.
- Remaining libpp order (B23): trc/rate_control (pure
  tables), pp.o queues, if_hwctrl, lmac, pm, wdev last.
- wdev.o replacement still blocked on SPEC section 6
  registers (0x3ff2006c, 0x3ff20c74, baseband bank).
