#!/usr/bin/env python3
"""Extract per-object symbol tables from Xtensa .a libraries (stdlib only).

Parses the ar archive and each ELF32 relocatable object: section
headers, .symtab with linked string table. Emits JSON with, per
object, every FUNC/OBJECT symbol: name, section, value, size, bind.

Usage: lib_symbols.py <lib.a> [<lib.a>...] <out.json>
"""
import json
import struct
import sys
from pathlib import Path


def parse_ar(data):
    assert data[:8] == b"!<arch>\n"
    members = []
    pos = 8
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        name = hdr[0:16].decode("ascii", "replace").strip()
        size = int(hdr[48:58].decode("ascii", "replace").strip())
        body = data[pos + 60:pos + 60 + size]
        members.append((name, body))
        pos += 60 + size + (size & 1)
    longnames = b""
    for name, body in members:
        if name == "//":
            longnames = body
    for name, body in members:
        if name.startswith("/") and name[1:].isdigit() and longnames:
            off = int(name[1:])
            end = longnames.find(b"/\n", off)
            name = longnames[off:end].decode("ascii", "replace")
        yield name.rstrip("/"), body


def parse_elf_obj(data):
    if data[:4] != b"\x7fELF" or data[4] != 1:
        return None
    (shoff,) = struct.unpack_from("<I", data, 32)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 46)
    sections = []
    for i in range(shnum):
        off = shoff + i * shentsize
        vals = struct.unpack_from("<IIIIIIIIII", data, off)
        sections.append({"name_off": vals[0], "type": vals[1],
                         "addr": vals[3], "offset": vals[4],
                         "size": vals[5], "link": vals[6],
                         "entsize": vals[9]})
    if shstrndx < len(sections):
        st = sections[shstrndx]
        strtab = data[st["offset"]:st["offset"] + st["size"]]
        for s in sections:
            end = strtab.find(b"\x00", s["name_off"])
            s["name"] = strtab[s["name_off"]:end].decode("ascii", "replace")
    symbols = []
    for s in sections:
        if s["type"] != 2:  # SHT_SYMTAB
            continue
        strs = sections[s["link"]]
        strdata = data[strs["offset"]:strs["offset"] + strs["size"]]
        count = s["size"] // s["entsize"] if s["entsize"] else 0
        for i in range(count):
            off = s["offset"] + i * s["entsize"]
            name_off, value, size, info, _other, ndx = struct.unpack_from(
                "<IIIBBH", data, off)
            end = strdata.find(b"\x00", name_off)
            name = strdata[name_off:end].decode("ascii", "replace")
            typ = info & 0xF
            if typ not in (1, 2) or not name:  # OBJECT or FUNC
                continue
            secname = sections[ndx]["name"] if ndx < len(sections) else "?"
            symbols.append({"name": name, "section": secname,
                            "value": value, "size": size,
                            "type": "FUNC" if typ == 2 else "OBJECT",
                            "bind": "GLOBAL" if info >> 4 == 1 else "LOCAL"})
    return symbols


def main():
    libs = sys.argv[1:-1]
    out = {}
    for libpath in libs:
        data = Path(libpath).read_bytes()
        objects = {}
        for name, body in parse_ar(data):
            syms = parse_elf_obj(body)
            if syms:
                objects[name] = syms
        out[Path(libpath).name] = objects
        total = sum(len(v) for v in objects.values())
        print(f"{libpath}: {len(objects)} objects, {total} FUNC/OBJECT symbols")
    Path(sys.argv[-1]).write_text(json.dumps(out, indent=1) + "\n")


if __name__ == "__main__":
    main()
