#!/usr/bin/env python3
"""Fingerprint-match SDK library functions against the linked image.

For each GLOBAL FUNC symbol in a chosen object, sample 24-byte windows
from the object's code section and search them in the extracted image
segment. Windows that hit vote for one alignment; a function is a
match when several windows agree on the same image address. This is
byte evidence, independent of Ghidra's function boundaries.

Usage: fingerprint.py <sdk-lib-dir> <backup.bin> <irom.bin> <out.csv>
       [lib.a:object.o ...]  (default: the Wi-Fi objects)
"""
import csv
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lib_symbols import parse_ar, parse_elf_obj  # noqa: E402

DEFAULT = [
    "libnet80211.a:ieee80211_hostap.o", "libnet80211.a:ieee80211_phy.o",
    "libnet80211.a:ieee80211.o", "libnet80211.a:ieee80211_input.o",
    "libnet80211.a:ieee80211_output.o", "libnet80211.a:ieee80211_scan.o",
    "libnet80211.a:ieee80211_sta.o", "libpp.a:wdev.o",
    "libpp.a:esf_buf.o", "libphy.a:phy.o",
]


def elf_sections(data):
    """Return {section_name: bytes} for an ELF32 object."""
    (shoff,) = struct.unpack_from("<I", data, 32)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 46)
    secs = []
    for i in range(shnum):
        v = struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
        secs.append(v)
    st = secs[shstrndx]
    strtab = data[st[4]:st[4] + st[5]]
    out = {}
    for v in secs:
        end = strtab.find(b"\x00", v[0])
        name = strtab[v[0]:end].decode("ascii", "replace")
        out[name] = data[v[4]:v[4] + v[5]]
    return out


def segments(backup, irom):
    # IRAM segment: second-header segment 0 of the V2 app (see B0/B1).
    second_abs = 0x1000 + 8 + 8 + len(irom)
    addr, ln = struct.unpack_from("<II", backup, second_abs + 8)
    iram = backup[second_abs + 16:second_abs + 16 + ln]
    return {".irom0.text": (0x40201010, irom), ".text": (addr, iram)}


def main():
    libdir = Path(sys.argv[1])
    backup = Path(sys.argv[2]).read_bytes()
    irom = Path(sys.argv[3]).read_bytes()
    segs = segments(backup, irom)
    wanted = sys.argv[5:] or DEFAULT
    rows = []
    for spec in wanted:
        libname, objname = spec.split(":")
        objs = dict(parse_ar((libdir / libname).read_bytes()))
        body = objs[objname]
        syms = parse_elf_obj(body)
        secbytes = elf_sections(body)
        for s in syms:
            if s["type"] != "FUNC" or s["bind"] != "GLOBAL":
                continue
            sec = s["section"]
            if sec not in segs or sec not in secbytes:
                continue
            code = secbytes[sec][s["value"]:s["value"] + s["size"]]
            if len(code) < 24:
                continue
            base, seg = segs[sec]
            votes = {}
            total = 0
            for off in range(8, len(code) - 24, 16):
                win = bytes(code[off:off + 24])
                hit = seg.find(win)
                total += 1
                if hit >= 0:
                    start = hit - off
                    votes[start] = votes.get(start, 0) + 1
            if votes:
                start, n = max(votes.items(), key=lambda kv: kv[1])
                rows.append({
                    "lib": libname, "object": objname, "symbol": s["name"],
                    "sdk_size": s["size"], "image_addr": f"{base + start:#x}",
                    "windows_hit": n, "windows_total": total,
                    "score": round(n / total, 2) if total else 0,
                })
            else:
                rows.append({
                    "lib": libname, "object": objname, "symbol": s["name"],
                    "sdk_size": s["size"], "image_addr": "",
                    "windows_hit": 0, "windows_total": total, "score": 0,
                })
    with open(sys.argv[4], "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    for r in rows:
        print(f"{r['symbol']:36s} {r['image_addr'] or '-':12s} "
              f"size={r['sdk_size']:5d} windows={r['windows_hit']}/{r['windows_total']}")


if __name__ == "__main__":
    main()
