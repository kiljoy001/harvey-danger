#!/usr/bin/env python3
"""Pack a Harvey arm64 kernel into an Android boot image for borneo.

The boot image header and device tree are taken from a template image
(the stock-derived harvey-boot.img already in the firmware directory);
only the kernel is replaced.

The DMA arena the kernel carves out of RAM is sized here, from the LKL
instances the image will run: for each family, its "mem" requirement
plus the 16 MiB headroom lklboot adds, summed, plus slack for native
drivers, rounded up.  The total is stamped into the DTB as
/chosen/harvey,lkl-arena-size so the kernel reservation grows with the
family table instead of being a board constant.

Usage:
    pack_boot.py --template harvey-boot.img --kernel harvey.img \
        --output harvey-boot.img [--families bengal_ctl,bengal_usb]
"""

import argparse
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAGE = 4096
HEADROOM = 16 * 1024 * 1024   # added per instance by lklboot (raminit)
NATIVE_SLACK = 64 * 1024 * 1024
ROUND = 16 * 1024 * 1024


def mem_bytes(text: str) -> int:
    text = text.strip().upper()
    mult = {"K": 1024, "M": 1024 ** 2, "G": 1024 ** 3}
    if text[-1] in mult:
        return int(text[:-1]) * mult[text[-1]]
    return int(text)


def arena_size(families_json: Path, names: list[str]) -> int:
    fams = json.loads(families_json.read_text())
    total = NATIVE_SLACK
    for name in names:
        if name not in fams:
            sys.exit(f"pack_boot: no such family: {name}")
        total += mem_bytes(fams[name]["mem"]) + HEADROOM
    return (total + ROUND - 1) // ROUND * ROUND


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--template", type=Path, required=True,
                    help="existing boot image providing header + DTB")
    ap.add_argument("--kernel", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--families", default="bengal_ctl,bengal_usb",
                    help="comma-separated LKL families the image runs")
    ap.add_argument("--families-json", type=Path,
                    default=ROOT / "util/arm64/lkl-families.json")
    args = ap.parse_args()

    arena = arena_size(args.families_json,
                       [s.strip() for s in args.families.split(",") if s.strip()])

    old = args.template.read_bytes()
    kern = args.kernel.read_bytes()
    ks_old = struct.unpack("<I", old[8:12])[0]
    off = PAGE + ((ks_old + PAGE - 1) // PAGE) * PAGE
    ds = struct.unpack("<I", old[1648:1652])[0]
    dtb = bytearray(old[off:off + ds])

    with tempfile.NamedTemporaryFile(suffix=".dtb", delete=False) as tf:
        tf.write(dtb)
        dtbpath = tf.name
    hi, lo = (arena >> 32) & 0xFFFFFFFF, arena & 0xFFFFFFFF
    subprocess.run(["fdtput", "-t", "u", dtbpath, "/chosen",
                    "harvey,lkl-arena-size", str(hi), str(lo)], check=True)
    dtb = Path(dtbpath).read_bytes()
    Path(dtbpath).unlink()
    ds = len(dtb)

    def le(n: int) -> bytes:
        return struct.pack("<I", n)

    idv = hashlib.sha1(
        kern + le(len(kern)) + le(0) + le(0) + le(0) + dtb + le(ds)
    ).digest()
    hdr = bytearray(old[:PAGE])
    struct.pack_into("<I", hdr, 8, len(kern))
    struct.pack_into("<I", hdr, 1648, ds)
    hdr[576:608] = idv + b"\0" * 12

    def pad(b: bytes) -> bytes:
        return b + b"\0" * ((PAGE - len(b) % PAGE) % PAGE)

    args.output.write_bytes(bytes(hdr) + pad(kern) + pad(bytes(dtb)))
    print(f"wrote {args.output} kernel {len(kern)} dtb {ds} "
          f"lkl-arena {arena} ({arena // (1024 * 1024)} MiB)")


if __name__ == "__main__":
    main()
