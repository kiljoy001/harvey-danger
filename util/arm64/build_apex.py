#!/usr/bin/env python3
"""Cross-build the APEX runtime layer for ARM64 and link a test program.

This is the ARM64 answer to the x86 APEX build in apex/src: the same
portable C sources, with an aarch64 crt0, an aarch64 stack switch, and the
variant-I TLS block in __libc_start_main().  It links against the ARM64
Plan 9 libc built by build_libc.py.

    python3 util/arm64/build_apex.py [--keep]

The output is build/arm64-apex/hello.elf (entry _start).  Add --keep to
keep the objects for inspection.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/arm64-apex"
APEX = ROOT / "apex"
LIBC = ROOT / "build/arm64-libc/libc.a"
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM64_LD", "ld.lld")
LLVM_PROJECT = Path(os.environ.get("LLVM_PROJECT",
                                  ROOT.parent / "llvm-project"))

# AArch64 long double is binary128, so the compiler emits calls to the
# soft-float quad routines.  There is no prebuilt compiler-rt for
# aarch64-unknown-linux-elf on this host, so build the handful of builtins the
# runtime needs from LLVM's own source.
BUILTINS = ["addtf3.c", "subtf3.c", "multf3.c", "divtf3.c",
            "comparetf2.c", "extenddftf2.c", "extendsftf2.c",
            "trunctfdf2.c", "trunctfsf2.c", "fixtfsi.c",
            "fixunstfsi.c", "floatsitf.c", "floatunsitf.c",
            "aarch64/fp_mode.c"]

HELLO = r"""
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

__thread long tls_counter = 11;

int
main(int argc, char **argv)
{
	/* The kernel starts the first process with no open descriptors. */
	open("#c/cons", 0);
	open("#c/cons", 1);
	open("#c/cons", 1);
	printf("apex-hello argc=%d tls=%ld env=%s\n", argc, tls_counter,
	    getenv("PATH") == 0 ? "(none)" : "set");
	if(tls_counter == 11)
		printf("TLS-OK\n");
	return 0;
}
"""


# The same set of directories the amd64 Makefile builds.  nan.c pokes at
# the x86 FPdbleword union and malloc.c shadows the Plan 9 allocator, so
# both are handled separately.
DIRS = ["bsd", "crt", "dirent", "gen", "locale", "math", "multibyte",
        "plan9", "posix", "stdio", "stdlib", "string"]
SKIP = {"nan.c"}


def sources() -> list[Path]:
    out = []
    for name in DIRS:
        for p in sorted((APEX / "src/ap" / name).glob("*.c")):
            if p.name not in SKIP:
                out.append(p)
    out.extend(sorted((APEX / "src/ap/aarch64").glob("*.c")))
    return out


def flags() -> list[str]:
    return [
        "--target=aarch64-unknown-linux-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-mstrict-align", "-fcommon", "-nostdlibinc",
        "-O2", "-g", "-funwind-tables", "-D_SUSV2_SOURCE", "-D_POSIX_SOURCE",
        "-D_LIMITS_EXTENSION", "-D_GNU_SOURCE", "-DHARVEY", "-D_PLAN9_SOURCE",
        "-D_BSD_EXTENSION", "-DIEEE_8087",
        "-I", str(APEX / "include"),
        "-I", str(APEX / "aarch64/include"),
        "-I", str(APEX / "src/ap/internal"),
    ]


LINK_SCRIPT = """\
ENTRY(_start)
PHDRS {
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
  tls PT_TLS FLAGS(4);
}
SECTIONS {
  /* Map the ELF header itself: __libc_start_main reads the program headers
   * through __ehdr_start to find PT_TLS. */
  . = 0x200000 + SIZEOF_HEADERS;
  .text : { *(.text .text.*) *(.rodata .rodata.*) } :text
  . = ALIGN(0x200000);
  .data : { *(.data .data.*) *(.data.rel.ro*) } :data
  .tdata : { *(.tdata .tdata.*) } :data :tls
  .tbss : { *(.tbss .tbss.*) } :data :tls
  .bss : { *(.bss .bss.* COMMON) } :data
  PROVIDE(end = .);
  /* __libc_start_main cannot read the ELF header on ARM64 (the first
   * PT_LOAD starts at its file offset), so the TLS segment is recorded
   * here instead of parsed at run time. */
  __tls_vaddr = ADDR(.tdata);
  __tls_filesz = SIZEOF(.tdata);
  __tls_memsz = SIZEOF(.tdata) + SIZEOF(.tbss);
  __tls_align = 16;
  /DISCARD/ : { *(.comment) *(.note*) }
}
"""


def run(cmd: list[str]) -> None:
    subprocess.run(cmd, check=True)


def main() -> None:
    keep = "--keep" in sys.argv[1:]
    if not LIBC.is_file():
        sys.exit(f"missing {LIBC}; run util/arm64/build_libc.py first")

    shutil.rmtree(OUT, ignore_errors=True)
    (OUT / "obj").mkdir(parents=True)
    (OUT / "hello.c").write_text(HELLO)
    (OUT / "hello.ld").write_text((ROOT / "util" / "arm64" / "apex.ld").read_text())

    objects = []
    for index, source in enumerate(sources()):
        obj = OUT / "obj" / f"{index:02d}-{source.stem}.o"
        run([CC, *flags(), "-c", str(source), "-o", str(obj)])
        objects.append(obj)

    # Archive the layer, as the x86 build does: an archive only pulls the
    # members something actually references, so the x86 assembly pieces
    # that have no aarch64 port yet stay out of the link.
    libap = OUT / "libap.a"
    run(["llvm-ar", "rcs", str(libap), *[str(o) for o in objects]])
    hello = OUT / "obj/hello.o"
    run([CC, *flags(), "-c", str(OUT / "hello.c"), "-o", str(hello)])

    builtins = OUT / "libbuiltins.a"
    rtobjs = []
    builtins_src = LLVM_PROJECT / "compiler-rt/lib/builtins"
    for name in BUILTINS:
        obj = OUT / "obj" / ("rt-" + name.replace("/", "-").replace(".c", ".o"))
        run([CC, *flags(), "-I", str(builtins_src), "-c",
             str(builtins_src / name), "-o", str(obj)])
        rtobjs.append(str(obj))
    run(["llvm-ar", "rcs", str(builtins), *rtobjs])

    elf = OUT / "hello.elf"
    # The kernel's ELF loader maps each PT_LOAD at p_align and rejects
    # overlaps, so keep segment alignment at the 4 KiB page size.
    run([LD, "-z", "max-page-size=4096", "-T", str(OUT / "hello.ld"),
         "-e", "_start", "-u", "_start",
         str(hello), str(libap), str(LIBC), str(builtins), "-o", str(elf)])
    print(f"ARM64 APEX link PASS: {elf} ({elf.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
