#!/usr/bin/env python3
"""Build the fmt library and a program with the shipped guest toolchain.

Compiles fmt/src/format.cc and a probe separately, archives format.o with
llvm-ar, links the probe against libfmt.a and the SDK runtime, and runs it.

    python3 util/arm64/test-fmtlib.py --smp 1 --mem 3072
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
    ap.add_argument("--fmt-src", default="/tmp/opencode/fmt-src")
    ap.add_argument("--stage", default="/tmp/opencode/fmtlib")
    ap.add_argument("--dump", default="/tmp/opencode/fmtlib.log")
    ap.add_argument("--sdk-archive",
                    default="/home/scott/tftp/aspera-tblgen-sdk.boot")
    args = ap.parse_args()

    fmt = Path(args.fmt_src)
    if not (fmt / "src" / "format.cc").is_file():
        sys.exit(f"missing {fmt}/src/format.cc")
    if not Path(args.sdk_archive).is_file():
        sys.exit(f"missing {args.sdk_archive}")
    stage = Path(args.stage)
    shutil.rmtree(stage, ignore_errors=True)
    bindir = stage / "bin"
    tdir = stage / "t"
    bindir.mkdir(parents=True)
    tdir.mkdir()
    subprocess.run([str(HERE / "aarch64-clang"),
                    str(HERE / "selfbuild-fmt.c"),
                    "-o", str(bindir / "fmtbuild")], check=True)
    shutil.copy2(HERE / "testdata" / "fmtlibprobe.cpp",
                 tdir / "fmtlibprobe.cpp")
    runner = stage / "runner.elf"
    subprocess.run([sys.executable, str(HERE / "mkrun.py"),
                    str(ROOT / "build" / "arm64-apex" / "hello.elf"),
                    "--in-tree", "/initramfs/bin/fmtbuild",
                    "--untar", "/boot/sdk.tar:/initramfs",
                    "--tree", f"{bindir}:/initramfs/bin",
                    "--tree", f"{tdir}:/initramfs/t",
                    "--tree", f"{fmt / 'include'}:/initramfs/fmt/include",
                    "--tree", f"{fmt / 'src'}:/initramfs/fmt/src",
                    "--suffix", "", "--output", str(runner)], check=True)
    dump = Path(args.dump)
    subprocess.run([sys.executable, str(HERE / "test_apex_boot.py"),
                    "--elf", str(runner), "--archive", args.sdk_archive,
                    "--marker", "FMTLIB-DONE", "--timeout", "180",
                    "--deadline", "900", "--mem", str(args.mem),
                    "--smp", str(args.smp), "--dump", str(dump)], check=True)
    text = dump.read_text(errors="replace")
    for line in text.splitlines():
        if line.startswith("FMTLIB"):
            print(line)
    if "harvey 042 3.50 [1, 2, 3]" not in text or "FMTLIB-DONE" not in text:
        sys.exit("fmt library build FAIL")
    print("fmt library build PASS")


if __name__ == "__main__":
    main()
