# B5 - init values harvested, RX queue struct decoded, first clean-room code

Continues B4. Prior reports, backup, extracted images unchanged.
Offline only; **nothing was flashed** and no code ran on the board.

## 1. Wdev init register values (into SPEC section 2.1)

Decompiled wdev DMA init 0x4021ff18 and its orchestrator
0x4022013c (104 bytes, SDK size match for `wDev_Initialize`; calls
`wDev_Option_Init`, FUN_40220664, 0x4021ff18, FUN_402200bc,
FUN_402200dc). All literal cells were resolved to constants from
the image. The register bases in the blobs are offset bases:
0x3ff1fe00 is used as WDEV-0x200, 0x3ff20a00 as a second base.

Harvested writes are tabulated in SPEC 2.1. The four write-only
registers 0x3ff2007c-0x3ff20088 each receive one chain-array BSS
address; 0x3ff20008 and 0x3ff2000c receive the chain #1/#3 head
values out of the wdev control struct; 0x3ff20c18 gets a magic
config word 0x2c9f0300 after first being zeroed; 0x3ff20c24 gets
0xffffffff; 0x3ff20004 is the last register touched (bit 31
cleared). Caveat recorded in the spec: the BSS addresses are
stock-build-specific. A clean build allocates its own arrays;
only the structure (which registers, in what role) carries over.
The `lldesc_build_chain` argument list is recorded verbatim but
its contract remains open, and clean-room code builds chains
itself instead of calling the ROM routine.

## 2. RX queue / wdev control struct (SPEC section 1, updated)

Correction and decode: 0x3ffe834c is a DRAM **global pointer**,
not the struct. Its stock initial value is 0x3ffecca0 (BSS). The
struct fields, from init + AppendRxBlocks with u16-indexed
offsets converted to bytes, are now tabulated in the spec:
+0x00 u16 set to 6 and +0x02 u16 zeroed (the two queue counters
AppendRxBlocks increments), +0x04 byte set to 7, +0x08/+0x0c
chain #1 outputs, +0x18 an embedded descriptor-style control
word, +0x1c a self-referential anchor (ctrl+0x24 in address
terms: ctrl+0x1c holds the value iVar3+0x24 - byte offset 0x24
from the struct base, i.e. pointing at its own +0x24 field),
+0x20 zeroed word, +0x28/+0x2c chain #2 output slots,
+0x30/+0x34 chain #3 output slots.

Also decoded this stage, for the esf side: the blob esf_buf is a
40-byte struct (stride proven in esf setup, which initialises
8+8+4+7 preallocated buffers of classes 1, 5, 7, 8), with a
12-byte lldesc at +0x04 (pointers at [1]/[2] in word terms), a
data pointer at word 4 adjusted back by 0x24 (36-byte header
allowance), the free-list next at word 8 (+0x20), and a 32-byte
auxiliary block at word 9 (+0x24) whose first word packs two
6-bit-shifted fields. The class free-list heads live in a table
at 0x3ffefc30 (BSS), indexed by class in the alloc/recycle paths.
This is recorded here and in SPEC section 5; the clean-room pool
below deliberately does not copy this layout.

## 3. First clean-room code: firmware/libpp/

Written only against SPEC-WDEV-DMA.md, no blob/SDK code:

- `pp_lldesc.h/.c` - the spec section 1 descriptor: init,
  size/len accessors, RX offer (owner set, eof/sub_sof cleared,
  len=size or size-4 with FCS reserve), completion test, chain
  building, `memw` barrier points (real `memw` under
  `__XTENSA__`). A layout lesson from the host build: `buf`/`next`
  must be explicit 32-bit hardware addresses, not C pointers -
  with pointers the struct is 24 bytes on a 64-bit host and the
  compile-time size check correctly failed until fixed.
- `pp_esf.h/.c` - fixed-size buffer pool, one descriptor per
  buffer, free-list alloc/free. Replaces the *role* of esf_buf;
  explicitly not ABI-compatible with the 40-byte blob struct.
- `test/test_pp.c` - host tests for descriptor fields, FCS
  reserve, completion/reclaim, chain linking (raw 32-bit field
  compares), pool exhaustion/reuse/double-free.

Verification (the only execution in this stage, on the host):

    cc -std=c99 -Wall -Wextra -Werror -o /tmp/test_pp \
      pp_lldesc.c pp_esf.c test/test_pp.c && /tmp/test_pp
    -> test_pp: all tests passed

Two failures on the way are recorded for honesty: the first
build failed its own 12-byte layout assert (the pointer issue
above), and the first chain test failed comparing 64-bit host
pointers against truncated 32-bit fields. Both were test/design
fixes in the new code, not spec changes.

## 4. Not done, deliberately

- No Xtensa cross-build yet (no xtensa gcc on this machine), so
  the code is host-verified C99 only.
- No integration with the blobs and no flashing: integration
  behind the blobs needs a firmware build that links SDK blobs,
  which needs the SDK *build* setup (xtensa toolchain), a
  separate step with its own provenance.
- `lldesc_build_chain` contract, 0x3ff2006c role, and esf class
  mapping remain open in SPEC section 5.

## 5. Next

1. Obtain/pin an Xtensa toolchain (provenance like the SDK) and
   cross-build libpp + a minimal RTOS-SDK firmware that calls
   pp_desc_chain/pp_buf_offer_rx behind blob init; UART log over
   the CP2102 is the test channel.
2. Decode the esf class table (0x3ffefc30) if link-level esf
   replacement is ever attempted; otherwise leave pp_esf as the
   pool and replace at queue level.
3. In parallel, the plan9ized OS phases from DESIGN section 9
   (host 9P mock first) remain untouched by this RE stage.

