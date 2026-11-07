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
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

EXPECT = [
    "containers", "exceptions", "exception_ptr", "iostream", "locale",
    "regex", "regex_replace", "chrono_sleep", "threads", "random",
    "filesystem", "format_charconv", "smart_ptr", "locale_facet",
    "latch_semaphore", "barrier_jthread", "path_ops", "ranges_span_variant",
    "fstream_filesystem", "print", "locale_env", "put_time",
    "filesystem_iter_chrono", "coroutine", "format_chrono",
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

    env = ["--env", "TMPDIR=/initramfs/sdk/tmp"]
    marker = "CXX-GUEST-DONE"
    if args.lto:
        env += ["--env", "CXX_EXTRA=-flto"]
    if args.fmt:
        fmt_src = Path(args.fmt_src)
        if not (fmt_src / "include" / "fmt" / "format.h").is_file():
            sys.exit(f"missing {fmt_src}/include/fmt/format.h; download fmt")
        shutil.copy2(HERE / "testdata" / "fmtprobe.cpp", tdir / "fmtprobe.cpp")
        env += ["--env", "CXX_SRC=/initramfs/t/fmtprobe.cpp",
                "--env", "CXX_STD=-std=c++17",
                "--env", "CXX_INC=-I/initramfs/fmt/include",
                "--env", "CXX_OUT=/initramfs/bin/fmtprobe"]
        extra = ["--tree", f"{fmt_src / 'include'}:/initramfs/fmt/include"]
    else:
        shutil.copy2(HERE / "testdata" / "cxxprobe.cpp", tdir / "cxxprobe.cpp")
        extra = []

    runner = stage / "runner.elf"
    cmd = [sys.executable, str(HERE / "mkrun.py"),
           str(ROOT / "build" / "arm64-apex" / "hello.elf"),
           "--in-tree", "/initramfs/bin/cxx",
           "--untar", "/boot/sdk.tar:/initramfs",
           *env,
           "--tree", f"{bindir}:/initramfs/bin",
           "--tree", f"{tdir}:/initramfs/t",
           *extra,
           "--suffix", "", "--output", str(runner)]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)
    dump = Path(args.dump)
    cmd = [sys.executable, str(HERE / "test_apex_boot.py"),
           "--elf", str(runner), "--archive", args.sdk_archive,
           "--marker", marker, "--timeout", "300",
           "--deadline", "2400", "--mem", str(args.mem),
           "--smp", str(args.smp), "--dump", str(dump)]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)
    if args.fmt:
        text = dump.read_text(errors="replace")
        print(text[-1200:])
        if "harvey 042 3.50 [1, 2, 3]" not in text or marker not in text:
            sys.exit("fmt probe FAIL")
        print("fmt probe PASS")
    else:
        check(dump, marker)


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
    ap.add_argument("--fmt", action="store_true",
                    help="with --guest, build the header-only fmt probe")
    ap.add_argument("--lto", action="store_true",
                    help="with --guest, add -flto to the SDK compile")
    ap.add_argument("--fmt-src", default="/tmp/opencode/fmt-src",
                    help="fmt checkout for --fmt")
    args = ap.parse_args()

    stage = Path(args.stage)
    if args.guest:
        run_guest(args, stage)
        return

    stage.mkdir(parents=True, exist_ok=True)
    elf = stage / "cxxprobe.elf"
    cmd = [str(HERE / "aarch64-clang++"), "-std=c++23",
           str(HERE / "testdata" / "cxxprobe.cpp"), "-o", str(elf)]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)

    # A writable /initramfs is needed for the fstream/filesystem test.
    boot = stage / "small.boot"
    blob = b"\0" * 1024
    header = struct.pack("<8sIIII", b"O9BOOT01", 1, 1, 24 + 48 + len(blob), 0)
    entry = struct.pack("<32sIIII", b"sdk.tar", 24 + 48, len(blob), 0o555, 0)
    boot.write_bytes(header + entry + blob)
    runner = stage / "runner.elf"
    cmd = [sys.executable, str(HERE / "mkrun.py"), str(elf),
           "--untar", "/boot/sdk.tar:/initramfs",
           "--suffix", "", "--output", str(runner)]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)

    cmd = [sys.executable, str(HERE / "test_apex_boot.py"),
           "--elf", str(runner), "--archive", str(boot),
           "--marker", "CXXPROBE-DONE",
           "--timeout", "60", "--mem", str(args.mem),
           "--smp", str(args.smp), "--dump", args.dump]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)

    check(Path(args.dump), "CXXPROBE-DONE")


if __name__ == "__main__":
    main()
