#!/usr/bin/env python3
"""Exercise the shipped clang, llvm-ar and llvm-ranlib in the guest.

    python3 util/arm64/test-tools.py --smp 1 --mem 3072
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
    ap.add_argument("--mem", type=int, default=3072)
    ap.add_argument("--stage", default="/tmp/opencode/toolstest")
    ap.add_argument("--dump", default="/tmp/opencode/toolstest.log")
    ap.add_argument("--sdk-archive",
                    default="/home/scott/tftp/aspera-tblgen-sdk.boot")
    args = ap.parse_args()

    stage = Path(args.stage)
    shutil.rmtree(stage, ignore_errors=True)
    bindir = stage / "bin"
    tdir = stage / "t"
    bindir.mkdir(parents=True)
    tdir.mkdir()
    subprocess.run([str(HERE / "aarch64-clang"),
                    str(HERE / "testdata" / "artest.c"),
                    "-o", str(bindir / "artest")], check=True)
    shutil.copy2(HERE / "testdata" / "x.c", tdir / "x.c")
    runner = stage / "runner.elf"
    subprocess.run([sys.executable, str(HERE / "mkrun.py"),
                    str(ROOT / "build" / "arm64-apex" / "hello.elf"),
                    "--in-tree", "/initramfs/bin/artest",
                    "--untar", "/boot/sdk.tar:/initramfs",
                    "--tree", f"{bindir}:/initramfs/bin",
                    "--tree", f"{tdir}:/initramfs/t",
                    "--suffix", "", "--output", str(runner)], check=True)
    dump = Path(args.dump)
    subprocess.run([sys.executable, str(HERE / "test_apex_boot.py"),
                    "--elf", str(runner), "--archive", args.sdk_archive,
                    "--marker", "ART-DONE", "--timeout", "120",
                    "--deadline", "600", "--mem", str(args.mem),
                    "--smp", str(args.smp), "--dump", str(dump)], check=True)
    text = dump.read_text(errors="replace")
    checks = ["ART cc OK", "ART ar-create OK", "ART ar-list OK",
              "ART ranlib OK", "ART-DONE"]
    missing = [c for c in checks if c not in text]
    for line in text.splitlines():
        if line.startswith("ART"):
            print(line)
    if missing:
        sys.exit("tool test FAIL; missing " + ", ".join(missing))
    print("toolchain tools PASS")


if __name__ == "__main__":
    main()
