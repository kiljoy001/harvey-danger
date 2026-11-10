#!/usr/bin/env python3
"""M0: reserve the AArch64 ASan shadow with the existing Harvey kernel.

Builds util/arm64/m0probe.c, ships it in a tiny boot archive, and checks
that a 64 GiB anonymous fixed mapping at 0x1000000000 can be touched,
protected, unmapped and remapped while normal mmap allocations stay out
of the shadow range.

    python3 util/arm64/test-asan-m0.py
    python3 util/arm64/test-asan-m0.py --board-image /home/scott/tftp/m0.img
"""
from pathlib import Path
import argparse
import shutil
import struct
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mem", type=int, default=4096)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--stage", default="/tmp/opencode/asan-m0")
    ap.add_argument("--dump", default="/tmp/opencode/asan-m0.log")
    ap.add_argument("--deadline", type=int, default=300)
    ap.add_argument("--board-image", default="",
                    help="write a board Image and stop instead of running QEMU")
    args = ap.parse_args()

    stage = Path(args.stage)
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True)
    binary = stage / "m0probe"
    subprocess.run([str(HERE / "aarch64-clang"), str(HERE / "m0probe.c"),
                    "-o", str(binary)], check=True)
    strip = shutil.which("llvm-strip")
    if strip:
        subprocess.run([strip, "--strip-all", str(binary)], check=True)

    inner = stage / "test.tar"
    with tarfile.open(inner, "w", format=tarfile.USTAR_FORMAT) as tar:
        tar.add(str(binary), arcname="m0probe")
    data = inner.read_bytes()
    boot = stage / "m0.boot"
    with boot.open("wb") as f:
        f.write(struct.pack("<8sIIII", b"O9BOOT01", 1, 1, 24 + 48 + len(data), 0))
        f.write(struct.pack("<32sIIII", b"test.tar", 24 + 48, len(data), 0o555, 0))
        f.write(data)

    runner = stage / "runner.elf"
    subprocess.run([
        sys.executable, str(HERE / "mkrun.py"), str(binary),
        "--in-tree", "/initramfs/m0probe",
        "--untar", "/boot/test.tar:/initramfs",
        "--suffix", "", "--output", str(runner),
    ], check=True)

    if args.board_image:
        subprocess.run([
            sys.executable, str(HERE / "test_apex_boot.py"),
            "--elf", str(runner), "--image-out", args.board_image,
        ], check=True)
        return

    dump = Path(args.dump)
    subprocess.run([
        sys.executable, str(HERE / "test_apex_boot.py"),
        "--elf", str(runner), "--archive", str(boot),
        "--mem", str(args.mem), "--smp", str(args.smp),
        "--marker", "M0-DONE", "--deadline", str(args.deadline),
        "--dump", str(dump),
    ], check=True)
    text = dump.read_text(errors="replace")
    for line in text.splitlines():
        if line.startswith("M0"):
            print(line)
    needed = ("M0-MMAP OK", "M0-TOUCH OK", "M0-MPROTECT-NONE OK",
              "M0-MPROTECT-RW OK", "M0-NORMAL OK", "M0-MUNMAP OK",
              "M0-REMAP OK", "M0-DONE")
    missing = [m for m in needed if m not in text]
    if missing:
        sys.exit("M0 failed: " + ", ".join(missing))


if __name__ == "__main__":
    main()
