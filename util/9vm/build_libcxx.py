#!/usr/bin/env python3
"""Build a minimal libc++ for Harvey/APEX and a smoke test.

This is the first rung of the C++ ladder: headers, libc++ containers, and
aligned allocation, with exceptions/RTTI/threads/locales/filesystem disabled.
It is deliberately small and reproducible; it is not a full libc++ port yet.

Usage:
    util/9vm/build_libcxx.py            # build build/libcxx/lib/libc++.a + test
    util/9vm/build_libcxx.py --run      # also boot it under 9vm
    util/9vm/build_libcxx.py --clean

Environment:
    LIBCXX_SRC   libcxx source tree (default ../llvm-project/libcxx)
    HARVEY       Harvey repo root (default two levels up)
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(os.environ.get(
    "HARVEY", Path(__file__).resolve().parents[2])).resolve()
LIBCXX = Path(os.environ.get(
    "LIBCXX_SRC", REPO_ROOT.parent / "llvm-project" / "libcxx")).resolve()
APEX = REPO_ROOT / "apex"
BUILD = REPO_ROOT / "build" / "libcxx"
INCLUDE = BUILD / "include"
INCLUDE_V1 = INCLUDE / "c++" / "v1"
LIBDIR = BUILD / "lib"
OBJDIR = BUILD / "obj"
COMPAT_H = REPO_ROOT / "util" / "libcxx" / "compat.h"
COMPAT_CPP = REPO_ROOT / "util" / "libcxx" / "compat.cpp"
TEST_CPP = REPO_ROOT / "util" / "libcxx" / "test_stl.cpp"
TEST_ELF = BUILD / "test_stl.elf"

CXX = "clang++"
AR = "llvm-ar"
VM = REPO_ROOT / "util" / "9vm" / "9vm"
KERNEL = REPO_ROOT / "build" / "harvey.32bit"
LIBC_A = REPO_ROOT / "build" / "libc.a"

# libc++ sources that make std::string/vector/map and their support work.
# Deliberately excluded for now:
#   charconv  - this libc++ snapshot's src/include/shared/fp_bits.h is missing
#   thread/mutex/condition_variable - APEX has no pthread layer yet
#   locale/iostream/filesystem - disabled in __config_site
SOURCES = [
    "string",
    "stdexcept",
    "exception",
    "typeinfo",
    "functional",
    "memory",
    "hash",
    "algorithm",
    "any",
    "optional",
    "variant",
    "new_handler",
    "new_helpers",
    "verbose_abort",
]

CONFIG_SITE = """\
#ifndef _LIBCPP___CONFIG_SITE
#define _LIBCPP___CONFIG_SITE

#define _LIBCPP_ABI_VERSION 1
#define _LIBCPP_ABI_NAMESPACE __1
#define _LIBCPP_ABI_FORCE_ITANIUM 0
#define _LIBCPP_ABI_FORCE_MICROSOFT 0
#define _LIBCPP_HAS_THREADS 0
#define _LIBCPP_HAS_MONOTONIC_CLOCK 0
#define _LIBCPP_HAS_TERMINAL 0
#define _LIBCPP_HAS_MUSL_LIBC 0
#define _LIBCPP_HAS_THREAD_API_PTHREAD 0
#define _LIBCPP_HAS_THREAD_API_EXTERNAL 0
#define _LIBCPP_HAS_THREAD_API_WIN32 0
#define _LIBCPP_HAS_THREAD_API_C11 0
#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0
#define _LIBCPP_HAS_FILESYSTEM 0
#define _LIBCPP_HAS_RANDOM_DEVICE 0
#define _LIBCPP_HAS_LOCALIZATION 0
#define _LIBCPP_HAS_UNICODE 0
#define _LIBCPP_HAS_WIDE_CHARACTERS 0
#define _LIBCPP_HAS_TIME_ZONE_DATABASE 0
#define _LIBCPP_INSTRUMENTED_WITH_ASAN 0
#define _LIBCPP_HARDENING_MODE_DEFAULT 2
#define _LIBCPP_PSTL_BACKEND_SERIAL 1

