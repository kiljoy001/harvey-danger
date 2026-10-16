#!/usr/bin/env python3
import os
import sys
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
LIB9P_DIR = os.path.join(REPO_ROOT, "sys/src/lib9p")
BUILD_DIR = os.path.join(REPO_ROOT, "build/lib9p")
OUT_LIB = os.path.join(REPO_ROOT, "build/lib9p.a")

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
    "-Wno-unused-variable",
    "-Wno-unused-function",
    "-O2",
    "-g",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
    f"-I{LIB9P_DIR}",
]

SOURCES = [
    "auth.c",
    "dirread.c",
    "fid.c",
    "file.c",
    "intmap.c",
    "listen.c",
    "mem.c",
    "req.c",
    "parse.c",
    "post.c",
    "rfork.c",
    "srv.c",
    "thread.c",
    "uid.c",
    "util.c",
]

def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    objs = []
    for s in SOURCES:
        src = os.path.join(LIB9P_DIR, s)
        base = os.path.splitext(s)[0]
        obj = os.path.join(BUILD_DIR, f"{base}.o")
        objs.append(obj)
        res = subprocess.run([CC] + CFLAGS + [src, "-o", obj])
        if res.returncode != 0:
            print(f"Failed to compile {src}")
            sys.exit(1)

    subprocess.run([AR, "rcs", OUT_LIB] + objs, check=True)
    print(f"lib9p.a created successfully at {OUT_LIB} ({os.path.getsize(OUT_LIB)} bytes)")

if __name__ == "__main__":
    main()
