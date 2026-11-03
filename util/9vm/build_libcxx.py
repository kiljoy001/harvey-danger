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
LIBCXXABI = LIBCXX.parent / "libcxxabi"
LIBUNWIND = LIBCXX.parent / "libunwind"
COMPILER_RT = LIBCXX.parent / "compiler-rt" / "lib" / "builtins"
APEX = REPO_ROOT / "apex"
BUILD = REPO_ROOT / "build" / "libcxx"
INCLUDE = BUILD / "include"
INCLUDE_V1 = INCLUDE / "c++" / "v1"
LIBDIR = BUILD / "lib"
OBJDIR = BUILD / "obj"
COMPAT_H = REPO_ROOT / "util" / "libcxx" / "compat.h"
COMPAT_CPP = REPO_ROOT / "util" / "libcxx" / "compat.cpp"
TEST_STL_CPP = REPO_ROOT / "util" / "libcxx" / "test_stl.cpp"
TEST_STL_ELF = BUILD / "test_stl.elf"
TEST_THREADS_CPP = REPO_ROOT / "util" / "libcxx" / "test_threads.cpp"
TEST_THREADS_ELF = BUILD / "test_threads.elf"
TEST_ERRNO_CPP = REPO_ROOT / "util" / "libcxx" / "test_errno.cpp"
TEST_ERRNO_ELF = BUILD / "test_errno.elf"
TEST_STDIO_CPP = REPO_ROOT / "util" / "libcxx" / "test_stdio.cpp"
TEST_STDIO_ELF = BUILD / "test_stdio.elf"
TEST_TIMED_CPP = REPO_ROOT / "util" / "libcxx" / "test_timed.cpp"
TEST_TIMED_ELF = BUILD / "test_timed.elf"
TEST_CXX20_CPP = REPO_ROOT / "util" / "libcxx" / "test_cxx20.cpp"
TEST_CXX20_ELF = BUILD / "test_cxx20.elf"
TEST_CHARCONV_CPP = REPO_ROOT / "util" / "libcxx" / "test_charconv.cpp"
TEST_CHARCONV_ELF = BUILD / "test_charconv.elf"
TEST_DEMANGLE_CPP = REPO_ROOT / "util" / "libcxx" / "test_demangle.cpp"
TEST_DEMANGLE_ELF = BUILD / "test_demangle.elf"
TEST_PRINT_CPP = REPO_ROOT / "util" / "libcxx" / "test_print.cpp"
TEST_PRINT_ELF = BUILD / "test_print.elf"
TEST_FLOCK_CPP = REPO_ROOT / "util" / "libcxx" / "test_flock.cpp"
TEST_FLOCK_ELF = BUILD / "test_flock.elf"

CXX = "clang++"
CC = "clang"
AR = "llvm-ar"
VM = REPO_ROOT / "util" / "9vm" / "9vm"
KERNEL = REPO_ROOT / "build" / "harvey.32bit"
LIBC_A = REPO_ROOT / "build" / "libc.a"
LIBUNWIND_A = LIBDIR / "libunwind.a"

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
    # new_handler is provided by libc++abi's cxa_handlers.cpp in abi mode.
    "new_helpers",
    "verbose_abort",
    "system_error",
    "error_category",
    "chrono",
    "mutex",
    "mutex_destructor",
    "condition_variable",
    "condition_variable_destructor",
    "call_once",
    "atomic",
    "barrier",
    "thread",
    "future",
    "random",
    "shared_mutex",
    "regex",
    "locale",
    "ios",
    "ios.instantiations",
    "print",
    "iostream",
    "fstream",
    "strstream",
    "filesystem/filesystem_clock",
    "filesystem/filesystem_error",
    "filesystem/path",
    "filesystem/directory_entry",
    "filesystem/directory_iterator",
    "filesystem/operations",
    # __divti3 and friends for the 128-bit arithmetic in the filesystem clock.
    "filesystem/int128_builtins",
    "charconv",
    # Ryu tables behind the floating-point to_chars paths.
    "ryu/d2fixed",
    "ryu/d2s",
    "ryu/f2s",
]

