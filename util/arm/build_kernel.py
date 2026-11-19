#!/usr/bin/env python3
"""Build the ARMv7-A Harvey kernel for QEMU's 32-bit "virt" board.

Harvey has no 32-bit ARM kernel yet: this is the board-independent core
first (entry, MMU, PL011 console, GIC-400 and the ARM generic timer).
The shared port layer is expected to link against it next; for now the
image proves the arch layer boots with caches and interrupts enabled.

Usage:
    python3 util/arm/build_kernel.py [--out DIR]
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ARCH = ROOT / "sys/src/9/arm"
DEFAULT_OUT = ROOT / "build/arm32-kernel"

CC = os.environ.get("ARM32_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM32_LD", "ld.lld")
OBJCOPY = os.environ.get("ARM32_OBJCOPY", "llvm-objcopy")

CFLAGS = [
    "--target=armv7a-none-eabi",
    "-march=armv7-a",
    "-mfloat-abi=soft",
    "-mno-unaligned-access",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-fno-pic",
    "-fno-pie",
    "-fcommon",
    "-nostdlib",
    "-O2",
    "-g",
    "-Wall",
    "-Wno-unused-parameter",
    "-I", str(ARCH),
]

SOURCES = ["l.S", "context.S", "main.c", "mmu.c", "arch.c"]


def run(cmd: list[str]) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    out = args.out
    out.mkdir(parents=True, exist_ok=True)

    objects = []
    for name in SOURCES:
        obj = out / (name.replace("/", "_") + ".o")
        run([CC, *CFLAGS, "-c", str(ARCH / name), "-o", str(obj)])
        objects.append(obj)

    elf = out / "harvey-arm32"
    run([LD, "-nostdlib", "-static", "-T", str(ARCH / "kernel.ld"),
         "-z", "max-page-size=0x1000", "-o", str(elf), *map(str, objects)])

    # A flat binary is handy for boards whose loaders do not parse ELF.
    binfile = out / "harvey-arm32.bin"
    run([OBJCOPY, "-O", "binary", str(elf), str(binfile)])
    print(f"ARMv7 kernel: {elf}")


if __name__ == "__main__":
    main()
