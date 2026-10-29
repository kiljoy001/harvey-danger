#!/usr/bin/env python3
"""Build Monocypher 4.0.2 for Harvey and its known-answer self-test.

Monocypher is vendored as sys/src/libmonocypher/monocypher.c with the
public header in sys/include/monocypher.h.  It is compiled in its
MONO_PLAN9 mode, which takes uint8_t/uint32_t/uint64_t/size_t from
<u.h>/<libc.h> instead of stdint.h.  The library has no libc
dependencies of its own.
"""
import os
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
SRC_DIR = os.path.join(REPO_ROOT, "sys/src/libmonocypher")
BUILD_DIR = os.path.join(REPO_ROOT, "build/libmonocypher")
OUT_LIB = os.path.join(REPO_ROOT, "build/libmonocypher.a")

CC = "clang"
AR = "llvm-ar"

CFLAGS = [
    "-c", "-O2",
    "-mno-red-zone", "-mno-implicit-float",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-mcmodel=small", "-fcommon",
    "-Wno-unknown-pragmas",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
    "-DMONO_PLAN9",
]

LDFLAGS = ["-e_main", "-static", "-z", "noseparate-code",
           "-z", "max-page-size=0x200000"]


def main() -> None:
    os.makedirs(BUILD_DIR, exist_ok=True)
    obj = os.path.join(BUILD_DIR, "monocypher.o")
    subprocess.run([CC] + CFLAGS + [os.path.join(SRC_DIR, "monocypher.c"),
                                    "-o", obj], check=True)
    subprocess.run([AR, "rcs", OUT_LIB, obj], check=True)
    print(f"libmonocypher.a created at {OUT_LIB} ({os.path.getsize(OUT_LIB)} bytes)")

    test_obj = os.path.join(BUILD_DIR, "mctest.o")
    subprocess.run([CC] + CFLAGS + [os.path.join(SRC_DIR, "mctest.c"),
                                    "-o", test_obj], check=True)
    subprocess.run(["ld.lld", *LDFLAGS, test_obj, OUT_LIB,
                    os.path.join(REPO_ROOT, "build/libc.a"),
                    "-o", os.path.join(REPO_ROOT, "build/aspera-mctest")],
                   check=True)
    print("built build/aspera-mctest")


if __name__ == "__main__":
    main()
