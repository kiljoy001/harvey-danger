# B4 - descriptor order confirmed, WDEV/DMA spec written, g_ic harvest

Continues B4-PREP. Prior reports, backup, extracted images
unchanged. Offline only; the board was not touched. Decompilations
in `inventory/b4/decomp4/` (via `scripts/B2Decompile.java`);
the deliverable spec is `../doc/SPEC-WDEV-DMA.md`.

## 1. Descriptor word-0 order: confirmed

`wDev_AppendRxBlocks` (0x40105d4c) and `wDev_ProcessFiq`
(0x4010584c) were decompiled and every literal-pool mask resolved
to its constant by reading the IRAM literal cells (script in the
B4 working notes; values below are from the image, not guessed):

Append path, per descriptor: set bit 31 (mask 0x7fffffff, OR
0x80000000), clear bit 30 (0xbfffffff), clear bit 29 (0xdfffffff),
then `w = w & 0xff000fff | (w & 0xfff) << 0xc`. So the low 12 bits
are a pre-existing capacity and are copied into bits 23:12 when
empty RX blocks are offered to hardware. Wdev init does the same
after subtracting 4 (FCS). `wDev_ProcessFiq` accepts a completed
descriptor when bits 30/29 read as exactly `01`.

Result (full table in the spec): word 0 = `size` [11:0],
`len` [23:12], reserved [28:24], `sub_sof` [29], `eof` [30],
`owner` [31]; +0x04 buffer pointer, +0x08 next pointer, 12-byte
descriptors built by ROM `lldesc_build_chain` from wdev init.
`sub_sof` remains the one inferred name - the code only clears
bit 29; its position between `len` and `eof` and the exactly-01
completion test are the evidence.

## 2. Spec written

`../doc/SPEC-WDEV-DMA.md` is the clean-room input document:
descriptor format and lifecycle (including the `memw()` barrier
rule the blobs observe), the WDEV register table with direction
(29 R/W, 17 R-only, 25 W-only of 71) and per-register confidence,
other-bank summary (DPORT, UART/GPIO/TIMER/RTC/IO_MUX named from
eagle_soc.h; 0x60000200/500/d00/1100 and 0x60009800+ still open),
the `g_ic`/softc field table, implementation rules, and the open
items that block blob-free bring-up. Headline open items: init
register values not yet harvested, roles of 0x3ff2006c /
0x3ff20c74, RX queue control struct at 0x3ffe834c, descriptor
word-1 semantics.

## 3. g_ic / softc harvest (this stage)

From `sta_input` (0x40227c84): `g_ic+0x1f0` and `g_ic+0x1f4`
hold pointers to callback structs (word 0 and +0x08 respectively
are called when non-null) - the receive-hook slots.
From the sta anchor path (0x402278f4): `g_ic+0x220` is a byte
copied into frame construction; the `+0x10` sub-struct's +0xd0
(state, compared to 2) and +0xd2 (tested 4, set 1) confirmed.
From `ieee80211_send_mgmt`: softc+0x90 is its working head
pointer (hard failure return when zero), corroborating B4-prep.
`hostap_input` works on passed-in node/descriptor structs rather
than `g_ic`, which explains why few new `g_ic` fields come from
the hostap side. All of this is tabulated in spec section 3.

## 4. What is still not proven

- Access widths per WDEV register (32-bit assumed from referrer
  code; not split in B4).
- The `size`-minus-4 at init is attributed to the FCS by
  convention and the RX context; no comment/string in the image
  states it.
- softc per-station record layout (reached from +0x98) - only
  its existence and indexing are observed.

## 5. Next (first clean-room code)

Per DESIGN 6.1 and spec section 5: harvest wdev init register
values into the spec, decode the RX queue control struct, then
write the first clean-room libpp code (esf_buf + RX descriptor
queue handling against section 1/2 of the spec), built and run
behind the existing blobs. No blob-free flashing until the spec's
open items for the touched registers are closed.

