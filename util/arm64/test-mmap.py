#!/usr/bin/env python3
"""Exercise kernel-level POSIX memory mapping in QEMU.

The probe checks that mmap honours prot, that mprotect changes the
protection of an existing mapping, that PROT_NONE faults, that munmap
removes a mapping, and that an RFMEM thread sees a mapping made after it
was created.

    python3 util/arm64/test-mmap.py
"""
from pathlib import Path
import argparse
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SOURCE = HERE / "testdata" / "mmapprobe.c"

EXPECT = [
    "MMAP-ANON 1",
    "MMAP-RW 1",
    "MPROT-READ-CALL 1",
    "MPROT-READ-BYTE 1",
    "MPROT-NONE-CALL 1",
    "MPROT-RW-CALL 1",
    "MPROT-RW-BYTE 1",
    "MMAP-FIXED 1",
    "MMAP-FIXEDBYTE 1",
    "MMAP-THREAD-SAW 1",
    "MMAP-DONE",
]

# A child that touches a protected/unmapped page must not exit normally.
FAULT = ["MPROT-WRITE", "MPROT-NONEREAD", "MUNMAP-READ"]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stage", default="/tmp/opencode/mmap-test")
    ap.add_argument("--mem", type=int, default=2048)
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--deadline", type=int, default=120)
    args = ap.parse_args()

    stage = Path(args.stage)
    stage.mkdir(parents=True, exist_ok=True)
    binary = stage / "mmapprobe"
    subprocess.run([str(HERE / "aarch64-clang"), str(SOURCE),
                    "-o", str(binary)], check=True)
    subprocess.run(["llvm-strip", "--strip-all", str(binary)], check=True)

    runner = stage / "runner.elf"
    subprocess.run([sys.executable, str(HERE / "mkrun.py"), str(binary),
                    "--output", str(runner)], check=True)

    dump = stage / "log"
    subprocess.run([
        sys.executable, str(HERE / "test_apex_boot.py"),
        "--elf", str(runner),
        "--marker", "MMAP-DONE",
        "--mem", str(args.mem), "--smp", str(args.smp),
        "--deadline", str(args.deadline), "--dump", str(dump),
    ], check=True)

    text = dump.read_text(errors="replace")
    lines = text.splitlines()
    missing = [line for line in EXPECT if line not in text]
    if missing:
        for line in lines:
            if line.startswith(("MMAP", "MPROT", "MUNMAP")):
                print("   ", line)
        sys.exit("missing mmap/mprotect results: " + ", ".join(missing))

    for tag in FAULT:
        got = [ln for ln in lines if ln.startswith(tag + " ")]
        if not got:
            sys.exit(f"missing fault check: {tag}")
        # "0 0" would mean the child completed the forbidden access.
        if got[-1].strip() == tag + " 0 0":
            sys.exit(f"protection did not fault: {got[-1]}")

    if "ARM64 exception" in text:
        sys.exit("a CPU took an unhandled exception")
    print("mmap/mprotect PASS")


if __name__ == "__main__":
    main()