CONFIG_SITE = """\
#ifndef _LIBCPP___CONFIG_SITE
#define _LIBCPP___CONFIG_SITE

#define _LIBCPP_ABI_VERSION 1
#define _LIBCPP_ABI_NAMESPACE __1
#define _LIBCPP_ABI_FORCE_ITANIUM 0
#define _LIBCPP_ABI_FORCE_MICROSOFT 0
#define _LIBCPP_HAS_THREADS 1
#define _LIBCPP_HAS_MONOTONIC_CLOCK 1
#define _LIBCPP_HAS_CLOCK_GETTIME 1
#define _LIBCPP_HAS_TERMINAL 0
#define _LIBCPP_HAS_MUSL_LIBC 0
#define _LIBCPP_HAS_THREAD_API_PTHREAD 1
#define _LIBCPP_HAS_THREAD_API_EXTERNAL 0
#define _LIBCPP_HAS_THREAD_API_WIN32 0
#define _LIBCPP_HAS_THREAD_API_C11 0
#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0
#define _LIBCPP_HAS_FILESYSTEM 1
#define _LIBCPP_HAS_RANDOM_DEVICE 1
#define _LIBCPP_HAS_LOCALIZATION 1
#define _LIBCPP_HAS_UNICODE 0
#define _LIBCPP_HAS_WIDE_CHARACTERS 1
#define _LIBCPP_HAS_TIME_ZONE_DATABASE 0
#define _LIBCPP_INSTRUMENTED_WITH_ASAN 0
#define _LIBCPP_HARDENING_MODE_DEFAULT 2
#define _LIBCPP_PSTL_BACKEND_SERIAL 1
#define _LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE 1

#endif
"""


def run(cmd, **kwargs):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kwargs)


def copy_if_changed(src: Path, dst: Path) -> None:
    """Copy only when the bytes differ, so unchanged headers do not get a
    new mtime and force a rebuild of everything that includes them."""
    if dst.is_file() and dst.stat().st_size == src.stat().st_size:
        if dst.read_bytes() == src.read_bytes():
            return
    shutil.copyfile(src, dst)


def prepare():
    if not (LIBCXX / "include" / "__config").is_file():
        sys.exit(f"libcxx source not found at {LIBCXX}")
    for d in (INCLUDE_V1, LIBDIR, OBJDIR):
        d.mkdir(parents=True, exist_ok=True)

    # The libc++ headers are installed into the build tree so ucc does not
    # depend on the LLVM checkout path.
    src_inc = LIBCXX / "include"
    for root, _, files in os.walk(src_inc):
        rel = Path(root).relative_to(src_inc)
        (INCLUDE_V1 / rel).mkdir(parents=True, exist_ok=True)
        for name in files:
            copy_if_changed(Path(root) / name, INCLUDE_V1 / rel / name)
    site = INCLUDE_V1 / "__config_site"
    if not site.is_file() or site.read_text() != CONFIG_SITE:
        site.write_text(CONFIG_SITE)
    copy_if_changed(LIBCXX / "vendor" / "llvm" / "default_assertion_handler.in",
                    INCLUDE_V1 / "__assertion_handler")
    copy_if_changed(COMPAT_H, INCLUDE / "compat.h")
    copy_if_changed(REPO_ROOT / "util" / "libcxx" / "pthread.h",
                    INCLUDE / "pthread.h")
    copy_if_changed(REPO_ROOT / "util" / "libcxx" / "sched.h",
                    INCLUDE / "sched.h")

    # <cxxabi.h> is part of libc++abi, not libc++, but programs that call
    # __cxa_demangle include it from the same place as <cstdio>.
    for name in ("cxxabi.h", "__cxxabi_config.h"):
        copy_if_changed(LIBCXXABI / "include" / name, INCLUDE / name)

    # from_chars_floating_point.h includes "shared/fp_bits.h".  Upstream
    # populates libcxx/src/include/shared from the libc project; do the same
    # so charconv.cpp builds.
    shared = LIBCXX / "src" / "include" / "shared"
    libc_shared = LIBCXX.parent / "libc" / "shared"
    if libc_shared.is_dir():
        shared.mkdir(parents=True, exist_ok=True)
        for name in ("libc_common.h", "fp_bits.h", "str_to_float.h",
                     "str_to_integer.h"):
            if not (shared / name).exists():
                shutil.copyfile(libc_shared / name, shared / name)


def base_flags(library: bool) -> list[str]:
    flags = [
        "-target", "x86_64-unknown-linux-elf",
        "-std=c++23",
        "-nostdinc",
        "-mno-red-zone",
        "-fno-stack-protector",
        "-fexceptions",
        "-frtti",
        "-fno-threadsafe-statics",
        "-O2",
        "-g",
        f"-I{REPO_ROOT / 'util' / 'clang-harvey' / 'target-include'}",
        f"-I{INCLUDE_V1}",
        f"-I{INCLUDE}",
        f"-I{LIBCXX / 'src'}",
        f"-I{LIBCXX.parent / 'libc'}",
        f"-I{APEX / 'include'}",
        f"-I{APEX / 'amd64' / 'include'}",
        "-include", str(INCLUDE / "compat.h"),
        "-D_SUSV2_SOURCE", "-D_POSIX_SOURCE", "-D_LIMITS_EXTENSION",
        "-D_GNU_SOURCE", "-DHARVEY", "-D_WCHAR_T",
    ]
    if library:
        flags += [
            "-D_LIBCPP_BUILDING_LIBRARY",
        ]
    return flags


