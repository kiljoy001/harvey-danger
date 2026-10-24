#!/usr/bin/env python3
"""Build the minimal Harvey init used by the Halatha/v64 boot test."""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OBJ = ROOT / "build/v64init.o"
OUT = ROOT / "build/v64init"
CFLAGS = [
    "-c", "-O2", "-static", "-mno-red-zone", "-mno-implicit-float",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-mcmodel=small", "-Wall", "-fcommon",
    f"-I{ROOT / 'amd64/include'}", f"-I{ROOT / 'sys/include'}",
    f"-I{ROOT / '../objective-9c'}", f"-I{ROOT / '../objective-9c/libtab'}",
]

def main() -> int:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["clang", *CFLAGS, str(ROOT / "boot/v64init.c"), "-o", str(OBJ)], check=True)
    subprocess.run(["ld.lld", "-e_main", "-static", "-z", "noseparate-code",
                    "-z", "max-page-size=0x200000", str(OBJ),
                    str(ROOT / "build/libc.a"), "-o", str(OUT)], check=True)
    print(f"created {OUT}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
