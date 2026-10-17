#!/usr/bin/env python3
"""Compile Harvey's shared scheduler against the emerging AArch64 kernel ABI.

This is a compile check. Linking and booting the shared kernel are separate
milestones; a successful object file alone does not imply a running scheduler.
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
OUT = Path(os.environ.get("ARM64_OUT", ROOT / "build/arm64"))
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        sys.executable, str(ROOT / "util/9vm/mksys.py"), "-mode=errstr.h",
        "-o", str(OUT / "errstr.h"), str(ROOT / "sys/src/sysconf.json"),
    ], check=True, stdout=subprocess.DEVNULL)
    includes = [OUT, ROOT / "sys/src/9/aarch64", ROOT / "sys/src/9/port",
                ROOT / "sys/src/9/ip", ROOT / "aarch64/include",
                ROOT / "sys/include", ROOT / "sys/src/libc/9syscall"]
    flags = [
        "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-mgeneral-regs-only", "-fno-stack-protector", "-O2", "-Wall",
        "-Wno-unknown-pragmas", "-Wno-unused-function",
        "-Wno-unused-variable",
        *(flag for path in includes for flag in ("-I", str(path))),
    ]
    for name in ("proc", "ps", "edf", "taslock", "qlock"):
        subprocess.run([
            CC, *flags, "-c", str(ROOT / f"sys/src/9/port/{name}.c"),
            "-o", str(OUT / f"{name}.o"),
        ], check=True)
    subprocess.run([
        CC, *flags, "-c", str(ROOT / "sys/src/9/aarch64/arch.c"),
        "-o", str(OUT / "arch.o"),
    ], check=True)
    print("AArch64 shared process, EDF, lock and architecture hooks compile PASS (link and boot pending)")


if __name__ == "__main__":
    main()
