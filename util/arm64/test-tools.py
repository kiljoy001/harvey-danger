#!/usr/bin/env python3
"""Exercise the shipped compiler, LLVM utilities and clang-tools in the guest.

    python3 util/arm64/test-tools.py --smp 2 --mem 3072
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

CHECKS = [
    "TOOLTEST cc OK", "TOOLTEST ar-create OK", "TOOLTEST ar-list OK",
    "TOOLTEST ranlib OK", "TOOLTEST nm OK", "TOOLTEST objcopy OK",
    "TOOLTEST strip OK", "TOOLTEST readelf OK", "TOOLTEST objdump OK",
    "TOOLTEST size OK", "TOOLTEST strings OK", "TOOLTEST cxxfilt OK",
    "TOOLTEST format OK", "TOOLTEST llvm-help OK", "TOOLTEST clang-tidy OK",
    "TOOLTEST clang-check OK", "TOOLTEST-DONE",
]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--mem", type=int, default=3072)
    ap.add_argument("--stage", default="/tmp/opencode/toolstest")
    ap.add_argument("--dump", default="/tmp/opencode/toolstest.log")
    ap.add_argument("--sdk-archive",
                    default="/home/scott/tftp/aspera-tblgen-sdk.boot")
    ap.add_argument("--timeout", default="900")
    args = ap.parse_args()

    stage = Path(args.stage)
    shutil.rmtree(stage, ignore_errors=True)
    bindir = stage / "bin"
    tdir = stage / "t"
    bindir.mkdir(parents=True)
    tdir.mkdir()
    subprocess.run([str(HERE / "aarch64-clang"),
                    str(HERE / "testdata" / "tooltest.c"),
                    "-o", str(bindir / "tooltest")], check=True)
    shutil.copy2(HERE / "testdata" / "x.c", tdir / "x.c")
    shutil.copy2(HERE / "testdata" / "fmtme.c", tdir / "fmtme.c")
    runner = stage / "runner.elf"
    subprocess.run([sys.executable, str(HERE / "mkrun.py"),
                    str(ROOT / "build" / "arm64-apex" / "hello.elf"),
                    "--in-tree", "/initramfs/bin/tooltest",
                    "--untar", "/boot/sdk.tar:/initramfs",
                    "--tree", f"{bindir}:/initramfs/bin",
                    "--tree", f"{tdir}:/initramfs/t",
                    "--suffix", "", "--output", str(runner)], check=True)
    dump = Path(args.dump)
    subprocess.run([sys.executable, str(HERE / "test_apex_boot.py"),
                    "--elf", str(runner), "--archive", args.sdk_archive,
                    "--marker", "TOOLTEST-DONE", "--timeout", args.timeout,
                    "--deadline", "2400", "--mem", str(args.mem),
                    "--smp", str(args.smp), "--dump", str(dump)], check=True)
    text = dump.read_text(errors="replace")
    missing = [c for c in CHECKS if c not in text]
    for line in text.splitlines():
        if line.startswith("TOOLTEST"):
            print(line)
    if missing:
        sys.exit("tool test FAIL; missing " + ", ".join(missing))
    print("toolchain tools PASS")


if __name__ == "__main__":
    main()
