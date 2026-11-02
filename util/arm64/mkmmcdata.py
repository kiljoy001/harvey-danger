#!/usr/bin/env python3
"""Create a large ext2 data partition on the eMMC without transferring it.

A fresh ext2 filesystem of 200 GiB has only about 13 MiB of non-zero
metadata, scattered as a few thousand short runs.  TFTP cannot send a
200 GiB sparse file, so this produces:

  mbr.bin        512-byte MBR with the existing system partition plus a
                 second partition covering the requested size
  datameta.bin   a self-describing blob: a table of (blob offset,
                 partition block, block count) runs followed by every
                 non-zero 4 KiB block

Write the MBR from U-Boot and reboot; then build with
ARM64_MMCDATA=<datameta.bin> so the blob lands in the boot archive and
run /boot/provision, which copies each run to the partition through
emmcfs.  A U-Boot script would be smaller, but this U-Boot's `source`
command parses the image and runs nothing.

    python3 util/arm64/mkmmcdata.py --size 200G --outdir /tmp/opencode/mmc
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import zlib
from pathlib import Path

PART1_START = 0x8000          # sectors
PART1_SECTORS = 0x40000       # 128 MiB system partition
PART2_START = PART1_START + PART1_SECTORS
BLOCK = 4096
BLOB_ADDR = 0x20000000
SCRIPT_ADDR = 0x30000000
IH_MAGIC = 0x27051956
IH_TYPE_SCRIPT = 6


def mbr() -> bytes:
    return b""


def make_mbr(part2_sectors: int) -> bytes:
    mbr = bytearray(512)
    for i, (start, count) in enumerate((
            (PART1_START, PART1_SECTORS),
            (PART2_START, part2_sectors))):
        e = bytearray(16)
        e[0] = 0x80 if i == 0 else 0x00
        e[4] = 0x83
        struct.pack_into("<I", e, 8, start)
        struct.pack_into("<I", e, 12, count)
        mbr[446 + i * 16:446 + i * 16 + 16] = e
    mbr[510] = 0x55
    mbr[511] = 0xaa
    return bytes(mbr)


MAGIC = b"O9MMCD01"


def pack(runs: list[tuple[int, int]], img: Path) -> bytes:
    """Header table plus the non-zero blocks, packed back to back."""
    nblk = BLOCK // 512
    table = bytearray()
    blob = bytearray()
    with open(img, "rb") as f:
        for start, count in runs:
            f.seek(start * BLOCK)
            chunk = f.read(count * BLOCK)
            # partition-relative block index: emmcfs exposes /partN
            # starting at zero, not at the card LBA.
            table += struct.pack("<IIII", len(blob), start, count, 0)
            blob += chunk
    head = MAGIC + struct.pack("<II", len(runs), 0) + bytes(table)
    return head + bytes(blob)


def make_script(cmds: list[str]) -> bytes:
    body = "\n".join(cmds).encode() + b"\n\0"
    hdr = struct.pack(">7I4B32s", IH_MAGIC, 0, 0, len(body), 0, 0,
                      zlib.crc32(body) & 0xffffffff, 0, 0,
                      IH_TYPE_SCRIPT, 0, b"mmc data partition")
    hcrc = zlib.crc32(hdr) & 0xffffffff
    hdr = struct.pack(">7I4B32s", IH_MAGIC, hcrc, 0, len(body), 0, 0,
                      zlib.crc32(body) & 0xffffffff, 0, 0,
                      IH_TYPE_SCRIPT, 0, b"mmc data partition")
    return hdr + body


def data_runs(path: Path) -> list[tuple[int, int]]:
    """(first 4 KiB block, block count) for every non-hole region."""
    fd = os.open(path, os.O_RDONLY)
    size = os.fstat(fd).st_size
    runs = []
    off = 0
    try:
        while off < size:
            try:
                d = os.lseek(fd, off, os.SEEK_DATA)
            except OSError:
                break
            h = os.lseek(fd, d, os.SEEK_HOLE)
            start = d // BLOCK
            end = (h + BLOCK - 1) // BLOCK
            runs.append((start, end - start))
            off = h
    finally:
        os.close(fd)
    return runs


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", default="200G",
                    help="data partition size, e.g. 200G")
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--label", default="harvey-data")
    a = ap.parse_args(argv)

    out = Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    img = out / "data.img"

    if img.exists():
        img.unlink()
    subprocess.run(["truncate", "-s", a.size, str(img)], check=True)
    # Large sparse inode density keeps the inode tables out of the way.
    subprocess.run(["mke2fs", "-q", "-t", "ext2", "-b", str(BLOCK),
                    "-I", "128", "-i", "65536", "-T", "largefile4",
                    "-O", "^resize_inode,^dir_index",
                    "-L", a.label, str(img)],
                   check=True, stderr=subprocess.DEVNULL)

    size = img.stat().st_size
    if size % 512:
        raise SystemExit("image size must be a multiple of 512")
    part2_sectors = size // 512

    runs = data_runs(img)
    blob = pack(runs, img)

    (out / "mbr.bin").write_bytes(make_mbr(part2_sectors))
    (out / "datameta.bin").write_bytes(blob)

    print("image      %s (%.1f GiB)" % (img, size / 2**30))
    print("partition2 start 0x%x sectors 0x%x" % (PART2_START, part2_sectors))
    print("runs       %d" % len(runs))
    print("blob       %.2f MiB (table %d bytes)"
          % (len(blob) / 2**20, 16 + 16 * len(runs)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
