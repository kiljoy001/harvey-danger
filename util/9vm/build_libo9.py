#!/usr/bin/env python3
import os
import sys
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
O9_REPO = os.path.abspath(os.environ.get(
    "O9_REPO", os.path.join(REPO_ROOT, "..", "objective-9c")))
LIBTAB_DIR = os.path.join(O9_REPO, "libtab")
BUILD_DIR = os.path.join(REPO_ROOT, "build/libo9")
OUT_LIB = os.path.join(REPO_ROOT, "build/libo9.a")

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
    f"-I{O9_REPO}",
    f"-I{LIBTAB_DIR}",
    "-DMONO_PLAN9",
]

SOURCES = [
    os.path.join(O9_REPO, "o9_dispatch.c"),
    os.path.join(O9_REPO, "o9_runtime.c"),
    os.path.join(O9_REPO, "o9_filetree.c"),
    os.path.join(O9_REPO, "o9_view.c"),
    os.path.join(O9_REPO, "o9_crypto.c"),
    os.path.join(O9_REPO, "monocypher.c"),
    os.path.join(O9_REPO, "o9_tab_discard.c"),
    os.path.join(LIBTAB_DIR, "tab_error.c"),
    os.path.join(LIBTAB_DIR, "tab_create.c"),
    os.path.join(LIBTAB_DIR, "tab_row.c"),
    os.path.join(LIBTAB_DIR, "tab_rowmap.c"),
    os.path.join(LIBTAB_DIR, "tab_iter.c"),
    os.path.join(LIBTAB_DIR, "tab_codec.c"),
    os.path.join(LIBTAB_DIR, "tab_open.c"),
    os.path.join(LIBTAB_DIR, "tab_serialize.c"),
    os.path.join(LIBTAB_DIR, "tab_persist.c"),
    os.path.join(LIBTAB_DIR, "tab_emit.c"),
    os.path.join(LIBTAB_DIR, "tab_text.c"),
    os.path.join(LIBTAB_DIR, "tab_writer.c"),
]

def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    objs = []
    for src in SOURCES:
        base = os.path.splitext(os.path.basename(src))[0]
        obj = os.path.join(BUILD_DIR, f"{base}.o")
        objs.append(obj)
        print(f"Compiling {os.path.basename(src)}...")
        res = subprocess.run([CC] + CFLAGS + [src, "-o", obj])
        if res.returncode != 0:
            print(f"Failed to compile {src}")
            sys.exit(1)

    subprocess.run([AR, "rcs", OUT_LIB] + objs, check=True)
    print(f"libo9.a created successfully at {OUT_LIB} ({os.path.getsize(OUT_LIB)} bytes)")

if __name__ == "__main__":
    main()
