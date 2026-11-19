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
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ARCH = ROOT / "sys/src/9/arm"
DEFAULT_OUT = ROOT / "build/arm32-kernel"

CC = os.environ.get("ARM32_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM32_LD", "ld.lld")
OBJCOPY = os.environ.get("ARM32_OBJCOPY", "llvm-objcopy")
AR = os.environ.get("ARM32_AR", "llvm-ar")

CFLAGS = [
    "--target=armv7a-none-eabi",
    "-march=armv7-a",
    "-mfloat-abi=softfp",
    "-mfpu=vfpv3-d16",
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
    "-I", str(ROOT / "sys/src/9/port"),
    "-I", str(ROOT / "sys/include"),
]

SOURCES = ["l.S", "context.S", "main.c", "mmu.c", "arch.c", "div.c",
           "lock.c", "console.c", "mach.c", "platform.c", "qmalloc.c"]
LIBC = ROOT / "sys/src/libc"


def run(cmd: list[str]) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)


def build_klibc(out: Path) -> Path:
    """Kernel libc: the arch-neutral subset in libc/klibc.json."""
    objdir = out / "klibc"
    objdir.mkdir(parents=True, exist_ok=True)
    archive = out / "klibc.a"
    entries = json.loads((LIBC / "klibc.json").read_text())[0]["SourceFiles"]
    # port/lock.c is the userspace lock (it sleeps through semacquire);
    # the kernel provides lock/unlock in sys/src/9/arm/lock.c instead.
    entries = [name for name in entries if name != "port/lock.c"]

    def compile_one(name: str) -> Path:
        obj = objdir / (name.replace("/", "_") + ".o")
        cmd = [CC, *CFLAGS, "-I", str(LIBC), "-I", str(ROOT / "sys/include"),
               "-c", str(LIBC / name), "-o", str(obj)]
        subprocess.run(cmd, check=True, capture_output=True)
        return obj

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        objects = list(pool.map(compile_one, entries))
    archive.unlink(missing_ok=True)
    subprocess.run([AR, "rcs", str(archive), *map(str, objects)], check=True)
    return archive


PORT_SOURCES = [
    "../port/allocb.c",
    "../port/qio.c",
]


def build_port(out: Path) -> list[Path]:
    """The first slice of the shared kernel that the bring-up exercises."""
    objdir = out / "port"
    objdir.mkdir(parents=True, exist_ok=True)
    objects = []
    for name in PORT_SOURCES:
        source = ARCH / name
        obj = objdir / (name.replace("/", "_").replace("..", "") + ".o")
        subprocess.run([CC, *CFLAGS, "-D__HARVEY__",
                        "-I", str(ROOT / "sys/src/9/port"),
                        "-I", str(ROOT / "sys/include"),
                        "-c", str(source), "-o", str(obj)],
                       check=True, capture_output=True)
        objects.append(obj)
    return objects


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    for mode, name in (("errstr.h", "errstr.h"), ("sys.h", "sys.h")):
        subprocess.run([sys.executable, str(ROOT.parent / "9vm/mksys.py"),
                        f"-mode={mode}", "-o", str(out / name),
                        str(ROOT / "sys/src/sysconf.json")],
                       check=True, stdout=subprocess.DEVNULL)
    CFLAGS.extend(["-I", str(out)])

    objects = []
    for name in SOURCES:
        obj = out / (name.replace("/", "_") + ".o")
        run([CC, *CFLAGS, "-c", str(ARCH / name), "-o", str(obj)])
        objects.append(obj)

    klibc = build_klibc(out)
    elf = out / "harvey-arm32"
    run([LD, "-nostdlib", "-static", "-T", str(ARCH / "kernel.ld"),
         "-z", "max-page-size=0x1000", "-o", str(elf),
         *map(str, objects), str(klibc)])

    # A flat binary is handy for boards whose loaders do not parse ELF.
    binfile = out / "harvey-arm32.bin"
    run([OBJCOPY, "-O", "binary", str(elf), str(binfile)])
    print(f"ARMv7 kernel: {elf}")


if __name__ == "__main__":
    main()
