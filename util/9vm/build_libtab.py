#!/usr/bin/env python3
"""Build libtab for Harvey, plus its example and crypto test binaries.

libtab stores ndb-shaped tables with an in-memory row index, typed
cells and atomic writes.  HASHED and SIGNED cells use Monocypher, so
build_libmonocypher.py must run first (or alongside this script).
"""
import os
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
SRC_DIR = os.path.join(REPO_ROOT, "sys/src/libtab")
BUILD_DIR = os.path.join(REPO_ROOT, "build/libtab")
OUT_LIB = os.path.join(REPO_ROOT, "build/libtab.a")

CC = "clang"
AR = "llvm-ar"

CFLAGS = [
    "-c", "-O2",
    "-mno-red-zone", "-mno-implicit-float",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-mcmodel=small", "-fcommon",
    "-Wno-unknown-pragmas", "-Wno-implicit-int",
    "-Wno-implicit-function-declaration",
    "-Wno-incompatible-function-pointer-types", "-Wno-parentheses",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
    f"-I{SRC_DIR}",
]

LDFLAGS = ["-e_main", "-static", "-z", "noseparate-code",
           "-z", "max-page-size=0x200000"]

SOURCES = [
    "tab_codec.c", "tab_create.c", "tab_emit.c", "tab_error.c",
    "tab_hashed.c", "tab_iter.c", "tab_open.c", "tab_persist.c",
    "tab_row.c", "tab_rowmap.c", "tab_serialize.c", "tab_signed.c",
    "tab_text.c", "tab_writer.c", "getrandom_compat.c",
]

TESTS = [
    ("test_libtab.c", "aspera-tabtest"),
    ("tabcryptest.c", "aspera-tabcryptest"),
]

LIBS = ["build/libmonocypher.a", "build/libndb.a", "build/libbio.a",
        "build/libc.a"]


def main() -> None:
    os.makedirs(BUILD_DIR, exist_ok=True)
    objs = []
    for name in SOURCES:
        obj = os.path.join(BUILD_DIR, name.replace(".c", ".o"))
        subprocess.run([CC] + CFLAGS + [os.path.join(SRC_DIR, name),
                                        "-o", obj], check=True)
        objs.append(obj)
    subprocess.run([AR, "rcs", OUT_LIB] + objs, check=True)
    print(f"libtab.a created at {OUT_LIB} ({os.path.getsize(OUT_LIB)} bytes)")

    for source, binary in TESTS:
        obj = os.path.join(BUILD_DIR, source.replace(".c", ".o"))
        subprocess.run([CC] + CFLAGS + [os.path.join(SRC_DIR, source),
                                        "-o", obj], check=True)
        subprocess.run(["ld.lld", *LDFLAGS, obj, OUT_LIB]
                       + [os.path.join(REPO_ROOT, lib) for lib in LIBS]
                       + ["-o", os.path.join(REPO_ROOT, "build", binary)],
                       check=True)
        print(f"built build/{binary}")

    cmd_obj = os.path.join(BUILD_DIR, "tab-cmd.o")
    subprocess.run([CC] + CFLAGS + [os.path.join(REPO_ROOT, "sys/src/cmd/tab.c"),
                                    "-o", cmd_obj], check=True)
    subprocess.run(["ld.lld", *LDFLAGS, cmd_obj, OUT_LIB]
                   + [os.path.join(REPO_ROOT, lib) for lib in LIBS]
                   + ["-o", os.path.join(REPO_ROOT, "build/aspera-tab")],
                   check=True)
    print("built build/aspera-tab")


if __name__ == "__main__":
    main()
