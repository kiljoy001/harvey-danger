# B1 disassembly - stock ESP8266 app (RE stage B1)

Continues `B0-INVENTORY.md` (preserved unchanged). Input is the B0
extraction; the board and the stock backup were not touched in B1.
All analysis is offline on this workstation.

## Method

1. `scripts/build_elf.py` builds `images/user1.elf`, an ELF32 Xtensa
   (EM_XTENSA=94) with PT_LOAD segments so each part lands at its true
   address in one Ghidra program:
   `.irom0.text` 0x40201010/383536, `.iram0.text` 0x40100000/31572,
   `.dram0.data` 0x3ffe8000/2188, `.dram0.bss_like` 0x3ffe8890/8856,
   entry 0x40100004. (First build had PF_* values in section-header
   flag fields; Ghidra then marked code non-executable and found 0
   functions. Fixed to SHF_ALLOC|SHF_EXECINSTR and re-analysed -
   recorded here because the 0-function result was an artifact bug,
   not image evidence.)
2. Ghidra 12.2 headless (`/opt/ghidra/support/analyzeHeadless`),
   language `Xtensa:LE:32:default`, project in `ghidra/B1Proj`.
3. `scripts/B1Dump.java` dumps memory blocks, every function
   (entry/size/callee/caller counts) and xrefs to every defined
   `*.c` filename string - the assert `__FILE__` anchors.
4. `scripts/cluster_map.py` turns anchors + functions into the
   link-order cluster map in `inventory/b1/clusters.md`.

Outputs: `inventory/b1/program.txt`, `inventory/b1/functions.csv`
(2321 rows), `inventory/b1/anchors.csv` (83 anchors),
`inventory/b1/clusters.md`.

## Observed

- **Base address validated.** Ghidra auto-analysis on
  0x40201010-based IROM produced 2321 functions (2082 IROM, 239 IRAM,
  332146 function bytes, median function 78 bytes). Decisive checks:
  the ELF entry point 0x40100004 was recovered as a function Ghidra
  itself names `entry`; defined strings sit exactly at
  0x40200000 + flash offset (e.g. `ieee80211.c` at 0x4025bee0);
  and code-to-string xrefs resolve in tight, monotonically ordered
  clusters (below), which only happens when L32R literals decode to
  the right addresses. The B0 inference is now treated as proven for
  IROM/IRAM addressing.
- **Wi-Fi clusters** (anchor = functions referencing the filename
  string; span = bytes from this unit's first anchor to the next
  unit's first anchor, an upper bound that includes unanchored
  neighbours):

  | unit | anchor functions | xrefs | est. region |
  | --- | --- | --- | --- |
  | wdev.c (libpp) | 0x40105fd8, 0x401060b0, 0x40106200 (IRAM) | 9 | IRAM-resident |
  | esf_buf.c (libpp) | 0x40220924, 0x40220b0c | 17 | ~1434 B, 11 funcs |
  | ieee80211.c | 0x40220fac | 11 | ~979 B, 6 funcs |
  | ieee80211_hostap.c | 0x402216b0-0x40222af8 (5) | 21 | ~7958 B, 36 funcs |
  | ieee80211_input.c | 0x402238bc | 2 | ~2262 B, 10 funcs |
  | ieee80211_mesh_quick.c | 0x40224250 | 2 | ~2844 B, 14 funcs |
  | ieee80211_output.c | 0x40224e98 | 2 | ~5171 B, 14 funcs |
  | ieee80211_phy.c | 0x4022630c | 3 | ~782 B, 10 funcs |
  | ieee80211_regdomain.c | 0x40226680, 0x402266a8 | 3 | ~2428 B, 31 funcs |
  | ieee80211_scan.c | 0x40227118-0x40227350 (3) | 7 | ~1974 B, 9 funcs |
  | ieee80211_sta.c | 0x402278f4, 0x402280e8 | 7 | ~5732 B, 31 funcs |
  | wl_cnx.c | 0x402290dc-0x4022a2bc (7) | 16 | ~8004 B, 58 funcs |
  | pm_for_bcn_only_mode.c | 0x4021d4f4, 0x4021d584 | 3 | before esf_buf |

  Full 60+ unit map (AT commands, lwIP, WPA, SSL, WPS, smartconfig)
  is in `inventory/b1/clusters.md`; link order in the image follows
  the string-table order found in B0, as expected for a stripped
  single link.
- **libpp is partly IRAM-resident**: every `wdev.c` anchor is at
  0x40105xxx-0x40106xxx, not IROM. That fits timing-critical frame
  handling code that cannot wait on flash cache.
- Two strings were *not* found by Ghidra's defined-string iterator
  under their full text (`phy ver: %d, pp ver: %d.%d`,
  `SDK ver: %s ...`) - they exist in the strings dump at known
  addresses (0x40259aa0 / 0x40259a71 in B0), so this is a
  string-definition gap in analysis, not missing content.

## Inferred

- Anchor functions are the functions in each unit that contain
  asserts/logs mentioning `__FILE__`; units have more functions than
  anchors. Region spans are therefore upper bounds, tightest where
  anchors are dense (hostap, wl_cnx) and loosest for single-anchor
  units (ieee80211_phy.c especially - the real PHY code is certainly
  larger than 782 B and likely lives in unanchored neighbouring code
  and/or IRAM).
- The 9696-byte "function" at 0x40242348 (ssl region, 0 callees) is
  almost certainly data mis-disassembled as code; function count and
  byte totals carry a few percent of such noise.

## Unknown (next: B2)

- No function has a real name yet; all are `FUN_<addr>` except
  `entry`. Naming needs either SDK 2.0.0 `.a` symbol matching
  (still not on this machine) or manual identification from
  string/behaviour analysis, cluster by cluster.
- Hardware register, DMA and ROM-function surface: not yet extracted.
  Callees outside the mapped segments (ROM at 0x4000xxxx, MMIO at
  0x3ff0xxxx-0x6000xxxx) were not dumped in B1.
- Exact object boundaries between adjacent units.
- Whether Ghidra's Xtensa model handles all LX106 windowed-ABI
  variants correctly here; decompiler output has not yet been
  spot-checked against hand-decoded instructions.

## Next steps

1. Dump external callees (ROM/MMIO targets) per function - that is
   the B2 hardware-surface input and also names many blob functions
   by what they call.
2. Spot-check decompiled output for the anchor functions
   (0x40220fac ieee80211.c, 0x40105fd8 wdev.c) against the raw
   instructions before trusting bulk decompilation.
3. Start function naming inside the ieee80211_hostap cluster,
   the densest and therefore most tractable Wi-Fi cluster.
4. In parallel (unchanged from B0): obtain NONOS SDK 2.0.0 libraries
   at a pinned source, hash and record provenance, then diff their
   object/function size fingerprints against `functions.csv`.

