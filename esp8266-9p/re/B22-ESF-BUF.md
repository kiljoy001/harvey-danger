# B22 - esf_buf fully decoded (RTOS libpp), RX-stall theory sharpened

Status: DONE (decode + spec + clean-room mirror + host tests).
No device work this round; the board keeps the esp9 image from
B21's rename round, per the standing instruction to leave our
version on the device.

## What was done

Full disassembly of `esf_buf.o` from the RTOS SDK `libpp.a`
(`esf_buf_setup`, `esf_buf_alloc`, `esf_buf_recycle`,
`esf_rx_buf_alloc`, `esp_wifi_{get,set}_rx_pbuf_mem_type`),
decoded instruction by instruction, and written into
[SPEC-WDEV-DMA.md](/home/scott/Repo/harvey/esp8266-9p/doc/SPEC-WDEV-DMA.md)
section 5. This supersedes the NONOS-era esf inference for the
RTOS blobs.

## Findings (all verified in the disassembly)

- `struct esf_buf` is 68 bytes. Descriptors (+0/+4, same pointer
  stored twice), data (+12), payload base (+20, data-36 for
  type 1), length (+26), free-list next (+36), header pointer
  (+40), 12-byte callback argument area (+44), completion
  callback (+56), composed DMA control word (+64).
- Three free lists in `ebCxt`: general (+0, types 1/2), mgmt
  (+8, type 5), RX (+16, type 7 only). All under ETSIntrLock.
- The +40 header is 28 bytes for types 1/4/5 and 8 bytes for
  type 7. Type-1 recycle composes +64 from the 28-byte header
  (exact bit formula in spec 5.3), invokes the +56 callback
  with (eb+44), zeroes the header, and requeues. Type 4 is a
  heap class: recycle frees descriptor, header, payload, struct
  (blob source lines 612-616).
- `esf_buf_setup` preallocates everything at ifup from
  `g_wifi_menuconfig` byte fields: esf pool
  68x(cfg[3]+cfg[17]+9), header pool 28x(cfg[17]+8), RX-header
  pool 8x(cfg[3]+1); it carves cfg[17] type-1 buffers
  (descriptors from the `wDevCtrl+24` ring), 8 type-5 mgmt
  buffers (76-byte stride payload space), and cfg[3]+1 type-7
  RX buffers, recycling each onto its list.

## Why this matters for the RX stall (B21)

Per-frame RX allocation is a free-list pop - there is no heap
call in the frame path on the RTOS blobs. That kills the
malloc-from-NMI theory for good (already doubted in B21) and
points at the remaining silent-death mechanism: **pool
exhaustion**. The RX pool is fixed at cfg[3]+1 buffers; any
path that consumes an RX buffer without a type-7 recycle
shrinks it permanently, and when `esf_rx_buf_alloc` starts
returning NULL the MAC drops every frame while the scheduler,
TX, and heartbeat stay alive - exactly the B21 signature.
Stall onset correlating with connection churn (each dropped
connection can strand in-flight buffers) fits better than a
raw frame count.

Decisive next experiment (not done): watch the pool drain.
Follow-up scan of all 9 libpp objects: `g_free_rxblock_eb_cnt`
is defined in `esf_buf.o` and the only code reference in the
whole archive is the decrement in `esf_rx_buf_alloc` (the one
other hit is `.xt.prop` metadata). It is never initialised or
incremented - write-only telemetry, not a pool gauge, and as a
local section symbol nothing outside the object can read it
anyway. So the probe has to be the ebCxt+16 list length (no
exported accessor exists) or an indirect signal, exposed via
the existing heartbeat/alive telemetry during a bench-to-stall
run.

## Clean-room updates

- [pp_esf.h](/home/scott/Repo/harvey/esp8266-9p/firmware/libpp/pp_esf.h):
  added `pp_esf_blob`, an exact mirror of the blob struct
  (offsets compile-verified on the Xtensa target: sizeof 68,
  len at +26, next at +36, ctrl at +64), plus the class flag
  constants (0x2000/0x4000/0x8000), headroom (36), header
  sizes (28/8), and the +4 AND mask 0xfc03ffff.
- [pp_esf.c](/home/scott/Repo/harvey/esp8266-9p/firmware/libpp/pp_esf.c):
  added `pp_esf_compose_ctrl`, the type-1 recycle control-word
  compose, implemented bit-for-bit from the disassembly.
