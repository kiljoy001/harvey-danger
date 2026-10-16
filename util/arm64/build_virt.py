#!/usr/bin/env python3
"""Build Harvey's first ARM64 Image for QEMU virt direct boot."""

from pathlib import Path
import os
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "sys/src/9/aarch64"
OUT = Path(os.environ.get("ARM64_OUT", ROOT / "build/arm64"))
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM64_LD", "ld.lld")
OBJCOPY = os.environ.get("ARM64_OBJCOPY", "llvm-objcopy")


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    flags = [
        "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-mgeneral-regs-only",
        "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
        "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "aarch64/include"),
    ]
    if os.environ.get("ARM64_TEST_FAULT") == "1":
        flags.append("-DARM64_TEST_FAULT")
    if os.environ.get("ARM64_TEST_RESERVE") == "1":
        flags.append("-DARM64_TEST_RESERVE")
    objects = []
    for name in ("l.S", "user.S", "main.c", "fdt.c", "timer.c", "phys.c", "mmu.c", "user.c"):
        obj = OUT / (name + ".o")
        subprocess.run([CC, *flags, "-c", str(SOURCE / name), "-o", str(obj)], check=True)
        objects.append(obj)
    elf = OUT / "harvey-virt.elf"
    image = OUT / "harvey-virt.img"
    subprocess.run([LD, "-T", str(SOURCE / "virt.ld"),
                    *map(str, objects), "-o", str(elf)], check=True)
    subprocess.run([OBJCOPY, "-O", "binary", str(elf), str(image)], check=True)
    print(image)


if __name__ == "__main__":
    main()
