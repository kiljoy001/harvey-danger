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
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

EXPECT = [
    "containers", "exceptions", "exception_ptr", "iostream", "locale",
    "regex", "regex_replace", "chrono_sleep", "threads", "random",
    "filesystem", "format_charconv", "smart_ptr", "locale_facet",
    "latch_semaphore", "barrier_jthread", "path_ops", "ranges_span_variant",
]


def check(dump: Path, marker: str) -> None:
    text = dump.read_text(errors="replace")
    lines = [l for l in text.splitlines() if "CXXPROBE" in l]
    print("\n".join(lines))
    missing = []
    for name in EXPECT:
        if not any(f"CXXPROBE {name}" in l and " OK" in l for l in lines):
            missing.append(name)
    if missing or marker not in text:
        print("C++ probe FAIL; missing: " + ", ".join(missing or [marker]))
        sys.exit(1)
    print("C++ runtime probe PASS")


def run_guest(args, stage: Path) -> None:
    if not Path(args.sdk_archive).is_file():
        sys.exit(f"missing {args.sdk_archive}; build it with "
                 "test-selfbuild-tblgen.py --stage-only")
    shutil.rmtree(stage, ignore_errors=True)
    bindir = stage / "bin"
    tdir = stage / "t"
    bindir.mkdir(parents=True)
    tdir.mkdir()
    driver = bindir / "cxx"
    cmd = [str(HERE / "aarch64-clang"), str(HERE / "selfbuild-cxx.c"),
           "-o", str(driver)]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)
    shutil.copy2(HERE / "testdata" / "cxxprobe.cpp", tdir / "cxxprobe.cpp")
    runner = stage / "runner.elf"
    cmd = [sys.executable, str(HERE / "mkrun.py"),
           str(ROOT / "build" / "arm64-apex" / "hello.elf"),
           "--in-tree", "/initramfs/bin/cxx",
           "--untar", "/boot/sdk.tar:/initramfs",
           "--env", "TMPDIR=/initramfs/sdk/tmp",
           "--tree", f"{bindir}:/initramfs/bin",
           "--tree", f"{tdir}:/initramfs/t",
           "--suffix", "", "--output", str(runner)]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)
    dump = Path(args.dump)
    cmd = [sys.executable, str(HERE / "test_apex_boot.py"),
           "--elf", str(runner), "--archive", args.sdk_archive,
           "--marker", "CXX-GUEST-DONE", "--timeout", "300",
           "--deadline", "2400", "--mem", str(args.mem),
           "--smp", str(args.smp), "--dump", str(dump)]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)
    check(dump, "CXX-GUEST-DONE")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--mem", type=int, default=1024)
    ap.add_argument("--stage", default="/tmp/opencode/cxxprobe")
    ap.add_argument("--dump", default="/tmp/opencode/cxxprobe.log")
    ap.add_argument("--guest", action="store_true",
                    help="compile the probe with the SDK clang++ in the guest")
    ap.add_argument("--sdk-archive",
                    default="/home/scott/tftp/aspera-tblgen-sdk.boot",
                    help="boot archive holding /boot/sdk.tar for --guest")
    args = ap.parse_args()

    stage = Path(args.stage)
    if args.guest:
        run_guest(args, stage)
        return

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

    check(Path(args.dump), "CXXPROBE-DONE")


if __name__ == "__main__":
    main()