def c_flags():
    """Flags for the handful of compiler-rt builtins we compile (C)."""
    return [
        "-target", "x86_64-unknown-linux-elf",
        "-std=c11",
        "-nostdinc",
        "-mno-red-zone",
        "-fno-stack-protector",
        "-O2",
        "-g",
        f"-I{REPO_ROOT / 'util' / 'clang-harvey' / 'target-include'}",
        f"-I{LIBCXX.parent / 'libc'}",
        f"-I{APEX / 'include'}",
        f"-I{APEX / 'amd64' / 'include'}",
        "-D_SUSV2_SOURCE", "-D_POSIX_SOURCE", "-D_LIMITS_EXTENSION",
        "-D_GNU_SOURCE", "-DHARVEY",
        "-include", str(INCLUDE / "compat.h"),
    ]


def build_library():
    objects = []
    for name in SOURCES:
        src = LIBCXX / "src" / f"{name}.cpp"
        obj = OBJDIR / f"{name}.o"
        obj.parent.mkdir(parents=True, exist_ok=True)
        extra = []
        if name == "exception":
            # std::exception_ptr lives in libc++abi; selecting this build mode
            # makes exception.cpp use the __cxa_* implementation instead of
            # the "not yet implemented" fallback.
            extra.append("-DLIBCXX_BUILDING_LIBCXXABI")
            extra.append(f"-I{LIBCXXABI / 'include'}")
        run([CXX, *base_flags(True), *extra, "-c", str(src), "-o", str(obj)])
        objects.append(obj)

    compat_obj = OBJDIR / "compat.o"
    run([CXX, *base_flags(False), "-c", str(COMPAT_CPP), "-o", str(compat_obj)])
    objects.append(compat_obj)

    # RTTI support: libc++abi's typeinfo implementation.  APEX's own
    # libcxxabi stub has no typeinfo at all, and __dynamic_cast needs the
    # __class_type_info hierarchy that private_typeinfo.cpp defines.
    cxxabi_flags = [f for f in base_flags(False) if f != "-fno-rtti"]
    cxxabi_flags += ["-frtti", f"-I{LIBCXXABI / 'include'}"]
    for name in ("private_typeinfo", "cxa_aux_runtime", "abort_message"):
        src = LIBCXXABI / "src" / f"{name}.cpp"
        obj = OBJDIR / f"cxxabi_{name}.o"
        run([CXX, *cxxabi_flags, "-c", str(src), "-o", str(obj)])
        objects.append(obj)

    # The exception runtime: libc++abi's __cxa_* implementation and the
    # personality routine, on top of libunwind.  APEX's libcxxabi stub has
    # none of it.
    exc_flags = [f for f in base_flags(False) if f not in ("-fno-rtti",)]
    exc_flags += [
        "-std=c++20",
        "-D_LIBCPP_ENABLE_CXX17_REMOVED_UNEXPECTED_FUNCTIONS",
        f"-I{LIBCXXABI / 'include'}",
        f"-I{LIBCXXABI / 'src'}",
        f"-I{LIBCXX / 'src'}",
        f"-I{LIBUNWIND / 'include'}",
    ]
    # libc++'s exception.cpp already provides std::terminate and friends, so
    # libc++abi's cxa_handlers/cxa_default_handlers would duplicate them.
    for name in ("cxa_exception", "cxa_personality",
                 "cxa_exception_storage", "fallback_malloc",
                 "stdlib_exception", "cxa_handlers", "cxa_default_handlers",
                 "cxa_demangle"):
        src = LIBCXXABI / "src" / f"{name}.cpp"
        obj = OBJDIR / f"cxa_{name}.o"
        run([CXX, *exc_flags, "-c", str(src), "-o", str(obj)])
        objects.append(obj)

    # 128-bit division, used by std::filesystem's clock conversion.  These are
    # compiler-rt builtins; Harvey has no compiler-rt runtime otherwise.
    for name in ("divti3", "udivti3", "modti3", "umodti3", "udivmodti4"):
        src = COMPILER_RT / f"{name}.c"
        obj = OBJDIR / f"crt_{name}.o"
        run([CC, *c_flags(), f"-I{COMPILER_RT}", "-c", str(src), "-o", str(obj)])
        objects.append(obj)


    libm_obj = OBJDIR / "libm_stub.o"
    run([CXX, *base_flags(False), "-c",
         str(REPO_ROOT / "util" / "libcxx" / "libm_stub.cpp"), "-o", str(libm_obj)])
    objects.append(libm_obj)

    locale_obj = OBJDIR / "locale_stub.o"
    run([CXX, *base_flags(False), "-c",
         str(REPO_ROOT / "util" / "libcxx" / "locale_stub.cpp"), "-o", str(locale_obj)])
    objects.append(locale_obj)

    pthread_obj = OBJDIR / "pthread.o"
    run([CXX, *base_flags(False), "-c",
         str(REPO_ROOT / "util" / "libcxx" / "pthread.cpp"), "-o", str(pthread_obj)])
    objects.append(pthread_obj)

    lib = LIBDIR / "libc++.a"
    try:
        lib.unlink()
    except FileNotFoundError:
        pass
    run([AR, "rcs", str(lib), *map(str, objects)])
    print(f"built {lib} ({lib.stat().st_size} bytes)")


