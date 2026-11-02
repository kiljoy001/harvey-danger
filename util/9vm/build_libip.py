#!/usr/bin/env python3
"""Build Harvey's user-space libip.a for the guest toolchain."""

import os
import sys
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
LIBIP_DIR = os.path.join(REPO_ROOT, "sys/src/libip")
BUILD_DIR = os.path.join(REPO_ROOT, "build/libip")
OUT_LIB = os.path.join(REPO_ROOT, "build/libip.a")

CC = "clang"
AR = "llvm-ar"

CFLAGS = [
    "-c",
    "-mno-red-zone",
    "-mno-implicit-float",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-mcmodel=small",
    "-Wall",
    "-fcommon",
    "-Wno-parentheses",
    "-Wno-missing-braces",
    "-Wno-unknown-pragmas",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    "-O2",
    "-g",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
    f"-I{LIBIP_DIR}",
]

# sys/src/libip/libip.json
SOURCES = [
    "bo.c",
    "classmask.c",
    "eipfmt.c",
    "equivip.c",
    "ipaux.c",
    "myetheraddr.c",
    "myipaddr.c",
    "parseether.c",
    "parseip.c",
    "ptclbsum.c",
    "readipifc.c",
]


def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    objs = []
    for src in SOURCES:
        path = os.path.join(LIBIP_DIR, src)
        obj = os.path.join(BUILD_DIR, os.path.splitext(src)[0] + ".o")
        objs.append(obj)
        cmd = [CC] + CFLAGS + [path, "-o", obj]
        res = subprocess.run(cmd)
        if res.returncode != 0:
            print(f"Failed to compile {path}", file=sys.stderr)
            sys.exit(1)

    res = subprocess.run([AR, "rcs", OUT_LIB] + objs)
    if res.returncode != 0:
        print(f"Failed to create {OUT_LIB}", file=sys.stderr)
        sys.exit(1)
    print(f"libip.a created successfully at {OUT_LIB} ({os.path.getsize(OUT_LIB)} bytes)")


if __name__ == "__main__":
    main()
