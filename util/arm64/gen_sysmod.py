#!/usr/bin/env python3
"""Regenerate libinterp/sysmod.h with this host's frame sizes and maps.

limbo writes that table (`limbo -t Sys module/runt.m`) using the sizes of
the host it runs on; the checked-in copy came from a 32-bit limbo, so on
a 64-bit build every frame is too small and newmp walks past it.

The F_* structs in runt.h are compiled by our own compiler, so their
sizes and pointer members can be read back from the debug info.  The
table format is Inferno's: byte size, and one map byte per eight
pointer-sized slots, high bit first.
"""
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PURG = Path(os.environ.get("PURGATORIO", ROOT.parent / "purgatorio"))
OBJ = ROOT / "build/arm64-emu/sysmod_probe.o"
OUT = PURG / "libinterp/sysmod.h"

TAG = re.compile(r"^0x([0-9a-f]+):(\s+)DW_TAG_(\w+)\s*$")
ATTR = re.compile(r"^\s{2,}DW_AT_(\w+)\s+(.*)$")
QUOTED = re.compile(r'"([^"]*)"')


def parse(structs: dict[str, dict]) -> None:
    text = subprocess.run(["llvm-dwarfdump", "--debug-info", str(OBJ)],
                          capture_output=True, text=True, check=True).stdout
    stack: list[dict] = []
    for line in text.splitlines():
        m = TAG.match(line)
        if m:
            depth = len(m.group(2)) // 2
            while stack and stack[-1]["depth"] >= depth:
                done = stack.pop()
                if done["tag"] == "member" and stack:
                    parent = next((s for s in reversed(stack)
                                   if s["tag"] == "structure_type"), None)
                    if parent is not None:
                        parent["members"].append(done)
                elif done["tag"] == "structure_type" and done.get("name"):
                    if done.get("size") is not None:
                        structs[done["name"]] = done
            stack.append({"depth": depth, "tag": m.group(3),
                          "name": None, "size": None, "type": None,
                          "offset": None, "members": []})
            continue
        a = ATTR.match(line)
        if not a or not stack:
            continue
        key, val = a.group(1), a.group(2)
        top = stack[-1]
        if key == "name":
            q = QUOTED.search(val)
            top["name"] = q.group(1) if q else val.strip()
        elif key == "byte_size":
            top["size"] = int(val.strip().strip("()"), 0)
        elif key == "type":
            q = QUOTED.search(val)
            top["type"] = q.group(1) if q else val.strip()
        elif key == "data_member_location":
            top["offset"] = int(val.strip().strip("()"), 0)
    while stack:
        done = stack.pop()
        if done["tag"] == "structure_type" and done.get("name") \
                and done.get("size") is not None:
            structs[done["name"]] = done


def emit(structs: dict[str, dict], entries) -> str:
    out = ["typedef struct{char *name; long sig; void (*fn)(void*); "
           "int size; int np; uchar map[16];} Runtab;",
           "Runtab Sysmodtab[]={"]
    for fname, sig, cname in entries:
        st = structs.get(f"F_{cname}")
        if st is None:
            raise SystemExit(f"no debug info for F_{cname}")
        size = st["size"]
        slots = (size + 7) // 8
        nbytes = max((slots + 7) // 8, 1)
        bits = [0] * nbytes
        for mem in st["members"]:
            if mem["offset"] is None or "*" not in (mem["type"] or ""):
                continue
            slot = mem["offset"] // 8
            if slot // 8 < nbytes:
                bits[slot // 8] |= 0x80 >> (slot % 8)
        maps = ", ".join(f"0x{b:02x}" for b in bits)
        out.append(f'\t"{fname}",{sig},{cname},{size},{nbytes},{{{maps}}},')
    out.append("};")
    out.append(f"#define Sysmodlen\t{len(entries)}")
    return "\n".join(out) + "\n"


def build_probe(entries) -> None:
    """Force debug info for every F_* struct, including unused ones."""
    import sys as _sys
    _sys.path.insert(0, str(ROOT / "util/arm64"))
    import build_emu

    probe = ROOT / "build/arm64-emu/sysmod_probe.c"
    lines = ['#include "dat.h"', '#include "fns.h"', '#include "error.h"',
             '#include "isa.h"\n#include "interp.h"', '#include "runt.h"']
    for i, (_n, _s, c) in enumerate(entries):
        # a variable of the type forces the full definition into DWARF
        lines.append(f"F_{c} probe_{i};")
    probe.write_text("\n".join(lines) + "\n")
    obj = ROOT / "build/arm64-emu/sysmod_probe.o"
    subprocess.run([build_emu.CC, *build_emu.cflags(), "-c", str(probe),
                    "-o", str(obj)], check=True)


def main() -> None:
    src = OUT.read_text()
    entries = [(m.group(1), m.group(2), m.group(3))
               for m in re.finditer(
                   r'\t"([^"]+)",(0x[0-9a-f]+),(\w+),\d+,\d+,\{[^}]*\},', src)]
    if not entries:
        raise SystemExit("no entries parsed from sysmod.h")
    build_probe(entries)
    structs: dict[str, dict] = {}
    parse(structs)
    OUT.write_text(emit(structs, entries))
    print(f"{OUT}: {len(entries)} entries")


if __name__ == "__main__":
    main()