- [test_pp.c](/home/scott/Repo/harvey/esp8266-9p/firmware/libpp/test/test_pp.c):
  `test_blob_layout` checks the constants and the compose
  formula against hand-computed vectors.
- Verified: host `test_pp: all tests passed`
  (cc -std=c99 -Wall -Wextra -Werror), and the mirror struct
  passes offset assertions under xtensa-lx106-elf-gcc.

## Ghidra cross-check (2026-11-04, later)

Since REA is gated (below), the decompiler cross-check ran on
local Ghidra 12.2 headless: esp9.elf imported as Xtensa,
`esf_rx_buf_alloc` (0x401016c8), `esf_buf_alloc` (0x40264dcc),
`esf_buf_recycle` (0x40264f94), `esf_buf_setup` (0x402650a8)
decompiled; extract saved as
[esf_decomp.txt](/home/scott/Repo/harvey/esp8266-9p/re/ghidra/esf_decomp.txt).
Result: every load-bearing claim in spec section 5 matches the
decompiler's independent view - struct offsets (+0/+4 desc,
+12 data, +20 base, +26 len, +36 next, +40 header, +56
callback, +64 control word), the three ebCxt lists at
+0/+8/+16, the type-1 compose formula bit for bit, the
28-vs-8-byte headers, and the setup pool arithmetic
68x(cfg[3]+cfg[17]+9), 28x(cfg[17]+8), 8x(cfg[3]+1) with
recycles of type 1 (x cfg[17]), 5 (x8), 7 (x cfg[3]+1).
Two bonuses: the blob's log strings name the header pools
"tdesc"/"rdesc" (type-1 = TX-side, type-7 = RX-side), and
`esp_wifi_internal_free_rx_buffer` decompiles to a wrapper on
`ppRecycleRxPkt` - the named RX-buffer return path for the
stall hunt. No discrepancies found; nothing in spec 5 needed
correction.

## REA MCP check (2026-11-04, later)

At the user's direction the `rea` MCP server was tried as the
RE tool for this work. It cannot load any of our artifacts:
`libpp.a` (archive: unsupported format), `esp9.elf` and
`esf_buf.o` (both rejected as unsupported ELF architecture -
Xtensa; the server's providers cover x86/x86_64/ARM/ARM64
only). Blob RE therefore stays on the local Xtensa toolchain
(objdump/nm, verified above) and the existing Ghidra-headless
setup. Revisit only if an Xtensa-capable provider appears.

Refined after an online check the same day: Ghidra has had
mainline Xtensa support since 11.0, and the local install
(/opt/ghidra, version 12.2) ships the Xtensa processor
(xtensaArch.sinc, xtensa.cspec, little-endian .sla present) -
that is what the project's earlier Ghidra work used. The REA
rejection is REA's own artifact gate, not Ghidra's limit:
forcing provider_id=ghidra on open_binary fails identically,
and REA's tool schemas enumerate only x86/x86_64/arm/arm64
ELF architectures. So even though REA can drive a local
Ghidra, it will not pass an Xtensa ELF to it in this
configuration.

FIXED the same day at the user's insistence: REA is patched
locally. Upstream main (6.8.0) is cloned at ~/rea-agents with
a small Xtensa admission patch (EM_XTENSA in the ELF family
table, xtensa in the Ghidra provider's supported set, xtensa
declined in Hopper, PE/raw type narrowing to match); full
details, changed-file list, and verification record are in
~/rea-agents/XTENSA-PATCH.md. Typecheck clean, architecture
guards pass, 13,794 focused tests pass, and an MCP stdio
harness against the patched server opens esp9.elf as
architecture "xtensa" and returns Ghidra decompilation of
esf_buf_recycle matching the headless decode above.
~/.claude.json mcpServers.rea now runs the patched tree
(backup at ~/.claude.json.pre-xtensa-fix); a session restart
is needed for live MCP tools to pick it up. Upstream REA
updates will need the patch re-applied.

## Open

- Which Kconfig counts cfg[3] and cfg[17] are.
- (Resolved) `g_free_rxblock_eb_cnt` writer: nobody - see above.
- Unmapped esf_buf offsets: +16, +24, +28..+35, +48..+55, +60.
- Setup loop 1 loads every type-1 buffer's +20 from the same
  ring word (`wDevCtrl+24` +4); whether the ring is built
  before setup runs, or +20 is fixed up later, is unresolved.
- The mgmt sbuf word-0 frob in setup (`&= 0xfffff000; |= 64`
  per buffer, with memw) is recorded here but not in the spec;
  its register meaning is open.
