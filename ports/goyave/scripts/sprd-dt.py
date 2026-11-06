#!/usr/bin/env python3
"""Unpack a Spreadtrum dt.img (magic 'SPRD') into individual DTBs.

Format (little-endian):
  header: magic[4]='SPRD', version:u32, count:u32, chip:u32
  entry : id:u32, rev:u32, offset:u32, size:u32, chip:u32
"""
import struct, sys, pathlib

def main(src, outdir):
    data = pathlib.Path(src).read_bytes()
    magic, version, count, chip = struct.unpack_from("<4sIII", data, 0)
    if magic != b"SPRD":
        sys.exit(f"{src}: not a SPRD dt.img (magic {magic!r})")
    print(f"SPRD v{version} count={count} chip={chip} (0x{chip:x})")
    out = pathlib.Path(outdir); out.mkdir(parents=True, exist_ok=True)
    for i in range(count):
        off = 16 + i * 20
        eid, rev, offset, size, echip = struct.unpack_from("<IIIII", data, off)
        blob = data[offset:offset+size]
        p = out / f"dtb-{eid}.dtb"
        p.write_bytes(blob)
        print(f"  id={eid} rev=0x{rev:x} off=0x{offset:x} size=0x{size:x} chip={echip} -> {p}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: sprd-dt.py dt.img outdir")
    main(sys.argv[1], sys.argv[2])
