#!/usr/bin/env python3
import json
import os
import sys
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
LIBMP_DIR = os.path.join(REPO_ROOT, "sys/src/libmp")
BUILD_DIR = os.path.join(REPO_ROOT, "build/libmp")
OUT_LIB = os.path.join(REPO_ROOT, "build/libmp.a")

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
    f"-I{os.path.join(LIBMP_DIR, 'port')}",
]

def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    with open(os.path.join(LIBMP_DIR, "libmp.json"), "r") as f:
        conf = json.load(f)[0]

    objs = []
    for sf in conf["SourceFiles"]:
        base = os.path.splitext(os.path.basename(sf))[0]
        if base == "crttest":
            continue
        src = os.path.join(LIBMP_DIR, sf)
        obj = os.path.join(BUILD_DIR, f"{base}.o")
        objs.append(obj)
        cmd = [CC] + CFLAGS + [src, "-o", obj]
        res = subprocess.run(cmd)
        if res.returncode != 0:
            print(f"Failed to compile {src}", file=sys.stderr)
            sys.exit(1)

    ar_cmd = [AR, "rcs", OUT_LIB] + objs
    res = subprocess.run(ar_cmd)
    if res.returncode != 0:
        print(f"Failed to create archive {OUT_LIB}", file=sys.stderr)
        sys.exit(1)

    print(f"libmp.a created successfully at {OUT_LIB} ({os.path.getsize(OUT_LIB)} bytes)")

if __name__ == "__main__":
    main()
