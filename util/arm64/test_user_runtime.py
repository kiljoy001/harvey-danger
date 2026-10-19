#!/usr/bin/env python3
"""Link a minimal ARM64 Harvey process against the cross-built libc."""

from pathlib import Path
import os
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/arm64-user-test"
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM64_LD") or shutil.which("ld.lld") or "ld.lld"
NM = shutil.which("llvm-nm-23") or shutil.which("llvm-nm") or "llvm-nm"


def main() -> None:
    subprocess.run([sys.executable, str(ROOT / "util/arm64/build_libc.py")],
                   check=True)
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    source = OUT / "runtime.c"
    source.write_text(
        "#include <u.h>\n"
        "#include <libc.h>\n"
        "void main(int argc, char **argv)\n"
        "{\n"
        "\tif(argc < 0 || argv == nil || nsec() == 0)\n"
        "\t\texits(\"runtime\");\n"
        "\texits(nil);\n"
        "}\n"
    )
    script = OUT / "user.ld"
    script.write_text(
        "ENTRY(_main)\n"
        "SECTIONS {\n"
        "  . = 0x200000;\n"
        "  .text : { *(.text .text.*) }\n"
        "  .rodata : { *(.rodata .rodata.*) }\n"
        "  .data : { *(.data .data.*) }\n"
        "  .bss : { *(.bss .bss.* COMMON) }\n"
        "}\n"
    )
    obj = OUT / "runtime.o"
    elf = OUT / "runtime.elf"
    subprocess.run([
        CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-mstrict-align", "-O2",
        "-I", str(ROOT / "aarch64/include"),
        "-I", str(ROOT / "sys/include"), "-c", str(source), "-o", str(obj),
    ], check=True)
    subprocess.run([
        LD, "-T", str(script), "-e", "_main", "-u", "_main", str(obj),
        str(ROOT / "build/arm64-libc/libc.a"), "-o", str(elf),
    ], check=True)
    symbols = subprocess.run(
        [NM, "-g", str(elf)], check=True, text=True,
        capture_output=True).stdout
    for name in ("_main", "main", "nsec", "_exits", "_tos", "_privates",
                 "_nprivates"):
        if not any(line.endswith(f" {name}") for line in symbols.splitlines()):
            raise RuntimeError(f"linked runtime is missing {name}")
    print(f"ARM64 user runtime link PASS: {elf}")


if __name__ == "__main__":
    main()
