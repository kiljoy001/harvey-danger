#!/usr/bin/env python3
"""Cross-build libc++ for ARM64 Harvey.

Mirrors 9vm/build_libcxx.py: the same source list, the same
__config_site, and the same support objects (compat, libm/locale stubs,
the pthread layer, libc++abi's typeinfo and exception runtime).  Only the
target and the platform include roots change, plus the ARM64 APEX startup
layer that programs link against.

    util/arm64/build_libcxx.py
    util/arm64/build_libcxx.py --test

--test also builds and boots a small program that uses std::string,
std::vector, exceptions and a thread.
"""
from pathlib import Path
import importlib.util
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "arm64-libcxx"
INCLUDE_V1 = OUT / "include" / "c++" / "v1"
INCLUDE = OUT / "include"
LIBDIR = OUT / "lib"
OBJDIR = OUT / "obj"
APEX = ROOT / "apex"
APEX_LIB = ROOT / "build" / "arm64-apex"
LIBC_A = ROOT / "build" / "arm64-libc" / "libc.a"
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM64_LD", "ld.lld")
LLVM = Path(os.environ.get("LLVM_PROJECT",
                           ROOT.parent / "llvm-project")).resolve()
LIBCXX = LLVM / "libcxx"
LIBCXXABI = LLVM / "libcxxabi"
LIBUNWIND = LLVM / "libunwind"
COMPILER_RT = LLVM / "compiler-rt" / "lib" / "builtins"

x86 = importlib.util.module_from_spec(importlib.util.spec_from_file_location(
    "hx", ROOT.parent / "9vm" / "build_libcxx.py"))
importlib.util.spec_from_file_location(
    "hx", ROOT.parent / "9vm" / "build_libcxx.py").loader.exec_module(x86)
SOURCES = list(x86.SOURCES)
# std::placeholders lives in bind.cpp; the x86 list predates it and does not
# include it, which leaves std::bind users with an undefined _1.
if "bind" not in SOURCES:
    SOURCES.append("bind")
CONFIG_SITE = x86.CONFIG_SITE


def run(cmd: list[str]) -> None:
    print("+", " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True)


def copy_if_changed(src: Path, dst: Path) -> None:
    if dst.is_file() and dst.stat().st_size == src.stat().st_size:
        if dst.read_bytes() == src.read_bytes():
            return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)


def prepare() -> None:
    if not (LIBCXX / "include" / "__config").is_file():
        sys.exit(f"libcxx source not found at {LIBCXX}")
    for d in (INCLUDE_V1, LIBDIR, OBJDIR):
        d.mkdir(parents=True, exist_ok=True)

    src_inc = LIBCXX / "include"
    for root, _, files in os.walk(src_inc):
        rel = Path(root).relative_to(src_inc)
        (INCLUDE_V1 / rel).mkdir(parents=True, exist_ok=True)
        for name in files:
            copy_if_changed(Path(root) / name, INCLUDE_V1 / rel / name)
    site = INCLUDE_V1 / "__config_site"
    if not site.is_file() or site.read_text() != CONFIG_SITE:
        site.write_text(CONFIG_SITE)
    copy_if_changed(LIBCXX / "vendor" / "llvm" /
                    "default_assertion_handler.in",
                    INCLUDE_V1 / "__assertion_handler")
    copy_if_changed(ROOT / "util" / "libcxx" / "compat.h",
                    INCLUDE / "compat.h")
    copy_if_changed(ROOT / "util" / "libcxx" / "pthread.h",
                    INCLUDE / "pthread.h")
    copy_if_changed(ROOT / "util" / "libcxx" / "sched.h",
                    INCLUDE / "sched.h")
    for name in ("cxxabi.h", "__cxxabi_config.h"):
        copy_if_changed(LIBCXXABI / "include" / name, INCLUDE / name)

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
        "--target=aarch64-unknown-linux-elf",
        "-std=c++23",
        "-nostdlibinc",
        "-mstrict-align",
        "-fno-stack-protector",
        "-fexceptions",
        "-frtti",
        "-fno-threadsafe-statics",
        "-O2",
        "-g",
        f"-I{ROOT / 'util' / 'clang-harvey' / 'target-include'}",
        f"-I{INCLUDE_V1}",
        f"-I{INCLUDE}",
        f"-I{LIBCXX / 'src'}",
        f"-I{LIBCXX.parent / 'libc'}",
        f"-I{APEX / 'include'}",
        f"-I{APEX / 'aarch64' / 'include'}",
        f"-I{LIBUNWIND / 'include'}",
        "-include", str(INCLUDE / "compat.h"),
        "-D_SUSV2_SOURCE", "-D_POSIX_SOURCE", "-D_LIMITS_EXTENSION",
        "-D_GNU_SOURCE", "-DHARVEY", "-D_WCHAR_T",
    ]
    if library:
        flags.append("-D_LIBCPP_BUILDING_LIBRARY")
    return flags


