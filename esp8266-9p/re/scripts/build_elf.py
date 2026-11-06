#!/usr/bin/env python3
"""Build a loadable Xtensa ELF from the extracted ESP8266 user1 image.

Ghidra's binary loader would place a raw .bin at one base only; the
app's segments live at far-apart addresses (IROM flash-mapped, IRAM,
DRAM). An ELF32 with PT_LOAD program headers makes the ELF loader map
each segment at its true virtual address in one program.

Inputs come from ../inventory/layout.json (segment addresses/lengths)
and the stock backup (segment bytes). IROM base is the validated
0x40201010 (= 0x40200000 + flash offset 0x1010 of the first IROM byte).

Usage: build_elf.py <backup.bin> <irom.bin> <layout.json> <out.elf>
"""
import json
import struct
import sys
from pathlib import Path

EM_XTENSA = 94
PT_LOAD = 1
PF_X, PF_W, PF_R = 1, 2, 4


def build(backup, irom, layout, out):
    app = layout["images"][1]
    second = app["second_image"]
    second_abs = app["offset"] + app["second_header_offset"] - app["offset"]
    # second_header_offset in layout is relative to the backup file start
    # only via offset math in parse_flash.py; recompute directly:
    second_abs = app["offset"] + (16 + app["irom_length"]) + 8 - 8
    # Simpler and verified in B0: second header sits right after IROM data.
    second_abs = app["offset"] + 8 + 8 + app["irom_length"]
    segs = []  # (name, vaddr, bytes, flags)
    segs.append((".irom0.text", 0x40201010, irom, PF_R | PF_X))
    pos = second_abs + 8
    names = [".iram0.text", ".dram0.data", ".dram0.bss_like"]
    for i, s in enumerate(second["segments"]):
        addr, ln = s["load_addr"], s["length"]
        data = backup[second_abs + s["file_offset"]:
                      second_abs + s["file_offset"] + ln]
        assert len(data) == ln
        flags = PF_R | PF_X if addr == 0x40100000 else PF_R | PF_W
        segs.append((names[i] if i < len(names) else f".seg{i}",
                     addr, data, flags))
    entry = app["entry"]

    shstr = b"\x00"
    sh_offsets = []
    for name, _, _, _ in segs:
        sh_offsets.append(len(shstr))
        shstr += name.encode() + b"\x00"
    shstrtab_off = len(shstr)
    shstr += b".shstrtab\x00"

    n = len(segs)
    ehsize, phentsize = 52, 32
    data_off = ehsize + phentsize * n
    blobs = []
    cur = data_off
    for name, vaddr, data, flags in segs:
        cur = (cur + 0xFFF) & ~0xFFF
        blobs.append((cur, vaddr, data, flags))
        cur += len(data)
    shstr_file_off = cur
    shoff = shstr_file_off + len(shstr)
    shoff = (shoff + 3) & ~3

    out_bytes = bytearray()
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    out_bytes += struct.pack(
        "<16sHHIIIIIHHHHHH", ident, 2, EM_XTENSA, 1, entry,
        ehsize, shoff, 0, ehsize, phentsize, n, 40, n + 2, n + 1)
    for (off, vaddr, data, flags), (_, _, _, _) in zip(blobs, segs):
        out_bytes += struct.pack(
            "<IIIIIIII", PT_LOAD, off, vaddr, vaddr,
            len(data), len(data), flags, 0x1000)
    for (off, vaddr, data, flags), (name, _, _, _) in zip(blobs, segs):
        out_bytes += bytes(off - len(out_bytes))
        out_bytes += data
    assert len(out_bytes) == shstr_file_off
    out_bytes += shstr
    out_bytes += bytes(shoff - len(out_bytes))
    # null section (ELF32 section header is 40 bytes)
    out_bytes += bytes(40)
    for (off, vaddr, data, flags), name_off in zip(blobs, sh_offsets):
        shtype = 1  # SHT_PROGBITS
        # Section flags are SHF_*, not PF_*: ALLOC=2, EXECINSTR=4, WRITE=1.
        shflags = 2 | (4 if flags & PF_X else 0) | (1 if flags & PF_W else 0)
        out_bytes += struct.pack(
            "<IIIIIIIIII", name_off, shtype, shflags, vaddr, off,
            len(data), 0, 0, 0x1000 if flags & PF_X else 4, 0)
    # .shstrtab section
    out_bytes += struct.pack(
        "<IIIIIIIIII", shstrtab_off, 3, 0, 0, shstr_file_off,
        len(shstr), 0, 0, 1, 0)
    Path(out).write_bytes(bytes(out_bytes))
    print(f"wrote {out}: entry={entry:#x} segments:")
    for (off, vaddr, data, flags), (name, _, _, _) in zip(blobs, segs):
        print(f"  {name} vaddr={vaddr:#x} len={len(data)} fileoff={off:#x}")


if __name__ == "__main__":
    backup = Path(sys.argv[1]).read_bytes()
    irom = Path(sys.argv[2]).read_bytes()
    layout = json.loads(Path(sys.argv[3]).read_text())
    build(backup, irom, layout, sys.argv[4])
