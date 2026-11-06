# B2 - ROM/MMIO surface, decompiler spot-check, first names

Continues `B1-DISASSEMBLY.md`; B0/B1 reports and the backup are
unchanged. Offline only: the board was not touched.

## Method

- `scripts/B2Dump.java` over the saved Ghidra project: every CALL
  reference landing outside IROM/IRAM (external surface), every DATA
  reference into ROM/MMIO ranges, and every defined-string xref from
  functions in the Wi-Fi regions (wdev IRAM 0x40105f00-0x40106400 and
  IROM 0x4021d000-0x4022b000). Output in `inventory/b2/`:
  `external-calls.csv`, `data-refs.csv`, `wifi-string-xrefs.csv`
  (148 string xrefs).
- `scripts/B2Decompile.java`: C + raw listing for four anchor
  functions in `inventory/b2/decomp/`.
- `scripts/B2Rename.java`: candidate names applied to the Ghidra
  project only (never to the extracted images).

## Observed

### External call surface (all ROM)

- 76 distinct ROM targets, 1908 call references, all in
  0x40000000-0x4000c268. No direct calls to any other external region:
  everything the blobs need from outside the image goes through ROM
  helpers or MMIO data access.
- Hottest ROM targets (identity unknown, ranked for B3 naming):
  0x400018b4 (576 refs / 280 callers), 0x400018a4 (184/127),
  0x400018d4 (166/99), 0x40002ac8 (131/54), 0x40002ae8 (94/63),
  0x400024cc (93/53), 0x4000e21c (85/58), 0x40000f80 (73/43),
  0x40000f74 (69/45), 0x4000dc88 (60/30), 0x40002ecc (39/29),
  0x40000e24 (32/28). The 0x400018xx cluster being called from
  nearly every subsystem marks it as the common runtime core
  (memcpy/memset-class helpers are the usual shape of this pattern;
  that is a hypothesis to prove by decompiling one caller context,
  not a finding).

### MMIO data surface (239 distinct addresses)

By 64 KB bank:

- 0x60000000: 144 addresses, 893 refs - the peripheral block
  (UART/SPI/GPIO class registers live here on ESP8266; per-register
  attribution is still open).
- 0x3ff20000: 82 addresses, 384 refs - the WDEV/MAC register block.
  Referenced heavily from both the IRAM wdev functions
  (0x40105xxx-0x40106xxx) and the IROM ieee80211 functions, which
  confirms this bank as the Wi-Fi MAC hardware surface for B3.
  Notable single registers: 0x3ff2006c (50 refs / 13 functions),
  0x3ff20004, 0x3ff20800.
- 0x3ff00000: 13 addresses, 101 refs - DPORT. Hottest: 0x3ff00014
  (28 refs), 0x3ff00018 (21 refs).

### Pointer-table discovery

The start of IROM is a function-pointer table, not code. Cells read
directly from `user1-irom.bin`:

- *0x40201448 -> 0x401018a0: the printf-like routine called with
  format strings everywhere (`mode : `, MAC formats, assert prints).
- *0x40201764 -> 0x40100e24: allocation, called (size, file, line).
- *0x40201778 -> 0x40100d9c: free, called (ptr, file, line).

Callers go through the cells (`l32r` + `callx0`), so B1's
called-function counts understate these three. Any blob replacement
must preserve this table contract or update every call site.

### Decompiler spot-check (verdict: usable, with care)

- `FUN_40220fac` (ieee80211.c): raw listing and C agree instruction
  by instruction on the mode switch (`beqi` 1/3, `addi`/`beqz` for
  2), MAC byte loads at struct offsets 0x1e-0x23 for the
  `%02x:...` print, and the (size, file, line) allocation calls
  with sizes 0x2a0/0x110 and line constants 0x22e/0x234/0x23a.
  Control flow and call arguments decompile faithfully.
- `FUN_40105fd8` (wdev.c, IRAM): frame-control bit tests
  (`>>0xe`, masks 0x80/0x7f/0x40000000), a 12-byte header copy,
  assert-fail print followed by an infinite loop - all match the
  listing. Types are invented (`ushort*` for frame words) but
  offsets and constants are real.
- Caveat: the C shows no ENTRY/prologue instruction at the head of
  `FUN_40220fac` (listing starts `addi a1,a1,-0x30` with register
  saves), so function boundaries may be off by the ENTRY opcode at
  some sites. Sizes in B1/B2 carry that uncertainty.

## First names (candidates, applied in Ghidra by B2Rename.java)

Confidence wording is deliberate: these are evidence-based
candidates, not recovered symbols.

- 0x40100e24 `mem_alloc_fileline` - (size, file, line) alloc via
  pointer cell; high confidence in role, exact SDK name unknown.
- 0x40100d9c `mem_free_fileline` - matching free; high confidence.
- 0x40100cd4 `mem_alloc_fileline_iram` - IRAM twin used by wdev;
  medium-high (same call shape, IRAM resident).
- 0x401018a0 `printf_like_401018a0` - format-string sink; high
  confidence in role.
- 0x40220fac `ieee80211_opmode_setup` - mode switch + MAC print +
  main struct allocation in ieee80211.c; medium-high.
- 0x40221810 `hostap_teardown` - frees the linked hostap structure
  chain field-by-field with hostap.c line refs 0x1ab-0x1c3; high.
- 0x40226680 `regdomain_free_field130` - frees struct +0x130 with
  regdomain.c line 0x39; high for what it does, name is descriptive.
- 0x40105fd8 `wdev_frame_process` - wdev.c frame-control parsing and
  header handling in IRAM; medium.

Descriptive strings in the hostap region are almost only the
filename anchor itself - hostap naming must come from behaviour
(callees, struct offsets, MMIO), not strings. The one exception in
the wider Wi-Fi region is `no buf for action frame` at 0x4025bfc0,
referenced by function 0x40223574.

## Unknown / next (B3)

- Identity of the 76 ROM targets; the 0x400018xx core first, proven
  from caller context + argument shapes, one target at a time.
- Per-register MMIO attribution inside 0x60000000 and 0x3ff20000;
  needs access-width/order analysis (loads vs stores) per address,
  which B2 did not split.
- Real SDK symbol names - still blocked on obtaining the NONOS SDK
  2.0.0 libraries at a pinned source (unchanged from B0/B1).
- Struct layouts: the recurring structs (globals via *0x4020101c,
  hostap chain at +0xcc/+0x18/+0x38..) are the highest-value next
  target; several candidate names above are really statements about
  those structs.