def build_test(src_var, elf_var, name):
    test_obj = OBJDIR / f"{name}.o"
    run([CXX, *base_flags(False), "-c", str(src_var), "-o", str(test_obj)])

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
        str(LIBDIR / "libunwind.a"),
        str(APEX / "amd64" / "lib" / "libcxxabi.a"),
        str(APEX / "amd64" / "lib" / "libap.a"),
        str(LIBC_A),
        str(APEX / "amd64" / "lib" / "crtn.o"),
        "-o", str(elf_var),
    ]
    run(cmd)
    print(f"built {elf_var} ({elf_var.stat().st_size} bytes)")


def run_test(elf, expect):
    if not VM.is_file() or not KERNEL.is_file():
        sys.exit(f"need {VM} and {KERNEL} to run the smoke test")
    # 1024 MB (9vm's default) intermittently corrupts memory: about one
    # boot in eight dies with an impossible failure (clock_gettime()
    # rejecting a valid buffer, or a write to the text base).  3 GiB is
    # reliable and is what the builds use.
    print(f"+ {VM} -m 3072 -boot {elf} {KERNEL}")
    proc = subprocess.run(
        [str(VM), "-m", "3072", "-boot", str(elf), str(KERNEL)],
        cwd=REPO_ROOT, text=True, capture_output=True, timeout=20)
    output = proc.stdout + proc.stderr
    for line in output.splitlines():
        if line.strip():
            print("  " + line)
    if expect not in output:
        sys.exit(f"smoke test {elf.name} did not print {expect!r}")
    if proc.returncode != 0:
        sys.exit(f"9vm exited {proc.returncode}")
    print(f"smoke test {elf.name} passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--run", action="store_true")
    args = ap.parse_args()

    if args.clean:
        shutil.rmtree(BUILD, ignore_errors=True)
        print(f"removed {BUILD}")
        return

    # libc++ now references _Unwind_*, so the unwinder has to be built first.
    run([sys.executable, str(REPO_ROOT / "util" / "9vm" / "build_libunwind.py")])

    prepare()
    build_library()
    build_test(TEST_STL_CPP, TEST_STL_ELF, "test_stl")
    build_test(TEST_THREADS_CPP, TEST_THREADS_ELF, "test_threads")
    build_test(TEST_ERRNO_CPP, TEST_ERRNO_ELF, "test_errno")
    build_test(TEST_STDIO_CPP, TEST_STDIO_ELF, "test_stdio")
    build_test(TEST_TIMED_CPP, TEST_TIMED_ELF, "test_timed")
    build_test(TEST_CXX20_CPP, TEST_CXX20_ELF, "test_cxx20")
    build_test(TEST_CHARCONV_CPP, TEST_CHARCONV_ELF, "test_charconv")
    build_test(TEST_DEMANGLE_CPP, TEST_DEMANGLE_ELF, "test_demangle")
    build_test(TEST_PRINT_CPP, TEST_PRINT_ELF, "test_print")
    build_test(TEST_FLOCK_CPP, TEST_FLOCK_ELF, "test_flock")
    if args.run:
        run_test(TEST_STL_ELF, "hello 2 2 42 42")
        run_test(TEST_THREADS_ELF, "threads-ok 7 14")
        run_test(TEST_ERRNO_ELF, "errno-ok main=33 a=11/2 b=22/2")
        run_test(TEST_STDIO_ELF, "stdio-ok lines=800 bad=0 seen=800")
        run_test(TEST_TIMED_ELF, "timed sleep=1 timeout=1 signal=1 future=1")
        run_test(TEST_CXX20_ELF, "cxx20 latch=1 barrier=1 sema=1 atomic=1 jthread=1")
        run_test(TEST_CHARCONV_ELF,
                 "charconv float=3.14159 parsed=3.14159 err=0 int=12345/12345 "
                 "format=42 harvey 3.14")
        run_test(TEST_DEMANGLE_ELF, "demangle status=0 name=foo(int)")
        run_test(TEST_PRINT_ELF, "print 42 harvey")
        run_test(TEST_FLOCK_ELF, "flock busy=1")


if __name__ == "__main__":
    main()
