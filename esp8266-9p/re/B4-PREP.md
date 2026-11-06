# B4 prep - boundaries resolved, WDEV direction, DMA first look, g_ic fields

Continues B3. Prior reports, backup, and extracted images unchanged.
Offline only; the board was not touched. Scripts: `B4Inspect.java`,
`B4Rename.java`; data in `inventory/b4/`.

## 1. hostap_input located

SDK fingerprint matched `hostap_input` at 0x40221a6c, but no Ghidra
function contained that address. Inspection (`inventory/b4/
probe-listing.txt`) shows the previous function ends with `ret.n`
at 0x40221a65, then 8 bytes at 0x40221a66-0x40221a6d that are a
literal-pool word pair (bytes include a 0x40107a40 pointer), then
code resumes at 0x40221a74 with frame-control parsing (`extui`
type/subtype extraction) - unmistakably the input path.

Conclusion: the fingerprint alignment lands on the literal pool the
linker placed immediately before this function's text for this
object; the function itself is Ghidra's 0x40221a74 (1184 bytes vs
SDK 1207 - the usual few-byte drift). Renamed `hostap_input`.
Lesson recorded for other matches: a fingerprint start can be up to
a literal pool (here 8 bytes) before the true entry.

## 2. Conflict at 0x40221810 resolved: it is teardown

Caller analysis settles B3's open conflict. The function's only
caller is 0x40222af8, and its only callee is `mem_free_fileline`.
Decompiling the caller shows the softap-stop path: it loops over
associated stations sending disassociation (0xa0) and deauth (0xc0)
management frames via `ieee80211_send_mgmt`, frees two `g_ic`
pointers (+0x1a0, +0x1a4, hostap file/line refs), recycles buffers,
and only then calls 0x40221810, whose whole body frees the hostap
structure chain (+0xcc and below, B2).

So: 0x40221810 is renamed back to `hostap_teardown` (B2's name was
right), and 0x40222af8 is named `wifi_softap_stop` (medium
confidence: SDK size for that symbol is 447 vs 412 here, but the
behaviour - deauth-all plus teardown - is specific). The SDK
`ieee80211_hostap_attach` size coincidence (203 vs 202) is judged
exactly that. If attach exists in this build, it is a different,
still-unnamed function.

## 3. WDEV direction split (71 registers in 0x3ff20000-0x3ff20fff)

`inventory/b4/wdev-direction.csv` classifies every WDEV reference
by load vs store mnemonic from named functions:

- 29 read/write, 17 read-only, 25 write-only registers.
- Write-only cluster 0x3ff20008-0x3ff20010 and 0x3ff2007c-
  0x3ff20088: written only by 0x4021ff18 (the DMA/wdev init path,
  section 4) - one-time hardware setup registers.
- Read-only 0x3ff20014/18/1c: read only by `wDev_ProcessFiq`
  (0x4010584c) - FIQ status/introspection registers.
- Read-only 0x3ff20038/3c: read by `wdev_frame_process` and
  neighbours - RX status.
- 0x3ff2006c remains the hottest register (25 loads / 25 stores
  across 12+ functions) - read-modify-write central control,
  still unnamed semantically.
- Semantic anchors from B3 stand: BSSID at 0x3ff20c3c/44
  (`wDev_SetBssid`), own-MAC near 0x3ff20c5c, RX mode/filter pair
  0x3ff20800/04 (sniffer + crypto functions).

## 4. DMA descriptors: first look

ROM `lldesc_build_chain` (0x40004f40, named in B3) is called only
from 0x4021ff18, the wdev init function - via the pointer cell at
0x4021ff10, three times with chain bases that the decompiler shows
as constants (0x48/0x50/0x60-spaced descriptor arrays and buffer
sizes 0x644, 0x20/4, 0x700/0x100). The same function then walks a
linked list of descriptors:

- descriptor word 0: two 12-bit fields are manipulated -
  `(*desc & 0xfff) - 4` and `(*desc & 0xfff) << 0xc` - i.e. length
  and size live in bits [11:0] and [23:12] (order to confirm),
  with flag bits above.
- descriptor word 2 (+0x08): next-descriptor pointer
  (`puVar5 = (uint*)puVar5[2]` loop).
- word 1 (+0x04) is not touched in this walk.

That is a 12-byte (minimum) descriptor: {len/size/flags, ?,
next}. It matches the shape `wDev_AppendRxBlocks` and
`wDev_AppendRxAmpduLensBlocks` manage. A written descriptor spec
needs the field order in word 0 confirmed from the RX path, which
is the first B4 task below.

## 5. g_ic field harvest (updated)

Base: `g_ic` at 0x3fff06d4 (1704 bytes, B3). New/updated fields
from `wifi_softap_stop` and the sta anchor function 0x402278f4:

- +0x10: sub-struct pointer (688-byte alloc, B3). Its +0xd0 holds a
  mode/state byte compared against 2, and +0xd2 a state byte
  tested for 4 then set to 1 (sta path).
- +0x14: softc pointer. Word-indexed evidence from softap stop:
  softc+0x90 is a value saved/restored around management sends
  (list head or current-station pointer), entries are reached at
  softc+0x98 + n (per-station records), softc+0xd0 is a flags word
  whose 0x1000000 bit gates the stop path.
- +0x1a0, +0x1a4: two pointers freed with hostap.c line refs on
  softap stop (beacon/buffer structures are the natural reading;
  not proven).
- +0x220: a byte copied into frame construction by the sta path
  (config/state byte; role open).

Everything is still offset-level: no field has a proven type beyond
pointer/byte/word usage in one or two functions.

## 6. Project state after B4 prep

Renames applied this stage (`scripts/B4Rename.java`):
`hostap_input` (0x40221a74), `hostap_teardown` restored
(0x40221810), `wifi_softap_stop` (0x40222af8, candidate).
Plus B3's 33 SDK names and B2's 8. The remaining unnamed Wi-Fi
core is now mostly local helpers between the named anchors.

## 7. Next (B4, first replacement)

1. Confirm DMA descriptor word-0 field order from
   `wDev_AppendRxBlocks` / `wDev_ProcessFiq` decompilation, and
   write the descriptor + WDEV register spec (0x3ff20000) as a
   document the clean-room code will be written against.
2. Continue `g_ic`/softc field harvest from `hostap_input`,
   `sta_input`, `ieee80211_send_mgmt` decompilations.
3. First clean-room target stays libpp frame plumbing
   (esf_buf + wdev queue functions, named in B3) per DESIGN 6.1,
   built and tested behind the existing blobs - no flashing of
   blob-free code until the spec in (1) exists.

