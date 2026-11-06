#!/usr/bin/env python3
"""Extract printable strings with flash file offsets from an image.

Strings are RE entry points only: a filename string proves the string
is present, not that the named function behaves any particular way.
Usage: extract_strings.py <image.bin> <flash_base_offset> <out.txt>
"""
import re
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
base = int(sys.argv[2], 0)
out = Path(sys.argv[3])
lines = []
for m in re.finditer(rb"[ -~]{5,}", data):
    s = m.group().decode("ascii", errors="replace")
    lines.append(f"{base + m.start():#08x}\t{s}")
out.write_text("\n".join(lines) + "\n")
print(f"{len(lines)} strings -> {out}")
