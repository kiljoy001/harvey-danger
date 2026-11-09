#!/usr/bin/env python3
"""Probe or provision the RK3566 eMMC from user space.

Default mode starts /boot/emmcfs, mounts its 9P tree and prints the card
control block and the MBR signature.  With --setup it also provisions the
200 GiB data partition as ext2, starts ext2srv, mounts it at /mnt/data and
writes a test file.

    python3 util/arm64/test-emmc.py --smp 2 --mem 1024      # QEMU (no card)
    python3 util/arm64/test-emmc.py --board-image out.img   # board image
    python3 util/arm64/test-emmc.py --setup \
        --mmcdata /tmp/opencode/mmc/datameta.bin --board-image out.img
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
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--mem", type=int, default=1024)
    ap.add_argument("--stage", default="/tmp/opencode/emmcprobe")
    ap.add_argument("--dump", default="/tmp/opencode/emmcprobe.log")
    ap.add_argument("--board-image", default="",
                    help="write a board boot image instead of running QEMU")
    ap.add_argument("--setup", action="store_true",
                    help="provision and mount the data partition")
    ap.add_argument("--install", action="store_true",
                    help="untar /boot/sdk.tar into /mnt/data/sdk")
    ap.add_argument("--mmcdata", default="/tmp/opencode/mmc/datameta.bin",
                    help="datameta.bin from util/arm64/mkmmcdata.py")
    ap.add_argument("--timeout", default="300")
    args = ap.parse_args()

    stage = Path(args.stage)
    shutil.rmtree(stage, ignore_errors=True)
    bindir = stage / "bin"
    bindir.mkdir(parents=True)
    prog = ("sdkinstall" if args.install else
            "emmcsetup" if args.setup else "emmcprobe")
    subprocess.run([str(HERE / "aarch64-clang"),
                    str(HERE / "testdata" / f"{prog}.c"),
                    "-o", str(bindir / prog)], check=True)
    userland = ROOT / "build" / "arm64-kernel" / "userland"
    names = ["emmcfs"]
    if args.setup:
        names += ["provision", "ext2srv"]
    if args.install:
        names += ["ext2srv"]
    for name in names:
        src = userland / name
        if not src.is_file():
            sys.exit(f"missing {src}; run util/arm64/build_kernel.py")
        shutil.copy2(src, bindir / name)

    cmd = [sys.executable, str(HERE / "mkrun.py"),
           str(bindir / prog),
           "--in-tree", f"/initramfs/bin/{prog}",
           "--tree", f"{bindir}:/initramfs/bin",
           "--server", "/initramfs/bin/emmcfs",
           "--wait", "/srv/emmc"]
    if args.install:
        # Keep the boot filesystem as the root namespace so /boot/sdk.tar
        # stays readable; /initramfs is the writable ramfs.
        cmd += ["--untar=-:/initramfs"]
    if args.setup:
        data = Path(args.mmcdata)
        if not data.is_file():
            sys.exit(f"missing {data}; run util/arm64/mkmmcdata.py")
        ddir = stage / "data"
        ddir.mkdir()
        shutil.copy2(data, ddir / "datameta.bin")
        cmd += ["--tree", f"{ddir}:/initramfs"]
    runner = stage / "runner.elf"
    cmd += ["--suffix", "", "--output", str(runner)]
    subprocess.run(cmd, check=True)

    if args.board_image:
        subprocess.run([sys.executable, str(HERE / "test_apex_boot.py"),
                        "--elf", str(runner),
                        "--image-out", args.board_image], check=True)
        return

    dump = Path(args.dump)
    marker = ("SDKINSTALL-DONE" if args.install else
              "EMMCSETUP-DONE" if args.setup else "EMMCPROBE-DONE")
    subprocess.run([sys.executable, str(HERE / "test_apex_boot.py"),
                    "--elf", str(runner), "--marker", marker,
                    "--timeout", args.timeout, "--deadline", "900",
                    "--mem", str(args.mem), "--smp", str(args.smp),
                    "--dump", str(dump)], check=True)
    text = dump.read_text(errors="replace")
    for line in text.splitlines():
        if line.startswith(("EMMCPROBE", "EMMCSETUP", "SDKINSTALL")):
            print(line)
    if marker not in text:
        sys.exit("eMMC probe did not finish")
    print("eMMC probe PASS")


if __name__ == "__main__":
    main()