#endif
"""


def run(cmd, **kwargs):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kwargs)


def prepare():
    if not (LIBCXX / "include" / "__config").is_file():
        sys.exit(f"libcxx source not found at {LIBCXX}")
    for d in (INCLUDE_V1, LIBDIR, OBJDIR):
        d.mkdir(parents=True, exist_ok=True)

    # The libc++ headers are installed into the build tree so ucc does not
    # depend on the LLVM checkout path.
    shutil.copytree(LIBCXX / "include", INCLUDE_V1, dirs_exist_ok=True)
    (INCLUDE_V1 / "__config_site").write_text(CONFIG_SITE)
    shutil.copyfile(LIBCXX / "vendor" / "llvm" / "default_assertion_handler.in",
                    INCLUDE_V1 / "__assertion_handler")
    shutil.copyfile(COMPAT_H, INCLUDE / "compat.h")


def base_flags(library: bool) -> list[str]:
    flags = [
        "-target", "x86_64-unknown-linux-elf",
        "-std=c++20",
        "-nostdinc",
        "-mno-red-zone",
        "-fno-stack-protector",
        "-fno-exceptions",
        "-fno-rtti",
        "-fno-threadsafe-statics",
        "-O2",
        "-g",
        f"-I{INCLUDE_V1}",
        f"-I{INCLUDE}",
        f"-I{LIBCXX / 'src'}",
        f"-I{APEX / 'include'}",
        f"-I{APEX / 'amd64' / 'include'}",
        "-include", str(INCLUDE / "compat.h"),
        "-D_SUSV2_SOURCE", "-D_POSIX_SOURCE", "-D_LIMITS_EXTENSION",
        "-DHARVEY", "-D_WCHAR_T",
    ]
    if library:
        flags += [
            "-D_LIBCPP_BUILDING_LIBRARY",
            "-D_LIBCPP_HAS_NO_THREADS",
            "-D_LIBCPP_HAS_NO_EXCEPTIONS",
            "-D_LIBCPP_HAS_NO_RTTI",
        ]
    return flags


def build_library():
    objects = []
    for name in SOURCES:
        src = LIBCXX / "src" / f"{name}.cpp"
        obj = OBJDIR / f"{name}.o"
        run([CXX, *base_flags(True), "-c", str(src), "-o", str(obj)])
        objects.append(obj)

    compat_obj = OBJDIR / "compat.o"
    run([CXX, *base_flags(False), "-c", str(COMPAT_CPP), "-o", str(compat_obj)])
    objects.append(compat_obj)

    lib = LIBDIR / "libc++.a"
    try:
        lib.unlink()
    except FileNotFoundError:
        pass
    run([AR, "rcs", str(lib), *map(str, objects)])
    print(f"built {lib} ({lib.stat().st_size} bytes)")


def build_test():
    test_obj = OBJDIR / "test_stl.o"
    run([CXX, *base_flags(False), "-c", str(TEST_CPP), "-o", str(test_obj)])

    cmd = [
        CXX,
        "-target", "x86_64-unknown-linux-elf",
        "-fuse-ld=lld",
        "-static",
        "-nostdlib",
        "-Wl,-z,max-page-size=0x200000",
        str(APEX / "amd64" / "lib" / "crt0.o"),
        str(APEX / "amd64" / "lib" / "crti.o"),
        str(test_obj),
        str(LIBDIR / "libc++.a"),
        str(APEX / "amd64" / "lib" / "libcxxabi.a"),
        str(APEX / "amd64" / "lib" / "libap.a"),
        str(LIBC_A),
        str(APEX / "amd64" / "lib" / "crtn.o"),
        "-o", str(TEST_ELF),
    ]
    run(cmd)
    print(f"built {TEST_ELF} ({TEST_ELF.stat().st_size} bytes)")


def run_test():
    if not VM.is_file() or not KERNEL.is_file():
        sys.exit(f"need {VM} and {KERNEL} to run the smoke test")
    print(f"+ {VM} -boot {TEST_ELF} {KERNEL}")
    proc = subprocess.run(
        [str(VM), "-boot", str(TEST_ELF), str(KERNEL)],
        cwd=REPO_ROOT, text=True, capture_output=True, timeout=20)
    output = proc.stdout + proc.stderr
    for line in output.splitlines():
        if line.strip():
            print("  " + line)
    if "hello 2 2 42 42" not in output:
        sys.exit("libc++ smoke test did not print 'hello 2 2 42 42'")
    if proc.returncode != 0:
        sys.exit(f"9vm exited {proc.returncode}")
    print("libc++ smoke test passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--run", action="store_true")
    args = ap.parse_args()

    if args.clean:
        shutil.rmtree(BUILD, ignore_errors=True)
        print(f"removed {BUILD}")
        return

    prepare()
    build_library()
    build_test()
    if args.run:
        run_test()


if __name__ == "__main__":
    main()
