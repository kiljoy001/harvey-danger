#!/usr/bin/env python3
"""Probe the RK3566 SD slot from user space.

Runs the guest `sdcard probe`, which takes over the DesignWare MSHC
controller (mmc@fe2b0000) that U-Boot already initialised, sends CMD13 for
the card status and reads block 0 (the MBR).  The driver does not
re-initialise the card, so at the vendor U-Boot prompt run

    mmc dev 0; mmc info

first (vendor numbering: 0 = SD, 1 = eMMC), then load this board image and
the DTB and `booti`.  Without that step the controller may never have seen
the card.

    python3 util/arm64/test-sdcard.py --board-image out.img
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--mem", type=int, default=1024)
    ap.add_argument("--stage", default="/tmp/opencode/sdcardprobe")
    ap.add_argument("--dump", default="/tmp/opencode/sdcardprobe.log")
    ap.add_argument("--board-image", default="")
    ap.add_argument("--binary", default="",
                    help="guest sdcard binary (default: the userland build)")
    ap.add_argument("--timeout", default="120")
    args = ap.parse_args()

    binary = Path(args.binary) if args.binary else \
        ROOT / "build" / "arm64-kernel" / "userland" / "sdcard"
    if not binary.is_file():
        for cand in (Path("/tmp/opencode/userland/sdcard"),
                     ROOT / "build" / "arm64-kernel" / "userland" / "sdcard"):
            if cand.is_file():
                binary = cand
                break
        else:
            sys.exit(f"missing {binary}; run util/arm64/build_userland.py")

    stage = Path(args.stage)
    shutil.rmtree(stage, ignore_errors=True)
    bindir = stage / "bin"
    bindir.mkdir(parents=True)
    shutil.copy2(binary, bindir / "sdcard")

    runner = stage / "runner.elf"
    subprocess.run([sys.executable, str(HERE / "mkrun.py"),
                    str(bindir / "sdcard"),
                    "--in-tree", "/initramfs/bin/sdcard",
                    "--tree", f"{bindir}:/initramfs/bin",
                    "--suffix", "", "--output", str(runner),
                    "--", "probe"], check=True)

    if args.board_image:
        subprocess.run([sys.executable, str(HERE / "test_apex_boot.py"),
                        "--elf", str(runner),
                        "--image-out", args.board_image], check=True)
        return

    dump = Path(args.dump)
    subprocess.run([sys.executable, str(HERE / "test_apex_boot.py"),
                    "--elf", str(runner), "--marker", "sdcard:",
                    "--timeout", args.timeout, "--deadline", "600",
                    "--mem", str(args.mem), "--smp", str(args.smp),
                    "--dump", str(dump)], check=False)
    text = dump.read_text(errors="replace")
    for line in text.splitlines():
        if line.startswith("sdcard:"):
            print(line)
    if "MBR signature OK" not in text:
        sys.exit("SD probe did not find the MBR")


if __name__ == "__main__":
    main()
