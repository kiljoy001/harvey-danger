# B0 inventory - stock ESP8266 flash (RE stage B0)

Target: the 4 MB stock dump in `../backup/` (SHA-256 in BACKUP.md).
Produced by `scripts/parse_flash.py` and `scripts/extract_strings.py`;
machine-readable output in `inventory/layout.json`, human summary in
`inventory/layout.txt`, strings in `inventory/user1-strings.txt`.
Method rule from DESIGN 6.1: strings are entry points only. Nothing
below claims behaviour from a string alone.

## Observed (directly measured)

- Flash is 4 MB. Two images are present and both checksums verify:
  - `images/boot.bin` at 0x000000: v1 image, 3856 bytes, entry
    0x40100438, 3 segments (IRAM 0x40100000/2408, DRAM 0x3ffe8000/776,
    0x3ffe8310/632). SHA-256 790b8e07e42813560f0621702de777d774a49
    d4a7d6e7616ad5b8bff81b16e5b. String at file 0x0d01 reads
    "2nd boot version : 1.6".
  - `images/user1-app.bin` at 0x001000: **V2 image** (magic 0xEA,
    constant segment field 4, format per esptool
    `ESP8266V2FirmwareImage`), 426208 bytes, entry 0x40100004.
    IROM segment: 383536 bytes stored at file offset 0x10 in the image,
    extracted as `images/user1-irom.bin` (SHA-256 in layout.json);
    i.e. flash-mapped at 0x40201010 if the usual
    0x40200000 + flash-offset mapping holds (see Unknowns).
    Second header segments: IRAM 0x40100000/31572, DRAM
    0x3ffe8000/2188, 0x3ffe8890/8856, each hashed in layout.json.
  - App data ends at flash 0x69140 (0x6a000 sector-aligned), matching
    the observed data run 0x000000-0x06a000 in the 4K sector map.
- Strings in the app image (3278 strings, offsets in
  `inventory/user1-strings.txt`), including at flash offsets:
  `ieee80211.c` 0x05bee0, `ieee80211_hostap.c` 0x05bf70,
  `ieee80211_input.c` 0x05bfe0, `ieee80211_mesh_quick.c` 0x05c000,
  `ieee80211_output.c` 0x05c050, `ieee80211_phy.c` 0x05c090,
  `ieee80211_regdomain.c` 0x05c270, `ieee80211_scan.c` 0x05c2a0,
  `ieee80211_sta.c` 0x05c340, `phy ver: %d, pp ver: %d.%d` 0x059aa0,
  `SDK ver: %s compiled @ Aug  9 2016 15:12:27` 0x059a71.
- Live-device cross-checks (from the AT session before the dump):
  AT 1.3.0.0, SDK 2.0.0, MAC 48:3f:da:7e:39:68, chip ESP8266EX,
  flash ID 0x5e/0x4016. The strings and the live answers agree.
- Lone non-blank sectors outside the images (4K sector map):
  0x07d000 (starts ASCII "VER2"), 0x07f000 (starts 00 ff ff ff aa55
  aa55 ...), 0x3f9000 (binary table), 0x3fb000 (starts ASCII hex
  "61add1b95d7afe11fbcf81778f9f02fe"), and the tail 0x3fc000-0x400000
  whose first sector begins 05 00 04 02 05 05 ... . Everything else
  from 0x6a000 to 0x3fc000 is 0xFF blank except those sectors.
- Toolchain present on this workstation: GNU objdump (host only, no
  Xtensa), Ghidra in /opt/ghidra with a Xtensa processor module
  (`Ghidra/Processors/Xtensa`, xtensa_le). No xtensa gcc/objdump in
  PATH at inventory time.

## Inferred (reasoned, not yet proven)

- The `ieee80211_*` filename strings plus phy/pp version prints mean
  the linked app contains the libnet80211, libphy and libpp blob code,
  compiled from sources of those names under NONOS SDK 2.0.0. Which
  bytes belong to which library is not yet known - the flash image is
  linked and stripped, so B1 needs the SDK 2.0.0 `.a` files (object
  granularity) or address-range attribution via the second-header
  segments and call-graph analysis.
- 0x3fc000 tail is the SDK system-parameter area and 0x7f000/0x7d000
  are boot/OTA bookkeeping sectors, by their position and magic-like
  heads. Their exact formats are not yet decoded.
- IROM virtual base 0x40201010 follows from the standard SPI flash
  mapping (0x40200000 + flash offset). It must be confirmed against
  return-address/call-target patterns in B1 disassembly before any
  address is trusted.

## Unknown (open for B1+)

- Function boundaries, entry points and call graph of any blob;
  no disassembly has been done yet.
- Which hardware registers, DMA descriptors and ROM functions the
  blobs touch; no MMIO trace exists yet.
- Exact format/purpose of the lone sectors listed above.
- NONOS SDK 2.0.0 `.a` libraries are not on this machine: a bounded
  search of /home/scott (depth 5, plus the RIOT `pkg/esp8266_sdk`
  package, which holds only Makefiles/patches) found no
  libnet80211.a or SDK tree. Obtaining them needs a pinned download,
  to be recorded with provenance when it happens. They would speed
  B1 but are not required - the flash image is complete, if stripped.
- Module form factor (ESP-01/ESP-12/...), still unidentified.

## Next step (B1)

1. Load `images/user1-irom.bin` (base candidate 0x40201010) and the
   IRAM segment (0x40100000) into Ghidra as Xtensa LE; auto-analyse;
   check that call targets land inside mapped ranges (validates the
   base address inference).
2. In parallel, look for NONOS SDK 2.0.0 `.a` libraries on this
   machine or fetch them at a pinned version, hash them, and record
   provenance before trusting their symbols.
3. Produce the function map for the `ieee80211_*` clusters first,
   since their filename strings give the strongest anchors.

