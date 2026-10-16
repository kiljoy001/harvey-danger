#!/usr/bin/env python3
import json
import os
import sys
import subprocess
import glob

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
LIBC_DIR = os.path.join(REPO_ROOT, "sys/src/libc")
BUILD_DIR = os.path.join(REPO_ROOT, "build/libc")
OUT_LIB = os.path.join(REPO_ROOT, "build/libc.a")
INSTALL_LIB = os.path.join(REPO_ROOT, "amd64/lib/libc.a")

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
    "-O2",
    "-g",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
    f"-I{LIBC_DIR}",
    f"-I{os.path.join(LIBC_DIR, '9syscall')}",
]

def load_sources():
    sources = []
    with open(os.path.join(LIBC_DIR, "libc.json"), "r") as f:
        conf = json.load(f)[0]

    for sf in conf["SourceFiles"]:
        path = os.path.join(LIBC_DIR, sf)
        if os.path.exists(path):
            sources.append(path)
        else:
            print(f"Warning: source file {path} not found")

    with open(os.path.join(LIBC_DIR, "amd64/build.json"), "r") as f:
        conf_arch = json.load(f)[0]

    for sf in conf_arch["SourceFiles"]:
        resolved = sf.replace("$ARCH", "amd64")
        path = os.path.join(LIBC_DIR, resolved)
        if os.path.exists(path):
            sources.append(path)
        else:
            print(f"Warning: arch source file {path} not found")

    for sf in glob.glob(os.path.join(LIBC_DIR, "9syscall/*.s")):
        sources.append(sf)
    crypt_path = os.path.join(LIBC_DIR, "port/crypt.c")
    if os.path.exists(crypt_path):
        sources.append(crypt_path)

    return list(set(sources))

def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(INSTALL_LIB), exist_ok=True)

    sources = load_sources()
    print(f"Compiling {len(sources)} libc sources with clang...")

    objs = []
    for src in sources:
        base = os.path.splitext(os.path.basename(src))[0]
        # Avoid collisions between port/foo.c and 9sys/foo.c by incorporating parent folder
        parent = os.path.basename(os.path.dirname(src))
        obj_name = f"{parent}_{base}.o"
        obj_path = os.path.join(BUILD_DIR, obj_name)
        objs.append(obj_path)

        script_mtime = os.path.getmtime(__file__)
        if os.path.exists(obj_path) and os.path.getmtime(obj_path) >= os.path.getmtime(src) and os.path.getmtime(obj_path) >= script_mtime:
            continue

        cmd = [CC] + CFLAGS + [src, "-o", obj_path]
        res = subprocess.run(cmd)
        if res.returncode != 0:
            print(f"Error compiling {src}")
            sys.exit(1)

    print(f"Archiving into {OUT_LIB}...")
    subprocess.run([AR, "rcs", OUT_LIB] + objs, check=True)
    subprocess.run(["cp", OUT_LIB, INSTALL_LIB], check=True)
    print(f"libc.a created successfully at {OUT_LIB} and {INSTALL_LIB}")

if __name__ == "__main__":
    main()
