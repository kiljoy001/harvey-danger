#!/usr/bin/env python3
"""Build a bootable SD-card image for the Orange Pi 3B.

The image holds an MBR, one FAT32 partition and the three files U-Boot
needs: the kernel Image, the board DTB and the SDK boot archive.  Write it
to the card from U-Boot with `mmc write` (split into 80 MiB chunks for
TFTP), or with dd from a workstation:

    python3 util/arm64/mk-sdimg.py \
        --image build/arm64-kernel/harvey.img \
        --dtb /home/scott/tftp/orangepi-3b-v1.1.dtb \
        --sdk /tmp/opencode/sdk-board.boot \
        --out /tmp/opencode/sd.img

It prints the U-Boot commands for both the write and the boot.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys

SECTOR = 512
PART_START = 2048          # 1 MiB alignment
CHUNK = 83886080           # 80 MiB, the U-Boot TFTP part size


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", default="build/arm64-kernel/harvey.img")
    ap.add_argument("--dtb", default="/home/scott/tftp/orangepi-3b-v1.1.dtb")
    ap.add_argument("--sdk", default="/tmp/opencode/sdk-board.boot")
    ap.add_argument("--out", default="/tmp/opencode/sd.img")
    ap.add_argument("--size-mb", type=int, default=0,
                    help="image size (default: files + 10%)")
    ap.add_argument("--parts-dir", default="",
                    help="also split the image into 80 MiB TFTP parts here")
    args = ap.parse_args()

    for name, path in (("image", args.image), ("dtb", args.dtb),
                       ("sdk", args.sdk)):
        if not Path(path).is_file():
            sys.exit(f"missing {name}: {path}")
    for tool in ("sfdisk", "mkfs.vfat", "mcopy"):
        if shutil.which(tool) is None:
            sys.exit(f"missing {tool}")

    image = Path(args.image)
    dtb = Path(args.dtb)
    sdk = Path(args.sdk)
    need = image.stat().st_size + dtb.stat().st_size + sdk.stat().st_size
    size = (args.size_mb * 1024 * 1024) if args.size_mb else \
        need + need // 10
    size = (size + 1024 * 1024 - 1) // (1024 * 1024) * 1024 * 1024
    out = Path(args.out)
    out.unlink(missing_ok=True)
    with open(out, "wb") as f:
        f.truncate(size)

    sectors = size // SECTOR
    part_sectors = sectors - PART_START
    subprocess.run(["sfdisk", "-q", "--no-reread", str(out)], check=True,
                   input=f"label: dos\nstart={PART_START}, "
                         f"size={part_sectors}, type=0c\n".encode())
    subprocess.run(["mkfs.vfat", "-F", "32", "-n", "HARVEY",
                    "--offset", str(PART_START), str(out)], check=True,
                   stdout=subprocess.DEVNULL)

    offset = PART_START * SECTOR
    for path, name in ((image, "Image"), (dtb, "board.dtb"),
                       (sdk, "sdk.tar")):
        subprocess.run(["mcopy", "-i", f"{out}@@{offset}", str(path),
                        f"::{name}"], check=True)

    # Read each file back so a silent failure cannot pass.
    for name in ("Image", "board.dtb", "sdk.tar"):
        if subprocess.run(["mtype", "-i", f"{out}@@{offset}", f"::{name}"],
                          stdout=subprocess.DEVNULL).returncode != 0:
            sys.exit(f"{name} missing from the image")

    blocks = size // SECTOR
    print(f"sd image: {out} ({size} bytes, {blocks} blocks)")
    print(f"  partition 1 at sector {PART_START}, {part_sectors} sectors")
    print()
    print("U-Boot write (after loading the parts at 0x50000000 + i*0x05000000):")
    print(f"  mmc dev 1")
    print(f"  mmc write 0x50000000 0 0x{blocks:X}")
    print()
    print("U-Boot boot:")
    print("  fatload mmc 1:1 0x40080000 Image")
    print("  fatload mmc 1:1 0x48000000 board.dtb")
    print("  fatload mmc 1:1 0x50000000 sdk.tar")
    print("  booti 0x40080000 - 0x48000000")
    print()
    print("  setenv bootcmd 'fatload mmc 1:1 0x40080000 Image; "
          "fatload mmc 1:1 0x48000000 board.dtb; "
          "fatload mmc 1:1 0x50000000 sdk.tar; "
          "booti 0x40080000 - 0x48000000'; saveenv")

    if args.parts_dir:
        pdir = Path(args.parts_dir)
        shutil.rmtree(pdir, ignore_errors=True)
        pdir.mkdir(parents=True)
        n = 0
        with open(out, "rb") as f:
            while True:
                chunk = f.read(CHUNK)
                if not chunk:
                    break
                (pdir / f"sd.part.{n:02d}").write_bytes(chunk)
                n += 1
        print(f"\nsplit into {n} parts under {pdir}")


if __name__ == "__main__":
    main()
