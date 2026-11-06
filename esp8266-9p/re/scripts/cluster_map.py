#!/usr/bin/env python3
"""Map ieee80211/libphy/libpp clusters from B1Dump output.

Anchor = filename string xref. Functions referencing the same source
filename string are attributed to that compilation unit. Cluster span
is estimated from the sorted anchor functions of all units: a unit's
region runs from its first anchor function to the next unit's first
anchor function (link order follows source order in this image, as
the anchor addresses are monotonically clustered).

Usage: cluster_map.py <functions.csv> <anchors.csv> <out.md>
"""
import csv
import sys
from pathlib import Path

funcs = {}
with open(sys.argv[1]) as f:
    for row in csv.DictReader(f):
        funcs[int(row["entry"], 16)] = {
            "size": int(row["size"]),
            "called": int(row["called_count"]),
            "calling": int(row["calling_count"]),
        }

units = {}
with open(sys.argv[2]) as f:
    for row in csv.DictReader(f):
        refs = [x for x in row["referring_functions"].split(";")
                if x and x != "?"]
        if not refs:
            continue
        units[row["string"]] = sorted({int(x, 16) for x in refs})

order = sorted(units.items(), key=lambda kv: min(kv[1]))
irom_funcs = sorted(a for a in funcs if 0x40201010 <= a < 0x4025EA40)
iram_funcs = sorted(a for a in funcs if 0x40100000 <= a < 0x40107B54)

lines = []
lines.append(f"total functions: {len(funcs)} "
             f"(irom {len(irom_funcs)}, iram {len(iram_funcs)}, "
             f"other {len(funcs) - len(irom_funcs) - len(iram_funcs)})")
tot_bytes = sum(f["size"] for f in funcs.values())
lines.append(f"total function bytes: {tot_bytes}")
lines.append("")
lines.append("| unit (string anchor) | anchor funcs | first | last | est. span bytes |")
lines.append("| --- | --- | --- | --- | --- |")
for i, (name, addrs) in enumerate(order):
    first = min(addrs)
    nxt = min(order[i + 1][1]) if i + 1 < len(order) else 0x4025EA40
    span_funcs = [a for a in irom_funcs if first <= a < nxt]
    span = sum(funcs[a]["size"] for a in span_funcs)
    lines.append(f"| {name} | {len(addrs)} | {first:#x} | {max(addrs):#x} "
                 f"| {span} ({len(span_funcs)} funcs) |")
    lines.append(f"  anchors: {' '.join(hex(a) for a in addrs)}")
out = Path(sys.argv[3])
out.write_text("\n".join(lines) + "\n")
print("\n".join(lines))