def c_flags() -> list[str]:
    return [
        "--target=aarch64-unknown-linux-elf", "-std=c11", "-nostdlibinc",
        "-mstrict-align", "-fno-stack-protector", "-O2", "-g",
        f"-I{ROOT / 'util' / 'clang-harvey' / 'target-include'}",
        f"-I{LIBCXX.parent / 'libc'}",
        f"-I{APEX / 'include'}",
        f"-I{APEX / 'aarch64' / 'include'}",
        "-D_SUSV2_SOURCE", "-D_POSIX_SOURCE", "-D_LIMITS_EXTENSION",
        "-D_GNU_SOURCE", "-DHARVEY",
        "-include", str(INCLUDE / "compat.h"),
    ]


def build_library() -> None:
    objects = []
    for name in SOURCES:
        src = LIBCXX / "src" / f"{name}.cpp"
        obj = OBJDIR / f"{name}.o"
        obj.parent.mkdir(parents=True, exist_ok=True)
        extra = []
        if name == "exception":
            extra += ["-DLIBCXX_BUILDING_LIBCXXABI",
                      f"-I{LIBCXXABI / 'include'}"]
        run([CC, *base_flags(True), *extra, "-c", str(src), "-o", str(obj)])
        objects.append(obj)

    # libm_stub.cpp reaches into the x87 stack, which aarch64 does not have;
    # APEX's math library provides the long-double entry points there.
    for name, src in (
            ("compat", ROOT / "util" / "libcxx" / "compat.cpp"),
            ("locale_stub", ROOT / "util" / "libcxx" / "locale_stub.cpp"),
            ("cxa_atexit", ROOT / "util" / "libcxx" / "cxa_atexit.cpp"),
            ("pthread", ROOT / "util" / "libcxx" / "pthread.cpp")):
        obj = OBJDIR / f"{name}.o"
        run([CC, *base_flags(False), "-c", str(src), "-o", str(obj)])
        objects.append(obj)

    cxxabi_flags = base_flags(False) + [f"-I{LIBCXXABI / 'include'}"]
    for name in ("private_typeinfo", "cxa_aux_runtime", "abort_message",
                 "cxa_virtual", "stdlib_new_delete", "cxa_guard"):
        src = LIBCXXABI / "src" / f"{name}.cpp"
        obj = OBJDIR / f"cxxabi_{name}.o"
        run([CC, *cxxabi_flags, "-c", str(src), "-o", str(obj)])
        objects.append(obj)

    exc_flags = base_flags(False) + [
        "-std=c++20",
        "-D_LIBCPP_ENABLE_CXX17_REMOVED_UNEXPECTED_FUNCTIONS",
        f"-I{LIBCXXABI / 'include'}",
        f"-I{LIBCXXABI / 'src'}",
        f"-I{LIBCXX / 'src'}",
        f"-I{LIBUNWIND / 'include'}",
    ]
    for name in ("cxa_exception", "cxa_personality",
                 "cxa_exception_storage", "fallback_malloc",
                 "stdlib_exception", "cxa_handlers", "cxa_default_handlers",
                 "cxa_demangle"):
        src = LIBCXXABI / "src" / f"{name}.cpp"
        obj = OBJDIR / f"cxa_{name}.o"
        run([CC, *exc_flags, "-c", str(src), "-o", str(obj)])
        objects.append(obj)

    for name in ("divti3", "udivti3", "modti3", "umodti3", "udivmodti4"):
        src = COMPILER_RT / f"{name}.c"
        obj = OBJDIR / f"crt_{name}.o"
        run([CC, *c_flags(), f"-I{COMPILER_RT}", "-c", str(src), "-o",
             str(obj)])
        objects.append(obj)

    lib = LIBDIR / "libc++.a"
    lib.unlink(missing_ok=True)
    run(["llvm-ar", "rcs", str(lib), *map(str, objects)])
    print(f"built {lib} ({lib.stat().st_size} bytes)")


