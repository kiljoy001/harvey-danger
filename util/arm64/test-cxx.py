#!/usr/bin/env python3
"""Run the C++ runtime probe on Harvey/ARM64.

Compiles util/arm64/testdata/cxxprobe.cpp with the cross driver, boots it as
the kernel's init under QEMU, and requires every feature line plus
CXXPROBE-DONE.  This is the fast regression test for libc++, the APEX
runtime, static constructors, printf floats, and the SDK headers.

    python3 util/arm64/test-cxx.py --smp 1 --mem 1024
"""
from pathlib import Path
import argparse
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

EXPECT = [
    "containers", "exceptions", "exception_ptr", "iostream", "locale",
    "regex", "regex_replace", "chrono_sleep", "threads", "random",
    "filesystem", "format_charconv", "smart_ptr",
]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--mem", type=int, default=1024)
    ap.add_argument("--stage", default="/tmp/opencode/cxxprobe")
    ap.add_argument("--dump", default="/tmp/opencode/cxxprobe.log")
    args = ap.parse_args()

    stage = Path(args.stage)
    stage.mkdir(parents=True, exist_ok=True)
    elf = stage / "cxxprobe.elf"
    cmd = [str(HERE / "aarch64-clang++"), "-std=c++20",
           str(HERE / "testdata" / "cxxprobe.cpp"), "-o", str(elf)]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)

    cmd = [sys.executable, str(HERE / "test_apex_boot.py"),
           "--elf", str(elf), "--marker", "CXXPROBE-DONE",
           "--timeout", "60", "--mem", str(args.mem),
           "--smp", str(args.smp), "--dump", args.dump]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)

    text = Path(args.dump).read_text(errors="replace")
    lines = [l for l in text.splitlines() if "CXXPROBE" in l]
    print("\n".join(lines))
    missing = []
    for name in EXPECT:
        if not any(f"CXXPROBE {name}" in l and " OK" in l for l in lines):
            missing.append(name)
    if missing or "CXXPROBE-DONE" not in text:
        print("C++ probe FAIL; missing: " + ", ".join(missing or ["CXXPROBE-DONE"]))
        sys.exit(1)
    print("C++ runtime probe PASS")


if __name__ == "__main__":
    main()
