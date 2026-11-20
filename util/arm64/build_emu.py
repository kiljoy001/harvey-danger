#!/usr/bin/env python3
"""Build the Dis runner (`disvm`) as a Harvey/ARM64 user program.

Sources come from two trees:

  * purgatorio/libinterp - the Dis interpreter (unchanged)
  * purgatorio/emu/port  - the kernel services the interpreter needs
  * util/arm64/emu       - the Harvey host: sysfile9.c (the Sys module),
                           disvm.c (entry point), asm-arm64.S

Inferno's own Chan/Dev layer is deliberately not built: sysfile9.c
implements the kernel entry points the Sys module calls directly on
Harvey's libc.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PURG = Path(os.environ.get("PURGATORIO", ROOT.parent / "purgatorio"))
OUT = Path(os.environ.get("ARM64_EMU_OUT", ROOT / "build/arm64-emu"))
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM64_LD", "ld.lld")
LIBC = ROOT / "build/arm64-libc/libc.a"

BASE = 0xCE00000
BIG_PAGE = 0x10000

# The interpreter proper: everything except the JIT back ends and the
# platform dynload stubs.
LIBINTERP = """
alt conv dec gc geom heap link load loader math raise readmod
runt stack string validstk xec
""".split()

# Left out on purpose: crypt, ipint, keyring, mono and sign pull in libmp
# and libsec; heapaudit only exists to name every Type descriptor.  None
# of them is needed to run a Dis module.

# The parts of emu/port that are not the OS: allocation, errors, the
# scheduler, the VM glue and printing.
EMUPORT = """
alloc cache discall dis env error errstr exception exptab inferno latin1 print
proc qio random uqid
""".split()

# The Harvey host.
HOST = ["k9", "stubs", "libcfree", "disvm"]


def cflags() -> list[str]:
    return [
        "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
        "-mstrict-align", "-fsigned-char", "-fcommon", "-O1", "-g",
        "-DEMU", "-DROOT=\"/dis\"", "-DUSE_FPdbleword",
        "-I", str(ROOT / "util/arm64/emu"),
        # limbo's headers, generated from purgatorio (gen/mkgen.sh), ahead
        # of any a local purgatorio build left in its libinterp
        "-I", str(ROOT / "util/arm64/emu/gen"),
        "-I", str(ROOT / "aarch64/include"),
        "-I", str(ROOT / "sys/include"),
        "-I", str(PURG / "emu/port"),
        "-I", str(PURG / "include"),
        "-I", str(PURG / "libinterp"),
    ]


def sources() -> list[Path]:
    files = []
    for name in LIBINTERP:
        files.append(PURG / "libinterp" / f"{name}.c")
    for name in EMUPORT:
        files.append(PURG / "emu/port" / f"{name}.c")
    for name in HOST:
        files.append(ROOT / "util/arm64/emu" / f"{name}.c")
    files.append(ROOT / "util/arm64/emu/asm-arm64.S")
    missing = [f for f in files if not f.exists()]
    if missing:
        raise SystemExit("missing sources: " + ", ".join(map(str, missing)))
    return files


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    objs = []
    failed = []
    for src in sources():
        obj = OUT / (src.stem + ".o")
        r = subprocess.run([CC, *cflags(), "-c", str(src), "-o", str(obj)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            failed.append((src, r.stderr))
        else:
            objs.append(obj)
    if failed:
        for src, err in failed:
            print(f"### {src}", file=sys.stderr)
            print("\n".join(err.splitlines()[:8]), file=sys.stderr)
        raise SystemExit(f"{len(failed)} source(s) failed")

    script = OUT / "disvm.ld"
    script.write_text(f'''ENTRY(_main)
PHDRS {{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}}
SECTIONS {{
  . = 0x{BASE:x};
  .text : {{ *(.text .text.*) *(.rodata .rodata.*) }} :text
  . = ALIGN(0x{BIG_PAGE:x});
  .data : {{ *(.data .data.*) *(.sdata .sdata.*) *(.got .got.*) }} :data
  .bss : {{ *(.bss .bss.*) *(.sbss .sbss.*) COMMON }} :data
  PROVIDE(end = .);
  /DISCARD/ : {{ *(.comment) *(.note*) }}
}}
''')
    elf = OUT / "disvm"
    r = subprocess.run([
        LD, "-static", "-z", "max-page-size=4096", "-T", str(script),
        "--allow-multiple-definition", "-e", "_main", "-u", "_main",
        *map(str, objs), "--start-group", str(LIBC), "--end-group",
        "-o", str(elf),
    ], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        raise SystemExit("link failed")
    print(f"disvm: {elf} ({elf.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