TEST = r"""
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <thread>
#include <mutex>

extern "C" long write(int, const void *, unsigned long);

static void step(const char *s)
{
	write(1, s, strlen(s));
}

__thread int tlsv = 7;

int
main()
{
	/* The kernel starts the first process with no open descriptors. */
	open("#c/cons", 0);
	open("#c/cons", 1);
	open("#c/cons", 1);
	step("S1 ");
	std::printf("printf\n");
	step("S2 ");
	std::string s = "harvey";
	s += " arm64";
	step("S3 ");
	std::vector<int> v{1, 2, 3};
	step("S4 ");

	int int_ok = 0;
	try {
		throw 42;
	} catch (int v) {
		int_ok = (v == 42);
	}
	step("S5 ");
	std::string what;
	try {
		throw std::runtime_error("boom");
	} catch (const std::exception &e) {
		what = e.what();
	}
	step("S6 ");

	long shared = 0;
	std::mutex m;
	std::thread t([&] {
		std::lock_guard<std::mutex> l(m);
		shared = 42;
	});
	t.join();
	step("S7 ");
	std::printf("cxx-libcxx %s sum=%d tls=%d shared=%ld int=%d what=%s\n",
	    s.c_str(), v[0] + v[1] + v[2], tlsv, shared, int_ok, what.c_str());
	if (s == "harvey arm64" && v[0] + v[1] + v[2] == 6 && tlsv == 7 &&
	    shared == 42 && int_ok && what == "boom")
		std::printf("LIBCXX-ARM64-OK\n");
	return 0;
}
"""

LINK_SCRIPT = """\
ENTRY(_start)
PHDRS {
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
  tls PT_TLS FLAGS(4);
  eh_frame_hdr PT_GNU_EH_FRAME FLAGS(4);
}
SECTIONS {
  /* Above the kernel identity map; see UTZERO in
   * sys/src/9/aarch64/mem.h. */
  . = 0x400000000 + SIZEOF_HEADERS;
  .text : { *(.text .text.*) *(.rodata .rodata.*) } :text
  .eh_frame_hdr : { *(.eh_frame_hdr) } :text :eh_frame_hdr
  . = ALIGN(0x200000);
  .data : { *(.data .data.*) *(.data.rel.ro*) } :data
  .tdata : { *(.tdata .tdata.*) } :data :tls
  .tbss : { *(.tbss .tbss.*) } :data :tls
  .bss : { *(.bss .bss.* COMMON) } :data
  PROVIDE(end = .);
  __tls_vaddr = ADDR(.tdata);
  __tls_filesz = SIZEOF(.tdata);
  __tls_memsz = SIZEOF(.tdata) + SIZEOF(.tbss);
  __tls_align = 16;
  /* dl_iterate_phdr cannot read the ELF header on ARM64, so record the
   * segments libunwind needs. */
  __harvey_text_start = ADDR(.text);
  __harvey_text_size = SIZEOF(.text);
  __harvey_eh_frame_hdr = ADDR(.eh_frame_hdr);
  __harvey_eh_frame_hdr_size = SIZEOF(.eh_frame_hdr);
  /DISCARD/ : { *(.comment) *(.note*) }
}
"""


def build_test() -> Path:
    src = OUT / "test.cpp"
    src.write_text(TEST)
    link = OUT / "test.ld"
    link.write_text((ROOT / "util" / "arm64" / "apex.ld").read_text())
    obj = OUT / "obj" / "test.o"
    run([CC, *base_flags(False), "-c", str(src), "-o", str(obj)])
    elf = OUT / "libcxx-test.elf"
    run([LD, "-z", "max-page-size=4096", "--eh-frame-hdr", "-T", str(link),
         "-e", "_start",
         "-u", "_start", str(obj), str(LIBDIR / "libc++.a"),
         str(LIBDIR / "libunwind.a"), str(APEX_LIB / "libap.a"),
         str(LIBC_A), str(APEX_LIB / "libbuiltins.a"), "-o", str(elf)])
    print(f"built {elf} ({elf.stat().st_size} bytes)")
    return elf


def main() -> None:
    prepare()
    subprocess.run([sys.executable, str(ROOT / "util" / "arm64" /
                                       "build_libunwind.py")], check=True)
    build_library()
    if "--test-only" in sys.argv[1:]:
        elf = build_test()
        print(f"test program: {elf}")
        return
    if "--test" in sys.argv[1:]:
        elf = build_test()
        print(f"test program: {elf}")


if __name__ == "__main__":
    main()
