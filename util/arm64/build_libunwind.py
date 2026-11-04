#!/usr/bin/env python3
"""Build libunwind for ARM64 Harvey.

The unwinder is what `throw`/`catch` stand on: libc++abi's personality
routine walks the stack with `_Unwind_*`.  libunwind's UnwindRegisters*.S
include the aarch64 register save/restore from the same source file.

    util/arm64/build_libunwind.py
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
LLVM = Path(os.environ.get("LLVM_PROJECT",
                           ROOT.parent / "llvm-project")).resolve()
LIBUNWIND = LLVM / "libunwind"
OUTDIR = ROOT / "build" / "arm64-libcxx"
OBJDIR = OUTDIR / "obj" / "libunwind"
OUT = OUTDIR / "lib" / "libunwind.a"
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"

C_SOURCES = ["UnwindLevel1.c", "UnwindLevel1-gcc-ext.c"]
CXX_SOURCES = ["libunwind.cpp"]
ASM_SOURCES = ["UnwindRegistersSave.S", "UnwindRegistersRestore.S"]


def run(cmd: list[str]) -> None:
    subprocess.run(cmd, check=True)


def flags(include_compat: bool = True) -> list[str]:
    out = [
        "--target=aarch64-none-elf",
        "-mstrict-align",
        f"-I{ROOT / 'util' / 'clang-harvey' / 'target-include'}",
        f"-I{ROOT / 'apex' / 'include'}",
        f"-I{ROOT / 'apex' / 'aarch64' / 'include'}",
        f"-I{LIBUNWIND / 'include'}",
        f"-I{LIBUNWIND / 'src'}",
        # One thread at a time for now, so libunwind needs no rwlock.
        "-D_LIBUNWIND_HAS_NO_THREADS",
        "-D_SUSV2_SOURCE", "-D_POSIX_SOURCE", "-D_LIMITS_EXTENSION",
        "-D_GNU_SOURCE", "-DHARVEY",
        "-O2",
        "-g",
        # aarch64-none-elf does not get asynchronous unwind tables by
        # default: without them the unwinder cannot leave its own frames
        # (_Unwind_RaiseException is the first frame), so phase 1 reports
        # end of stack.
        "-funwind-tables",
        "-fno-omit-frame-pointer",
    ]
    if include_compat:
        # compat.h supplies PRIxPTR and the other format macros APEX's
        # inttypes.h lacks; it cannot be force-included into assembly.
        out += ["-include", str(ROOT / "util" / "libcxx" / "compat.h")]
    return out


def main() -> None:
    if not (LIBUNWIND / "src" / "UnwindLevel1.c").is_file():
        sys.exit(f"libunwind source not found at {LIBUNWIND}")
    OBJDIR.mkdir(parents=True, exist_ok=True)
    OUT.parent.mkdir(parents=True, exist_ok=True)

    objects = []
    for name in C_SOURCES + CXX_SOURCES + ASM_SOURCES:
        obj = OBJDIR / (name + ".o")
        # In C++ mode wchar_t is a keyword; _WCHAR_T stops APEX's stddef.h
        # from typedef'ing it.
        asm = name.endswith(".S")
        extra = ["-D_WCHAR_T"] if name.endswith((".cpp", ".cc")) else []
        run([CC, *flags(not asm), *extra, "-c",
             str(LIBUNWIND / "src" / name), "-o", str(obj)])
        objects.append(obj)

    OUT.unlink(missing_ok=True)
    run(["llvm-ar", "rcs", str(OUT), *map(str, objects)])
    print(f"built {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
