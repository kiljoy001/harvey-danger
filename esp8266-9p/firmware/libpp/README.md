# libpp (clean-room, first components)

First clean-room code for the plan9ized ESP8266 firmware, written
only against `../../doc/SPEC-WDEV-DMA.md` (derived from the RE in
`../../re/`). No blob or SDK code is used or linked.

## Contents

- `pp_lldesc.h` / `pp_lldesc.c` - the 12-byte DMA descriptor
  (spec section 1): init, size/len accessors, RX offer with FCS
  reserve, completion test, chain building. `buf`/`next` are
  32-bit hardware addresses by design, so the struct is 12 bytes
  on any host; on the 32-bit target the conversion helpers are
  identity casts.
- `pp_esf.h` / `pp_esf.c` - a fixed-size frame-buffer pool, one
  descriptor per buffer, free-list alloc/free. This replaces the
  *role* of the blob esf_buf layer. It is not ABI-compatible with
  the blob's esf_buf struct. That layout is now fully decoded
  (68 bytes, spec section 5, re/B22) and mirrored as pp_esf_blob
  in pp_esf.h, with the type-1 recycle control-word compose
  implemented as pp_esf_compose_ctrl; integration still happens
  behind the existing blobs at the queue level first, not by
  link replacement.
- `rate_control.h` / `rate_control.c` - link-compatible
  replacement for the blob `rate_control.o` (B31): the
  32-record rate table (reconstructed from
  RC_SetBasicRate's store sequence), ACK/RTS rate lookup,
  and ACK/CTS/BlockAck airtime arithmetic. esp9 links it
  directly; the blob member is never pulled.
- `trc_tables.h` / `trc_tables.c` - blob trc.o schedule
  tables extracted as source (B31 stage 1).
- `trc.h` / `trc.c` - link-compatible replacement for
  the blob `trc.o` (B36 stage 2): the per-station PER
  adaptation state machine (9 records, rcGetSched /
  rcGetRate / rcUpdateTxDone / rcUpdatePhyMode /
  rcLowerSched / rssi_margin) walking the stage-1
  tables. esp9 links it directly; the blob member is
  never pulled (0 `libpp.a(trc.o)` refs in the map).
- `esf_buf.h` / `esf_buf.c` - link-compatible replacement for
  the blob `esf_buf.o` (B24): exact exported symbols and the
  68-byte blob ABI, three free lists, setup carving from
  `g_wifi_menuconfig` (byte 3 = rx_pkt_num, byte 17 =
  tx_buf_num). esp9 links this object directly so the blob
  member is never pulled. Also exports the RX pool level
  (`pp_esf_rx_free_count`) the blob kept unreadable.
- `pp_arena.h` / `pp_arena.c` - Goose-style data stack (PLAN9-OS
  section 4.5): bump allocation in a fixed slab, scoped
  mark/release, one growable buffer that never moves, high-water
  reporting. This is the kernel's transient-memory mechanism;
  DMA memory is never allocated from it.

## Scope limits

- Nothing here touches WDEV registers yet. Bring-up still uses
  blob init (spec section 4, rule 4); the init writes recorded in
  spec section 2.1 are replay candidates for a later stage.
- `lldesc_build_chain` (ROM) is not called; chains are built by
  `pp_desc_chain` from the spec.

## Host test

    cc -std=c99 -Wall -Wextra -Werror -o /tmp/test_pp \
      pp_lldesc.c pp_esf.c pp_arena.c test/test_pp.c
    /tmp/test_pp
    cc -std=c99 -Wall -Wextra -Werror -o /tmp/test_esf_buf \
      esf_buf.c pp_esf.c pp_lldesc.c test/test_esf_buf.c
    /tmp/test_esf_buf
    cc -std=c99 -Wall -Wextra -Werror -o /tmp/test_rate \
      rate_control.c test/test_rate.c
    /tmp/test_rate
    cc -std=c99 -Wall -Wextra -Werror -o /tmp/test_trc_tables \
      trc_tables.c test/test_trc_tables.c
    /tmp/test_trc_tables
    cc -m32 -std=c99 -Wall -Wextra -Werror -DTRC_HOST_TEST \
      -o /tmp/test_trc trc.c trc_tables.c test/test_trc.c
    /tmp/test_trc
    cc -m32 -std=c99 -Wall -Wextra -Werror -DPP_HOST_TEST \
      -o /tmp/test_ppqueue pp.c test/test_ppqueue.c
    /tmp/test_ppqueue
    cc -m32 -std=c99 -Wall -Wextra -Werror -DPM_HOST_TEST \
      -o /tmp/test_pm pm.c test/test_pm.c
    /tmp/test_pm
    cc -m32 -std=c99 -Wall -Wextra -Werror -DIFHW_HOST_TEST \
      -o /tmp/test_ifhw if_hwctrl.c test/test_ifhw.c
    /tmp/test_ifhw
    cc -m32 -std=c99 -Wall -Wextra -Werror -DLMAC_HOST_TEST \
      -o /tmp/test_lmac lmac.c test/test_lmac.c
    /tmp/test_lmac

Expected: `test_pp: all tests passed`.

