#!/usr/bin/env python3
import json
import os
import sys
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
LIBAUTHSRV_DIR = os.path.join(REPO_ROOT, "sys/src/libauthsrv")
BUILD_DIR = os.path.join(REPO_ROOT, "build/libauthsrv")
OUT_LIB = os.path.join(REPO_ROOT, "build/libauthsrv.a")

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
    f"-I{LIBAUTHSRV_DIR}",
    f"-I{os.path.join(LIBAUTHSRV_DIR, 'dp9ik')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/src/cmd/auth/factotum')}",
]

def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    with open(os.path.join(LIBAUTHSRV_DIR, "libauthsrv.json"), "r") as f:
        conf = json.load(f)[0]

    objs = []
    for s in conf["SourceFiles"]:
        src = os.path.join(LIBAUTHSRV_DIR, s)
        base = s.replace("/", "_").replace(".c", "")
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

    print(f"libauthsrv.a created successfully at {OUT_LIB} ({os.path.getsize(OUT_LIB)} bytes)")

if __name__ == "__main__":
    main()
