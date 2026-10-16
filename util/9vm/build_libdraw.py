#!/usr/bin/env python3
import os
import sys
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
LIBDRAW_DIR = os.path.join(REPO_ROOT, "sys/src/libdraw")
LIBMEMDRAW_DIR = os.path.join(REPO_ROOT, "sys/src/libmemdraw")
BUILD_DRAW_DIR = os.path.join(REPO_ROOT, "build/libdraw")
BUILD_MEMDRAW_DIR = os.path.join(REPO_ROOT, "build/libmemdraw")
OUT_DRAW = os.path.join(REPO_ROOT, "build/libdraw.a")
OUT_MEMDRAW = os.path.join(REPO_ROOT, "build/libmemdraw.a")

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
    "-Wno-unused-but-set-variable",
    "-O2",
    "-g",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
]

def build_lib(src_dir, build_dir, out_a, exclude=None):
    if exclude is None:
        exclude = set()
    os.makedirs(build_dir, exist_ok=True)
    c_files = [f for f in sorted(os.listdir(src_dir)) if f.endswith(".c") and f not in exclude]
    objs = []
    for f in c_files:
        src = os.path.join(src_dir, f)
        base = os.path.splitext(f)[0]
        obj = os.path.join(build_dir, f"{base}.o")
        objs.append(obj)
        res = subprocess.run([CC] + CFLAGS + [src, "-o", obj])
        if res.returncode != 0:
            print(f"Failed to compile {src}", file=sys.stderr)
            sys.exit(1)
    subprocess.run([AR, "rcs", out_a] + objs, check=True)
    print(f"Created {out_a} ({os.path.getsize(out_a)} bytes)")

def main():
    print("Building libdraw.a...")
    build_lib(LIBDRAW_DIR, BUILD_DRAW_DIR, OUT_DRAW, exclude={"test.c"})

    print("Building libmemdraw.a...")
    build_lib(LIBMEMDRAW_DIR, BUILD_MEMDRAW_DIR, OUT_MEMDRAW, exclude={"arctest.c", "drawtest.c"})

if __name__ == "__main__":
    main()
