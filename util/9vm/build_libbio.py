#!/usr/bin/env python3
import os
import sys
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
LIBBIO_DIR = os.path.join(REPO_ROOT, "sys/src/libbio")
BUILD_DIR = os.path.join(REPO_ROOT, "build/libbio")
OUT_LIB = os.path.join(REPO_ROOT, "build/libbio.a")

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
    "-Wno-parentheses",
    "-Wno-missing-braces",
    "-Wno-unknown-pragmas",
    "-O2",
    "-g",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
]

SOURCES = [
    "bbuffered.c",
    "bfildes.c",
    "bflush.c",
    "bgetrune.c",
    "bgetc.c",
    "bgetd.c",
    "binit.c",
    "boffset.c",
    "bprint.c",
    "bputrune.c",
    "bputc.c",
    "brdline.c",
    "brdstr.c",
    "bread.c",
    "bseek.c",
    "bwrite.c",
    "bvprint.c",
]

def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    objs = []
    for s in SOURCES:
        src = os.path.join(LIBBIO_DIR, s)
        base = os.path.splitext(s)[0]
        obj = os.path.join(BUILD_DIR, f"{base}.o")
        objs.append(obj)
        subprocess.run([CC] + CFLAGS + [src, "-o", obj], check=True)

    subprocess.run([AR, "rcs", OUT_LIB] + objs, check=True)
    print(f"libbio.a created successfully at {OUT_LIB} ({os.path.getsize(OUT_LIB)} bytes)")

if __name__ == "__main__":
    main()
