#!/usr/bin/env python3
"""Join B2 external ROM call targets with the SDK 2.0.0 ROM address map.

Source of truth for names: ld/eagle.rom.addr.v6.ld in the pinned
NONOS SDK 2.0.0 (PROVIDE lines). Targets not in the map stay unnamed
with caller statistics, ready for manual identification.

Usage: rom_map.py <eagle.rom.addr.v6.ld> <external-calls.csv> <out.csv> <out.md>
"""
import csv
import re
import sys
from pathlib import Path

rom = {}
for line in Path(sys.argv[1]).read_text().splitlines():
    m = re.match(r"PROVIDE\s*\(\s*(\S+)\s*=\s*(0x[0-9a-fA-F]+)\s*\)", line)
    if m:
        rom[int(m.group(2), 16)] = m.group(1)

rows = list(csv.DictReader(open(sys.argv[2])))
named, unnamed = [], []
for r in rows:
    addr = int(r["target"], 16)
    r["name"] = rom.get(addr, "")
    (named if r["name"] else unnamed).append(r)

with open(sys.argv[3], "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["target", "name", "ref_count", "caller_count", "sample_callers"])
    for r in sorted(rows, key=lambda r: -int(r["ref_count"])):
        w.writerow([r["target"], r["name"], r["ref_count"],
                    r["caller_count"], r["sample_callers"]])

lines = [f"ROM targets: {len(rows)} total, {len(named)} named by SDK ROM map, "
         f"{len(unnamed)} unnamed", "",
         "| target | SDK name | refs | callers |", "| --- | --- | --- | --- |"]
for r in sorted(named, key=lambda r: -int(r["ref_count"])):
    lines.append(f"| 0x{r['target']} | {r['name']} | {r['ref_count']} | {r['caller_count']} |")
lines += ["", "Unnamed targets (no PROVIDE in SDK 2.0.0 ROM map):", ""]
for r in sorted(unnamed, key=lambda r: -int(r["ref_count"])):
    lines.append(f"- 0x{r['target']}: {r['ref_count']} refs / {r['caller_count']} callers")
Path(sys.argv[4]).write_text("\n".join(lines) + "\n")
print("\n".join(lines[:45]))
print(f"... ({len(named)} named, {len(unnamed)} unnamed)")
