#!/usr/bin/env python3
"""Build libunwind for Harvey.

The unwinder is what `throw`/`catch` stand on: libc++abi's personality routine
walks the stack with `_Unwind_*`.  Harvey has no dynamic loader, but
`util/clang-harvey/target-include/link.h` and `dl_iterate_phdr()` (in
compat.cpp) report the single static image, so libunwind's normal ELF path
works unmodified.

Usage:
    util/9vm/build_libunwind.py
    util/9vm/build_libunwind.py --clean

Output: build/libcxx/lib/libunwind.a
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(os.environ.get(
    "HARVEY", Path(__file__).resolve().parents[2])).resolve()
LLVM = Path(os.environ.get(
    "LLVM_PROJECT", REPO_ROOT.parent / "llvm-project")).resolve()
LIBUNWIND = LLVM / "libunwind"
LIBCXX = REPO_ROOT / "build" / "libcxx"
OBJDIR = LIBCXX / "obj" / "libunwind"
OUT = LIBCXX / "lib" / "libunwind.a"
WRAPPER = REPO_ROOT / "util" / "clang-harvey"

C_SOURCES = ["UnwindLevel1.c", "UnwindLevel1-gcc-ext.c"]
CXX_SOURCES = ["libunwind.cpp"]
ASM_SOURCES = ["UnwindRegistersSave.S", "UnwindRegistersRestore.S"]


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


def flags():
    return [
        f"-I{LIBUNWIND / 'include'}",
        f"-I{LIBUNWIND / 'src'}",
        # Harvey has one thread for now, so libunwind needs no rwlock.
        "-D_LIBUNWIND_HAS_NO_THREADS",
        "-O2",
        "-g",
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--clean", action="store_true")
    args = ap.parse_args()

    if args.clean:
        shutil.rmtree(OBJDIR, ignore_errors=True)
        OUT.unlink(missing_ok=True)
        return

    if not (LIBUNWIND / "src" / "UnwindLevel1.c").is_file():
        sys.exit(f"libunwind source not found at {LIBUNWIND}")

    OBJDIR.mkdir(parents=True, exist_ok=True)
    OUT.parent.mkdir(parents=True, exist_ok=True)

    objects = []
    for name in C_SOURCES:
        obj = OBJDIR / (name + ".o")
        run([str(WRAPPER / "harvey-clang"), *flags(),
             "-c", str(LIBUNWIND / "src" / name), "-o", str(obj)])
        objects.append(obj)
    for name in CXX_SOURCES:
        obj = OBJDIR / (name + ".o")
        run([str(WRAPPER / "harvey-clang++"), *flags(),
             "-c", str(LIBUNWIND / "src" / name), "-o", str(obj)])
        objects.append(obj)
    for name in ASM_SOURCES:
        obj = OBJDIR / (name + ".o")
        run([str(WRAPPER / "harvey-clang"), *flags(),
             "-c", str(LIBUNWIND / "src" / name), "-o", str(obj)])
        objects.append(obj)

    OUT.unlink(missing_ok=True)
    run(["llvm-ar", "rcs", str(OUT), *map(str, objects)])
    print(f"built {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
