#!/usr/bin/env python3
"""Build the guest user-space /net service (o9net)."""
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "sys/src/cmd/o9net/netd.c"
OUT = ROOT / "build/netd"
OBJ = ROOT / "build/netd.o"
CC = "gcc"
LD = "ld.lld"

CFLAGS = [
    "-c", "-O2", "-g", "-fplan9-extensions", "-ffreestanding",
    "-fno-builtin", "-fno-stack-protector", "-fcommon",
    "-mno-red-zone", "-mno-sse", "-Wno-unknown-pragmas",
    "-Wno-incompatible-pointer-types", "-Wno-unused-variable",
    "-Wno-unused-function", "-I" + str(ROOT / "sys/include"),
    "-I" + str(ROOT / "amd64/include"),
]
LDFLAGS = ["-e_main", "-static", "-z", "noseparate-code",
           "-z", "max-page-size=0x200000"]
LIBS = ["libthread.a", "lib9p.a", "libauth.a", "libsec.a", "libbio.a",
        "libc.a"]

def main():
    OBJ.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([CC, *CFLAGS, str(SRC), "-o", str(OBJ)], check=True)
    subprocess.run([LD, *LDFLAGS, str(OBJ),
                    *[str(ROOT / "build" / lib) for lib in LIBS],
                    "-o", str(OUT)], check=True)
    subprocess.run(["llvm-strip", "--strip-debug", str(OUT)], check=True)
    print(f"Built {OUT} ({OUT.stat().st_size} bytes)")

if __name__ == "__main__":
    main()
