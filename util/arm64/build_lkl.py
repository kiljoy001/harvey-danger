#!/usr/bin/env python3
"""Build the LKL M1 probe for Harvey/ARM64.

Requires an LKL build tree and the aarch64 lkl.o produced by it (see
doc/drivers/lkl.md for the exact kernel build recipe):

    python3 util/arm64/build_lkl.py \
        --lkl /tmp/opencode/lkl-install/lib/lkl.o \
        --include /tmp/opencode/lkl-install/include \
        --archive /tmp/opencode/lkl-u.img

The probe links below 0x40000000: Harvey's kernel/user boundary is
fixed there (KPTR), so a binary at 0x40000000 faults into kernel space.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM64_LD", "ld.lld")
BASE = 0x10000000          # below the 0x40000000 kernel boundary
BIG_PAGE = 0x200000
FAMILIES = json.loads((ROOT / "util/arm64/lkl-families.json").read_text())
SETUP_IDS = {"emmc": 1, "usb": 2, "wifi": 3}


def run(cmd: list[str]) -> None:
    subprocess.run(cmd, check=True)


def build(out: Path, lkl_o: Path, lkl_inc: Path, libc: Path) -> Path:
    src = ROOT / "sys/src/cmd/lkl"
    out.mkdir(parents=True, exist_ok=True)
    cflags = [
        CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-mstrict-align", "-fsigned-char", "-fcommon",
        "-O2",
        "-I", str(ROOT / "aarch64/include"),
        "-I", str(ROOT / "sys/include"),
        "-I", str(lkl_inc),
    ]
    objects = []
    for name in ("harvey-host.c", "lklboot.c", "lklusb.c"):
        obj = out / (Path(name).stem + ".o")
        run(cflags + ["-c", str(src / name), "-o", str(obj)])
        objects.append(obj)
    obj = out / "tramp.o"
    run([CC, "--target=aarch64-none-elf", "-c", str(src / "tramp.s"),
         "-o", str(obj)])
    objects.append(obj)

    # One trimmed device tree per driver family.  The family tables and
    # the .incbin stubs are generated here so adding a family is a JSON
    # entry plus a .dts file.
    dtbs = []
    for fam, spec in FAMILIES.items():
        dtssrc = ROOT / "util/arm64" / spec["dtb"].replace(".dtb", ".dts")
        dst = out / spec["dtb"]
        run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(dst), str(dtssrc)])
        dtbs.append((fam, spec["dtb"]))
    asm = out / "lkl-dtb.S"
    asm.write_text(".section .rodata\n" + "".join(
        f".balign 8\n.global lkl_dtb_{fam}\n.global lkl_dtb_{fam}_end\n"
        f"lkl_dtb_{fam}:\n\t.incbin \"{name}\"\n"
        f"lkl_dtb_{fam}_end:\n"
        for fam, name in dtbs))
    obj = out / "lkl-dtb.o"
    run([CC, "--target=aarch64-none-elf", "-c", "-I", str(out),
         str(asm), "-o", str(obj)])
    objects.append(obj)

    tables = out / "lkl-tables.c"
    entries = []
    dtb_entries = []
    for fam, spec in FAMILIES.items():
        memsz = 0
        m = spec["mem"].strip().upper()
        if m.endswith("M"):
            memsz = int(m[:-1]) * 1024 * 1024
        elif m.endswith("G"):
            memsz = int(m[:-1]) * 1024 * 1024 * 1024
        else:
            memsz = int(m)
        sid = SETUP_IDS[spec["setup"]]
        entries.append(
            f'\t{{"{fam}", "{spec["program"]}", "{spec["mem"]}", '
            f'{memsz}UL, "{",".join(spec["devices"])}", '
            f'"{spec["dtb"]}", {sid}}},')
        dtb_entries.append(
            f'\t{{"{spec["dtb"]}", lkl_dtb_{fam}, lkl_dtb_{fam}_end}},')
    tables.write_text(
        "#include <u.h>\n#include \"lklfamily.h\"\n" +
        "".join(f"extern uchar lkl_dtb_{fam}[], lkl_dtb_{fam}_end[];\n"
                for fam, _ in dtbs) +
        "Family lklfamilies[] = {\n" + "\n".join(entries) + "\n};\n"
        f"int nlklfamilies = {len(entries)};\n"
        "Dtb lkldtbs[] = {\n" + "\n".join(dtb_entries) + "\n};\n"
        f"int nlkldtbs = {len(dtb_entries)};\n")
    obj = out / "lkl-tables.o"
    run(cflags + ["-I", str(src), "-c", str(tables), "-o", str(obj)])
    objects.append(obj)

    script = out / "lkl.ld"
    script.write_text(f"""ENTRY(_main)
PHDRS {{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}}
SECTIONS {{
  . = 0x{BASE:x};
  .text : {{
    *(.text .text.*)
    *(.init.text .exit.text)
  }} :text
  . = ALIGN(0x{BIG_PAGE:x});
  .data : {{
    *(.rodata .rodata.*)
    *(.init.rodata .exit.rodata)
    *(__init_rodata)
    *(.notes)
    *(__ex_table)
    *(__ksymtab*)
    *(__kcrctab*)
    *(__param)
    *(__modver)
    *(pci_fixup*)
    *(.pci_fixup*)
    *(.data .data.*)
    *(.init.data .exit.data)
    *(.sdata .sdata.*)
    *(.got .got.*)
    *(.data.rel.ro*)
  }} :data
  .bss : {{
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
  }} :data
  PROVIDE(end = .);
}}
""")
    elf = out / "lklboot"
    run([LD, "-static", "-z", "max-page-size=4096", "-T", str(script),
         "--allow-multiple-definition", "-e", "_main", "-u", "_main",
         *map(str, objects), str(lkl_o), str(libc), "-o", str(elf)])
    return elf


def archive(elf: Path, out: Path) -> None:
    blob = elf.read_bytes()
    table = 24 + 48
    body = struct.pack("<8sIIII", b"O9BOOT01", 1, 1, table + len(blob), 0)
    body += struct.pack("<32sIIII", b"lklboot", table, len(blob), 0o555, 0)
    out.write_bytes(body + blob)
    print(f"{out}: {len(blob)} bytes program, {len(body) + len(blob)} archive")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lkl", required=True,
                    help="lkl.o from the LKL build (install/lib/lkl.o)")
    ap.add_argument("--include", required=True,
                    help="LKL install/include (asm/host_ops.h)")
    ap.add_argument("--libc", default=str(ROOT / "build/arm64-libc/libc.a"))
    ap.add_argument("--out", default=str(ROOT / "build/arm64-lkl"))
    ap.add_argument("--archive")
    a = ap.parse_args()
    elf = build(Path(a.out), Path(a.lkl), Path(a.include), Path(a.libc))
    print(f"{elf}: {elf.stat().st_size} bytes")
    # The debug-info-heavy ELF is 100+ MB; the embedded boot copy only needs
    # the program.  Keep both so a debugger still has symbols.
    strip = shutil.which("llvm-strip") or shutil.which("strip")
    if strip is not None:
        thin = elf.with_suffix(".stripped")
        shutil.copyfile(elf, thin)
        run([strip, "--strip-debug", str(thin)])
        print(f"{thin}: {thin.stat().st_size} bytes (stripped)")
    if a.archive:
        archive(elf, Path(a.archive))


if __name__ == "__main__":
    main()
