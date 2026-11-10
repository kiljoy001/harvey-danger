#!/usr/bin/env python3
"""Build the first filesystem-backed ARM64 user processes for Harvey."""

from pathlib import Path
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import buildcache


ROOT = Path(__file__).resolve().parents[2]
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM64_LD") or shutil.which("ld.lld") or "ld.lld"
NM = shutil.which("llvm-nm-23") or shutil.which("llvm-nm") or "llvm-nm"
STUB_BASE = 0x4000000000
INIT_BASE = 0x200000
BIG_PAGE = 0x200000
O9DRVMAN_BASE = 0x600000
UARTFS_BASE = 0x1600000
GPIOFS_BASE = 0x1A00000
GPUS_BASE = 0xD000000
HUBFS_BASE = 0x1E00000
DRVMAN_BASE = 0x2200000
I2CFS_BASE = 0x2600000
PMICFS_BASE = 0xA600000
MKDIR_BASE = 0xAA00000
MOUNT_BASE = 0xAE00000
MOUNT9_BASE = 0xB200000
EXT2SRV_BASE = 0xBA00000
PROVISION_BASE = 0xBE00000
GEFS_BASE = 0xC200000
BIGTEST_BASE = 0xCA00000
RC_BASE = 0x2A00000
ECHO_BASE = 0x2E00000
CAT_BASE = 0x3200000
LS_BASE = 0x3600000
RTC_BASE = 0x3E00000
PING_BASE = 0x8200000
SDPROBE_BASE = 0x8600000
SDCARD_BASE = 0xB600000
KEYROOT_PROBE_BASE = 0x8A00000
DP9IKTEST_BASE = 0x8E00000
FACTOTUM_BASE = 0x9600000
FACTOTUMKEY_BASE = 0x9A00000
KEYROOT_BASE = 0x9E00000
KEYROOTTEST_BASE = 0xA200000
EMMC_BASE = 0x5200000
EMMCFS_BASE = 0x4600000
GMAC_BASE = 0x4A00000
GMACFS_BASE = 0x7A00000
DISPLAYTEST_BASE = 0x4200000
O9XHCI_BASE = 0x5600000
TESTXHCI_BASE = 0x5E00000
TESTXHCI1_BASE = 0x6600000
TESTAUDIO_BASE = 0x6E00000
USBPOWER_BASE = 0x7600000

RC_SOURCES = [
    "code.c", "exec.c", "getflags.c", "glob.c", "havefork.c", "here.c",
    "io.c", "lex.c", "pcmd.c", "pfnc.c", "plan9.c", "simple.c", "subr.c",
    "trap.c", "tree.c", "var.c",
]

# rc only needs this closure of libString for backquote handling and
# variable growth; pulling in the whole library would also pull in libbio.
RC_STRING_SOURCES = [
    "s_alloc.c", "s_nappend.c", "s_reset.c", "s_unique.c", "s_terminate.c",
    "s_putc.c", "s_grow.c", "s_copy.c",
]


def symbol_value(elf: Path, name: str) -> int:
    output = subprocess.run([NM, "-P", str(elf)], check=True, text=True,
                            capture_output=True).stdout
    for line in output.splitlines():
        fields = line.split()
        if fields and fields[0] == name:
            return int(fields[2], 16)
    raise RuntimeError(f"missing symbol {name} in {elf}")


def validate_elf(path: Path, base: int) -> tuple[bytes, int]:
    image = path.read_bytes()
    if len(image) < 64 or image[:7] != b"\x7fELF\x02\x01\x01":
        raise RuntimeError(f"{path} is not a little-endian ELF64 image")
    machine, = struct.unpack_from("<H", image, 18)
    entry, phoff = struct.unpack_from("<QQ", image, 24)
    phentsize, phnum = struct.unpack_from("<HH", image, 54)
    if machine != 183 or phentsize != 56 or phnum == 0:
        raise RuntimeError(f"{path} has an invalid AArch64 program header")
    executable = False
    writable = False
    for index in range(phnum):
        off = phoff + index * phentsize
        ptype, flags, fileoff, va, _, filesz, memsz, align = struct.unpack_from(
            "<IIQQQQQQ", image, off)
        if ptype != 1:
            continue
        if filesz > memsz or fileoff + filesz > len(image):
            raise RuntimeError(f"{path} has an invalid load segment")
        if flags & 1:
            executable = executable or base <= entry < base + BIG_PAGE
            if not base <= va or va + memsz > base + BIG_PAGE:
                raise RuntimeError(f"{path} executable segment exceeds one big page")
        if flags & 2:
            writable = True
            if not base + BIG_PAGE <= va or va + memsz > base + 2 * BIG_PAGE:
                raise RuntimeError(f"{path} writable segment exceeds one big page")
        if align > BIG_PAGE:
            raise RuntimeError(f"{path} requests unsupported alignment {align:#x}")
    if not executable or not writable:
        raise RuntimeError(f"{path} needs separate executable and writable segments")
    return image, entry


def build_program(out: Path, name: str, source_text: str, base: int,
                  libc: Path) -> tuple[bytes, int]:
    source = out / f"{name}.c"
    script = out / f"{name}.ld"
    obj = out / f"{name}.o"
    elf = out / name
    source.write_text(source_text)
    script.write_text(f'''ENTRY(_main)
SECTIONS {{
  . = 0x{base:x};
  .text : {{ *(.text .text.*) *(.rodata .rodata.*) }}
  . = ALIGN(0x{BIG_PAGE:x});
  .data : {{ *(.data .data.*) }}
  .bss : {{ *(.bss .bss.* COMMON) }}
  PROVIDE(end = .);
}}
''')
    subprocess.run([
        CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
        "-mstrict-align", "-O2",
        "-I", str(ROOT / "aarch64/include"),
        "-I", str(ROOT / "sys/include"), "-c", str(source), "-o", str(obj),
    ], check=True)
    subprocess.run([
        LD, "-z", "max-page-size=4096", "-T", str(script),
        "-e", "_main", "-u", "_main", str(obj), str(libc), "-o", str(elf),
    ], check=True)
    image, entry = validate_elf(elf, base)
    if symbol_value(elf, "_main") != entry:
        raise RuntimeError(f"{name} entry does not name _main")
    return image, entry


def emit_array(lines: list[str], name: str, image: bytes) -> None:
    # Emit the image as an escaped string literal rather than millions of
    # byte initialisers: clang 23 crashes in its parser on a translation
    # unit with this many elements (it started failing once the userland
    # image passed ~16 MiB).  sizeof() is unchanged because the array size
    # is explicit.
    lines.append(f"static const u8 {name}[{len(image)}] =")
    for offset in range(0, len(image), 32):
        chunk = image[offset:offset + 32]
        lines.append("\t\"" + "".join(f"\\x{byte:02x}" for byte in chunk) + "\"")
    lines.extend(("\t;", ""))


def build_elf(out: Path, libc: Path, name: str, source,
              base: int, extra: list[Path], ldflags: list[str] = (),
              cflags: list[str] = ()) -> tuple[bytes, int]:
    sources = list(source) if isinstance(source, (list, tuple)) else [source]
    objs = []
    script = out / f"{name}.ld"
    elf = out / name
    script.write_text(f'''ENTRY(_main)
PHDRS {{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}}
SECTIONS {{
  . = 0x{base:x};
  .text : {{ *(.text .text.*) *(.rodata .rodata.*) }} :text
  . = ALIGN(0x{BIG_PAGE:x});
  .data : {{ *(.data .data.*) *(.sdata .sdata.*) *(.got .got.*) }} :data
  .bss : {{ *(.bss .bss.*) *(.sbss .sbss.*) COMMON }} :data
  PROVIDE(end = .);
  /DISCARD/ : {{ *(.comment) *(.note*) }}
}}
''')
    for index, src in enumerate(sources):
        obj = out / (f"{name}.o" if len(sources) == 1
                     else f"{name}-{index}-{src.stem}.o")
        subprocess.run([
            CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
            "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
            "-mstrict-align", "-fsigned-char", "-fcommon", "-O2",
            *cflags,
            "-I", str(ROOT / "aarch64/include"),
            "-I", str(ROOT / "sys/include"),
            "-I", str(ROOT / "sys/src/cmd/auth/keyroot/ta/pq/usercompat"),
            "-I", str(ROOT / "sys/src/cmd/auth/keyroot/ta/pq"),
            "-c", str(src), "-o", str(obj),
        ], check=True)
        objs.append(str(obj))
    libs = [*extra, libc]
    subprocess.run([
        LD, "-static", "-z", "max-page-size=4096", "-T", str(script),
        "--allow-multiple-definition", *ldflags,
        "-e", "_main", "-u", "_main", *objs,
        "--start-group", *map(str, libs), "--end-group",
        "-o", str(elf),
    ], check=True)
    image, entry = validate_elf(elf, base)
    if symbol_value(elf, "_main") != entry:
        raise RuntimeError(f"{name} entry does not name _main")
    return image, entry


def build_fs(out: Path, libc: Path, name: str, source: Path,
             base: int) -> tuple[bytes, int]:
    o9 = ROOT / "build/arm64-o9"
    health = out / "driverhealth.o"
    drvkit = out / "drvkit.o"
    drvflags = [
        "-D__HARVEY__",
        "-I", str(ROOT / "sys/src/cmd/o9drvman"),
    ]
    if not health.exists():
        subprocess.run([
            CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
            "-fno-stack-protector", "-mstrict-align", "-fcommon", "-O2",
            "-D__HARVEY__",
            "-I", str(ROOT / "aarch64/include"),
            "-I", str(ROOT / "sys/include"),
            "-I", str(ROOT / "sys/src/cmd/o9drvman"),
            "-c", str(ROOT / "sys/src/cmd/o9drvman/health.c"),
            "-o", str(health),
        ], check=True)
    subprocess.run([
        CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
        "-mstrict-align", "-fsigned-char", "-fcommon", "-O2",
        *drvflags,
        "-I", str(ROOT / "aarch64/include"),
        "-I", str(ROOT / "sys/include"),
        "-c", str(ROOT / "sys/src/cmd/drvkit/drv.c"),
        "-o", str(drvkit),
    ], check=True)
    extra = [health, drvkit, o9 / "lib9p.a", o9 / "libthread.a",
             o9 / "libauth.a", o9 / "libauthsrv.a", o9 / "libsec.a",
             o9 / "libndb.a", o9 / "libbio.a"]
    return build_elf(out, libc, name, source, base, extra,
                     ldflags=["--defsym=threadmain=main"], cflags=drvflags)


def build_factotum(out: Path, libc: Path) -> tuple[bytes, int]:
    """Build factotum with the dp9ik protocol for ARM64 Harvey."""
    o9 = ROOT / "build/arm64-o9"
    srcdir = ROOT / "sys/src/cmd/auth/factotum"
    names = (
        "apop.c", "chap.c", "confirm.c", "fs.c", "httpdigest.c",
        "log.c", "p9any.c", "p9cr.c", "p9sk1.c", "dp9ik.c", "fido.c",
        "pass.c", "rpc.c", "rsa.c", "secstore.c", "util.c", "wep.c",
    )
    flags = [
        CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables", "-fno-vectorize",
        "-fno-slp-vectorize", "-mstrict-align", "-fsigned-char",
        "-fcommon", "-O2", "-D__HARVEY__", "-DMONO_PLAN9",
        "-I", str(ROOT / "aarch64/include"),
        "-I", str(ROOT / "sys/include"),
        "-I", str(srcdir),
    ]
    objs = []
    for name in names:
        obj = out / ("factotum-" + name.replace(".c", ".o"))
        subprocess.run([*flags, "-c", str(srcdir / name), "-o", str(obj)],
                       check=True)
        objs.append(obj)
    script = out / "factotum.ld"
    script.write_text(f'''ENTRY(_main)
PHDRS {{ text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }}
SECTIONS {{
  . = 0x{FACTOTUM_BASE:x};
  .text : {{ *(.text .text.*) *(.rodata .rodata.*) }} :text
  . = ALIGN(0x{BIG_PAGE:x});
  .data : {{ *(.data .data.*) *(.sdata .sdata.*) *(.got .got.*) }} :data
  .bss : {{ *(.bss .bss.*) *(.sbss .sbss.*) COMMON }} :data
  PROVIDE(end = .);
  /DISCARD/ : {{ *(.comment) *(.note*) }}
}}
''')
    libs = [
        o9 / "lib9p.a", o9 / "libthread.a", o9 / "libauth.a",
        o9 / "libauthsrv.a", o9 / "libsec.a", o9 / "libndb.a",
        o9 / "libbio.a", o9 / "libmp.a", o9 / "libString.a",
        o9 / "libip.a", libc,
    ]
    elf = out / "factotum"
    subprocess.run([
        LD, "-static", "-z", "max-page-size=4096", "-T", str(script),
        "--allow-multiple-definition", "--defsym=threadmain=main",
        "-e", "_main", "-u", "_main", *map(str, objs),
        "--start-group", *map(str, libs), "--end-group",
        "-o", str(elf),
    ], check=True)
    return validate_elf(elf, FACTOTUM_BASE)


def build_simple(out: Path, libc: Path, name: str, source: Path,
                 base: int) -> tuple[bytes, int]:
    return build_elf(out, libc, name, source, base, [])


def build_pq_objs(out: Path) -> list[Path]:
    """Compile the Dilithium verifier side for normal-world Harvey."""
    pqdir = ROOT / "sys/src/cmd/auth/keyroot/ta/pq"
    names = (
        "fips202.c", "ntt.c", "packing.c", "poly.c", "polyvec.c",
        "reduce.c", "rounding.c", "sign.c", "symmetric-shake.c",
        "randombytes_user.c",
    )
    objs: list[Path] = []
    for name in names:
        obj = out / ("pq_" + name.replace(".c", ".o"))
        subprocess.run([
            CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
            "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
            "-mstrict-align", "-fsigned-char", "-fcommon", "-O2",
            "-I", str(ROOT / "aarch64/include"),
            "-I", str(ROOT / "sys/include"),
            "-I", str(pqdir / "usercompat"),
            "-I", str(pqdir),
            "-c", str(pqdir / name), "-o", str(obj),
        ], check=True)
        objs.append(obj)
    desdir = ROOT / "sys/src/cmd/auth/keyroot/ta/des"
    for name in ("des.c", "des_crypt.c"):
        obj = out / ("des_" + name.replace(".c", ".o"))
        subprocess.run([
            CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
            "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
            "-mstrict-align", "-fsigned-char", "-fcommon", "-O2",
            "-I", str(desdir),
            "-c", str(desdir / name), "-o", str(obj),
        ], check=True)
        objs.append(obj)
    return objs


def build_uartfs(out: Path, libc: Path) -> tuple[bytes, int]:
    return build_fs(out, libc, "uartfs",
                    ROOT / "sys/src/cmd/uartfs/uartfs.c", UARTFS_BASE)


def build_gpiofs(out: Path, libc: Path) -> tuple[bytes, int]:
    return build_fs(out, libc, "gpiofs",
                    ROOT / "sys/src/cmd/gpiofs/gpiofs.c", GPIOFS_BASE)


def build_gpufs(out: Path, libc: Path) -> tuple[bytes, int]:
    return build_fs(out, libc, "gpufs",
                    ROOT / "sys/src/cmd/gpufs/gpufs.c", GPUS_BASE)


def build_hubfs(out: Path, libc: Path) -> tuple[bytes, int]:
    return build_fs(out, libc, "hubfs",
                    ROOT / "sys/src/cmd/hubfs/hubfs.c", HUBFS_BASE)


def build_drvman(out: Path, libc: Path) -> tuple[bytes, int]:
    return build_simple(out, libc, "drvman",
                        ROOT / "sys/src/cmd/drvman/drvman.c", DRVMAN_BASE)


def build_i2cfs(out: Path, libc: Path) -> tuple[bytes, int]:
    return build_fs(out, libc, "i2cfs",
                    ROOT / "sys/src/cmd/i2cfs/i2cfs.c", I2CFS_BASE)


def build_pmicfs(out: Path, libc: Path) -> tuple[bytes, int]:
    return build_fs(out, libc, "pmicfs",
                    ROOT / "sys/src/cmd/pmicfs/pmicfs.c", PMICFS_BASE)


def build_emmcfs(out: Path, libc: Path) -> tuple[bytes, int]:
    """The eMMC driver core plus its 9P server, linked with lib9p."""
    o9 = ROOT / "build/arm64-o9"
    flags = [
        CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
        "-mstrict-align", "-fsigned-char", "-fcommon", "-O2",
        "-I", str(ROOT / "aarch64/include"),
        "-I", str(ROOT / "sys/include"),
    ]
    objects = []
    for name, source, defines in (
            ("emmcfs-driver", ROOT / "sys/src/cmd/emmc/emmc.c",
             ["-DEMMCFS_LIB"]),
            ("emmcfs", ROOT / "sys/src/cmd/emmcfs/emmcfs.c", [])):
        obj = out / f"{name}.o"
        subprocess.run([*flags, *defines, "-c", str(source), "-o", str(obj)],
                       check=True)
        objects.append(obj)
    script = out / "emmcfs.ld"
    elf = out / "emmcfs"
    script.write_text(f'''ENTRY(_main)
PHDRS {{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}}
SECTIONS {{
  . = 0x{EMMCFS_BASE:x};
  .text : {{ *(.text .text.*) *(.rodata .rodata.*) }} :text
  . = ALIGN(0x{BIG_PAGE:x});
  .data : {{ *(.data .data.*) *(.sdata .sdata.*) *(.got .got.*) }} :data
  .bss : {{ *(.bss .bss.*) *(.sbss .sbss.*) COMMON }} :data
  PROVIDE(end = .);
  /DISCARD/ : {{ *(.comment) *(.note*) }}
}}
''')
    subprocess.run([
        LD, "-static", "-z", "max-page-size=4096", "-T", str(script),
        "--allow-multiple-definition", "--defsym=threadmain=main",
        "-e", "_main", "-u", "_main", *map(str, objects),
        "--start-group",
        str(o9 / "lib9p.a"), str(o9 / "libthread.a"), str(o9 / "libauth.a"),
        str(o9 / "libauthsrv.a"), str(o9 / "libsec.a"), str(o9 / "libndb.a"),
        str(o9 / "libbio.a"), str(libc),
        "--end-group", "-o", str(elf),
    ], check=True)
    image, entry = validate_elf(elf, EMMCFS_BASE)
    if symbol_value(elf, "_main") != entry:
        raise RuntimeError("emmcfs entry does not name _main")
    return image, entry


def build_ext2srv(out: Path, libc: Path) -> tuple[bytes, int]:
    """The ext2 file server, linked with lib9p over an emmc partition."""
    o9 = ROOT / "build/arm64-o9"
    src = ROOT / "sys/src/cmd/ext2srv"
    flags = [
        CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
        "-mstrict-align", "-fsigned-char", "-fcommon", "-O2",
        "-I", str(ROOT / "aarch64/include"),
        "-I", str(ROOT / "sys/include"),
        "-I", str(src),
    ]
    objects = []
    for name in ("chat.c", "ext2fs.c", "ext2subs.c", "iobuf.c", "xfile.c",
                 "xfssrv.c"):
        obj = out / f"ext2srv-{name}.o"
        subprocess.run([*flags, "-c", str(src / name), "-o", str(obj)],
                       check=True)
        objects.append(obj)
    script = out / "ext2srv.ld"
    elf = out / "ext2srv"
    script.write_text(f'''ENTRY(_main)
PHDRS {{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}}
SECTIONS {{
  . = 0x{EXT2SRV_BASE:x};
  .text : {{ *(.text .text.*) *(.rodata .rodata.*) }} :text
  . = ALIGN(0x{BIG_PAGE:x});
  .data : {{ *(.data .data.*) *(.sdata .sdata.*) *(.got .got.*) }} :data
  .bss : {{ *(.bss .bss.*) *(.sbss .sbss.*) COMMON }} :data
  PROVIDE(end = .);
  /DISCARD/ : {{ *(.comment) *(.note*) }}
}}
''')
    subprocess.run([
        LD, "-static", "-z", "max-page-size=4096", "-T", str(script),
        "--allow-multiple-definition", "--defsym=threadmain=main",
        "-e", "_main", "-u", "_main", *map(str, objects),
        "--start-group",
        str(o9 / "lib9p.a"), str(o9 / "libthread.a"), str(o9 / "libauth.a"),
        str(o9 / "libauthsrv.a"), str(o9 / "libsec.a"), str(o9 / "libndb.a"),
        str(o9 / "libbio.a"), str(libc),
        "--end-group", "-o", str(elf),
    ], check=True)
    image, entry = validate_elf(elf, EXT2SRV_BASE)
    if symbol_value(elf, "_main") != entry:
        raise RuntimeError("ext2srv entry does not name _main")
    return image, entry


GEFS_SOURCES = (
    "blk.c", "cache.c", "check.c", "cons.c", "dump.c", "error.c", "fs.c",
    "hash.c", "load.c", "main.c", "pack.c", "ream.c", "snap.c", "tree.c",
    "user.c",
)


def build_gefs(out: Path, libc: Path) -> tuple[bytes, int]:
    """gefs: compiled by ucc in Plan 9 mode, linked with our own layout.

    ucc (-m9) understands Plan 9 C, including anonymous struct members,
    so the 9front sources compile unchanged.  Its own driver links a
    three-segment image, while this kernel's user loader expects one
    text page and one data page, so the objects are linked here with
    ld.lld and the same script as the other services.
    """
    o9 = ROOT / "build/arm64-o9"
    src = ROOT / "sys/src/cmd/gefs"
    ucc = os.environ.get("UCC") or shutil.which("ucc") or "/home/scott/Repo/ucc/ucc"
    if not Path(ucc).is_file():
        raise RuntimeError("ucc not found; set UCC to the compiler driver")
    objects = []
    for name in GEFS_SOURCES:
        obj = out / f"gefs-{name}.o"
        subprocess.run([ucc, "-m9", "-marm64", "-c", str(src / name),
                        "-o", str(obj)], check=True)
        objects.append(obj)
    # TCC has no __atomic builtins; the accessors come from clang.
    atomic = out / "gefs-atomic9.o"
    subprocess.run([CC, "--target=aarch64-none-elf", "-ffreestanding",
                    "-fno-builtin", "-mstrict-align", "-fsigned-char",
                    "-fcommon", "-O2",
                    "-I", str(ROOT / "aarch64/include"),
                    "-I", str(ROOT / "sys/include"),
                    "-c", str(ROOT / "util/arm64/atomic9.c"),
                    "-o", str(atomic)], check=True)
    objects.append(atomic)
    compat = out / "gefs-compat.o"
    subprocess.run([CC, "--target=aarch64-none-elf", "-ffreestanding",
                    "-fno-builtin", "-mstrict-align", "-fsigned-char",
                    "-fcommon", "-O2",
                    "-I", str(ROOT / "aarch64/include"),
                    "-I", str(ROOT / "sys/include"),
                    "-c", str(ROOT / "util/arm64/gefscompat.c"),
                    "-o", str(compat)], check=True)
    objects.append(compat)
    script = out / "gefs.ld"
    elf = out / "gefs"
    script.write_text(f'''ENTRY(_main)
PHDRS {{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}}
SECTIONS {{
  . = 0x{GEFS_BASE:x};
  .text : {{ *(.text .text.*) *(.rodata .rodata.*) }} :text
  . = ALIGN(0x{BIG_PAGE:x});
  .data : {{ *(.data .data.*) *(.sdata .sdata.*) *(.got .got.*) }} :data
  .bss : {{ *(.bss .bss.*) *(.sbss .sbss.*) COMMON }} :data
  PROVIDE(end = .);
  /DISCARD/ : {{ *(.comment) *(.note*) }}
}}
''')
    subprocess.run([
        LD, "-static", "-z", "max-page-size=4096", "-T", str(script),
        "--allow-multiple-definition", "-e", "_main", "-u", "_main",
        *map(str, objects),
        "--start-group",
        str(o9 / "libavl.a"), str(o9 / "libbio.a"), str(o9 / "libauth.a"),
        str(o9 / "libsec.a"), str(o9 / "libmp.a"), str(libc),
        "--end-group", "-o", str(elf),
    ], check=True)
    image, entry = validate_elf(elf, GEFS_BASE)
    if symbol_value(elf, "_main") != entry:
        raise RuntimeError("gefs entry does not name _main")
    return image, entry


def build_gmacfs(out: Path, libc: Path) -> tuple[bytes, int]:
    """The GMAC driver core plus its 9P service, linked with lib9p."""
    o9 = ROOT / "build/arm64-o9"
    flags = [
        CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
        "-mstrict-align", "-fsigned-char", "-fcommon", "-O2",
        "-I", str(ROOT / "aarch64/include"),
        "-I", str(ROOT / "sys/include"),
    ]
    objects = []
    for name, source, defines in (
            ("gmacfs-driver", ROOT / "sys/src/cmd/gmac/gmac.c",
             ["-DGMACFS_LIB"]),
            ("gmacfs", ROOT / "sys/src/cmd/gmacfs/gmacfs.c", [])):
        obj = out / f"{name}.o"
        subprocess.run([*flags, *defines, "-c", str(source), "-o", str(obj)],
                       check=True)
        objects.append(obj)
    script = out / "gmacfs.ld"
    elf = out / "gmacfs"
    script.write_text(f'''ENTRY(_main)
PHDRS {{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}}
SECTIONS {{
  . = 0x{GMACFS_BASE:x};
  .text : {{ *(.text .text.*) *(.rodata .rodata.*) }} :text
  . = ALIGN(0x{BIG_PAGE:x});
  .data : {{ *(.data .data.*) *(.sdata .sdata.*) *(.got .got.*) }} :data
  .bss : {{ *(.bss .bss.*) *(.sbss .sbss.*) COMMON }} :data
  PROVIDE(end = .);
  /DISCARD/ : {{ *(.comment) *(.note*) }}
}}
''')
    subprocess.run([
        LD, "-static", "-z", "max-page-size=4096", "-T", str(script),
        "--allow-multiple-definition", "--defsym=threadmain=main",
        "-e", "_main", "-u", "_main", *map(str, objects),
        "--start-group",
        str(o9 / "lib9p.a"), str(o9 / "libthread.a"), str(o9 / "libauth.a"),
        str(o9 / "libauthsrv.a"), str(o9 / "libsec.a"), str(o9 / "libndb.a"),
        str(o9 / "libbio.a"), str(libc),
        "--end-group", "-o", str(elf),
    ], check=True)
    image, entry = validate_elf(elf, GMACFS_BASE)
    if symbol_value(elf, "_main") != entry:
        raise RuntimeError("gmacfs entry does not name _main")
    return image, entry


def build_usbpower(out: Path, libc: Path) -> tuple[bytes, int]:
    if os.environ.get("ARM64_NO_USBPOWER") == "1":
        # Isolation build: satisfy drvman without touching PMU/CRU, so the
        # eMMC can be tested against the bootloader's untouched state.
        stub = out / "usbpower_stub.c"
        stub.write_text(r'''#include <u.h>
#include <libc.h>

void
main(void)
{
	int fd;

	fd = create("/mnt/usbpower.ready", OWRITE, 0644);
	if(fd >= 0)
		close(fd);
	print("usbpower: disabled for isolation test\n");
	for(;;)
		sleep(1000);
}
''')
        return build_fs(out, libc, "usbpower", stub, USBPOWER_BASE)
    return build_fs(out, libc, "usbpower",
                    ROOT / "sys/src/cmd/usbpower/usbpower.c", USBPOWER_BASE)


def build_rc(out: Path, libc: Path) -> tuple[bytes, int]:
    rcsrc = out / "rcsrc"
    rcsrc.mkdir()
    parser = rcsrc / "syn.y"
    shutil.copyfile(ROOT / "sys/src/cmd/rc/syn.y", parser)
    subprocess.run(["yacc", "-d", "-o", "y.tab.c", "syn.y"], cwd=rcsrc,
                   check=True, text=True, capture_output=True)
    shutil.copyfile(rcsrc / "y.tab.h", rcsrc / "x.tab.h")

    flags = [
        CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
        "-mstrict-align", "-fsigned-char", "-fcommon", "-O2", "-D__HARVEY__",
        "-I", str(rcsrc), "-I", str(ROOT / "sys/src/cmd/rc"),
        "-I", str(ROOT / "aarch64/include"), "-I", str(ROOT / "sys/include"),
    ]
    objects = []
    sources = [ROOT / "sys/src/cmd/rc" / name for name in RC_SOURCES]
    sources.append(rcsrc / "y.tab.c")
    for source in sources:
        obj = out / f"rc-{source.stem}.o"
        subprocess.run([*flags, "-c", str(source), "-o", str(obj)], check=True)
        objects.append(obj)
    for name in RC_STRING_SOURCES:
        obj = out / f"rc-{Path(name).stem}.o"
        subprocess.run([*flags, "-I", str(ROOT / "sys/src/libString"),
                        "-c", str(ROOT / "sys/src/libString" / name),
                        "-o", str(obj)], check=True)
        objects.append(obj)

    script = out / "rc.ld"
    elf = out / "rc"
    script.write_text(f'''ENTRY(_main)
PHDRS {{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}}
SECTIONS {{
  . = 0x{RC_BASE:x};
  .text : {{ *(.text .text.*) *(.rodata .rodata.*) }} :text
  . = ALIGN(0x{BIG_PAGE:x});
  .data : {{ *(.data .data.*) *(.sdata .sdata.*) *(.got .got.*) }} :data
  .bss : {{ *(.bss .bss.*) *(.sbss .sbss.*) COMMON }} :data
  PROVIDE(end = .);
  /DISCARD/ : {{ *(.comment) *(.note*) }}
}}
''')
    subprocess.run([
        LD, "-static", "-z", "max-page-size=4096", "-T", str(script),
        "-e", "_main", "-u", "_main", *map(str, objects),
        "--start-group", str(libc), "--end-group", "-o", str(elf),
    ], check=True)
    image, entry = validate_elf(elf, RC_BASE)
    if symbol_value(elf, "_main") != entry:
        raise RuntimeError("rc entry does not name _main")
    return image, entry


def write_archive(path: Path, files: dict[str, bytes]) -> None:
    """Emit the boot archive U-Boot loads next to the kernel.

    Layout (little endian): magic[8], version, count, total, reserved,
    then count entries of {name[32], offset, size, mode, reserved}, then
    the blobs, 8-byte aligned.  Offsets are from the start of the blob.
    """
    names = sorted(files)
    table_size = 24 + 48 * len(names)
    data = bytearray()
    entries = []
    for name in names:
        while (table_size + len(data)) % 8:
            data.append(0)
        offset = table_size + len(data)
        blob = files[name]
        data += blob
        entries.append((name, offset, len(blob)))
    table = b"".join(struct.pack("<32sIIII", name.encode(), offset, size,
                                 0o555, 0)
                     for name, offset, size in entries)
    total = table_size + len(data)
    header = struct.pack("<8sIIII", b"O9BOOT01", 1, len(names), total, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + table + bytes(data))


def userland_signature() -> str:
    files = [Path(__file__).resolve()]
    for p in (ROOT / "sys/src/cmd").rglob("*"):
        if p.is_file() and p.suffix in (".c", ".h", ".o9", ".y", ".s", ".json"):
            files.append(p)
    for lib in ("sys/src/libString", "sys/src/libthread", "sys/src/lib9p",
                "sys/src/libc"):
        for p in (ROOT / lib).rglob("*"):
            if p.is_file() and p.suffix in (".c", ".h", ".json", ".s", ".S"):
                files.append(p)
    files.sort()
    variant = os.environ.get("ARM64_NO_USBPOWER", "0")
    # These select which programs are built (and with what), so they have
    # to be part of the cache key or a stale archive is reused.
    gefs = os.environ.get("ARM64_BUILD_GEFS", "0")
    ucc = os.environ.get("UCC", "")
    return buildcache.signature(
        ["build_userland", "no-usbpower=" + variant, "gefs=" + gefs,
         "ucc=" + ucc], files)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--ta", type=Path,
                        help="signed keyroot probe TA for /boot")
    args = parser.parse_args()
    if os.environ.get("ARM64_SKIP_O9") != "1":
        subprocess.run([sys.executable,
                        str(ROOT / "util/arm64/build_o9_services.py")],
                       check=True, stdout=subprocess.DEVNULL)
    out = args.output.parent / "userland"
    stamp = args.output.parent / "userland.sig"
    sig = userland_signature()
    if args.ta is not None:
        sig += ":ta:" + hashlib.sha256(args.ta.read_bytes()).hexdigest()
    if (args.output.exists() and buildcache.up_to_date(stamp, sig)
            and (args.archive is None or args.archive.exists())):
        print("ARM64 userland up to date")
        return
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    libc = ROOT / "build/arm64-libc/libc.a"
    stub, stub_entry = build_program(out, "execstub", r'''#include <u.h>
#include <libc.h>

void
main(int argc, char **argv)
{
	char *initargv[] = { "/boot/init", nil };
	char message[] = "HARVEY-ARM64 FILE-EXEC FAIL\n";
	int fd;

	USED(argc);
	USED(argv);
	exec(initargv[0], initargv);
	fd = open("#c/cons", OWRITE);
	if(fd >= 0)
		write(fd, message, sizeof(message)-1);
	_exits("exec");
}
''', STUB_BASE, libc)
    init, init_entry = build_program(out, "init", r'''#include <u.h>
#include <libc.h>

static void
say(char *message)
{
	write(1, message, strlen(message));
}

static int
waitfor(char *path, int tries)
{
	int fd;

	while(tries-- > 0){
		fd = open(path, OREAD);
		if(fd >= 0){
			close(fd);
			return 0;
		}
		sleep(100);
	}
	return -1;
}

static int
hubtest(void)
{
	char wbuf[] = "hubfs-ok\n";
	char rbuf1[32], rbuf2[32];
	int mfd, wfd, rfd1, rfd2, n1, n2;

	mfd = open("/srv/hubfs", OREAD);
	if(mfd < 0)
		return -1;
	create("/mnt/hub", OREAD, DMDIR|0755);
	if(mount(mfd, -1, "/mnt/hub", MREPL | MCREATE, "") < 0){
		close(mfd);
		return -1;
	}
	close(mfd);
	mfd = create("/mnt/hub/console", OWRITE, 0666);
	if(mfd < 0)
		return -1;
	close(mfd);
	wfd = open("/mnt/hub/console", OWRITE);
	if(wfd < 0)
		return -1;
	if(write(wfd, wbuf, sizeof(wbuf)-1) != sizeof(wbuf)-1){
		close(wfd);
		return -1;
	}
	close(wfd);
	/* two independent readers must each replay the stream (broadcast) */
	rfd1 = open("/mnt/hub/console", OREAD);
	rfd2 = open("/mnt/hub/console", OREAD);
	if(rfd1 < 0 || rfd2 < 0)
		return -1;
	n1 = read(rfd1, rbuf1, sizeof(rbuf1)-1);
	n2 = read(rfd2, rbuf2, sizeof(rbuf2)-1);
	close(rfd1);
	close(rfd2);
	if(n1 != sizeof(wbuf)-1 || memcmp(rbuf1, wbuf, n1) != 0 ||
	   n2 != sizeof(wbuf)-1 || memcmp(rbuf2, wbuf, n2) != 0)
		return -1;
	return 0;
}

static int
emmctest(void)
{
	char ctl[256], w[4096], r[4096];
	int fd, m, i;

	fd = open("/srv/emmc", OREAD);
	if(fd < 0)
		return -1;
	create("/mnt/emmc", OREAD, DMDIR|0755);
	if(mount(fd, -1, "/mnt/emmc", MREPL|MCREATE, "") < 0){
		close(fd);
		return -1;
	}
	close(fd);

	fd = open("/mnt/emmc/ctl", OREAD);
	if(fd < 0)
		return -1;
	m = read(fd, ctl, sizeof ctl - 1);
	close(fd);
	if(m <= 0)
		return -1;
	ctl[m] = 0;
	if(strstr(ctl, "blocksize 512") == nil)
		return -1;

	/*
	 * The card is read-only in normal service: the root filesystem is
	 * mounted with -r, so nothing here should write.  The write test
	 * costs a few KiB of flash every boot, so it only runs when asked.
	 */
	fd = open("/mnt/emmc/data", OREAD);
	if(fd < 0)
		return -1;
	if(pread(fd, r, 512, 18000LL*512) != 512)
		goto fail;
	close(fd);
	if(getenv("emmcwtest") == nil)
		return 0;

	fd = open("/mnt/emmc/data", ORDWR);
	if(fd < 0)
		return -1;

	/* eight blocks, aligned: the multi-block DMA path */
	for(i = 0; i < sizeof w; i++)
		w[i] = (i * 13 + 7) & 0xff;
	if(pwrite(fd, w, sizeof w, 18000LL*512) != sizeof w)
		goto fail;
	memset(r, 0, sizeof r);
	if(pread(fd, r, sizeof r, 18000LL*512) != sizeof r)
		goto fail;
	if(memcmp(w, r, sizeof w) != 0)
		goto fail;

	/* a 300-byte overwrite inside one block must preserve the rest */
	for(i = 0; i < 512; i++)
		w[i] = i & 0xff;
	if(pwrite(fd, w, 512, 17000LL*512) != 512)
		goto fail;
	for(i = 0; i < 300; i++)
		w[i] = (i * 3 + 1) & 0xff;
	if(pwrite(fd, w, 300, 17000LL*512 + 100) != 300)
		goto fail;
	memset(r, 0, sizeof r);
	if(pread(fd, r, 512, 17000LL*512) != 512)
		goto fail;
	for(i = 0; i < 100; i++)
		if(r[i] != (i & 0xff))
			goto fail;
	for(i = 0; i < 300; i++)
		if(r[100+i] != ((i * 3 + 1) & 0xff))
			goto fail;
	for(i = 400; i < 512; i++)
		if(r[i] != (i & 0xff))
			goto fail;

	close(fd);

	/* durability: the flush file runs EXT_CSD FLUSH_CACHE */
	fd = open("/mnt/emmc/flush", OWRITE);
	if(fd < 0)
		return -1;
	if(write(fd, "f", 1) != 1){
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
fail:
	close(fd);
	return -1;
}

/*
 * Format the data partition once, from the boot archive.  This runs
 * before the re-root so the blob comes from the archive rather than
 * from the ext2 root, where a 14 MiB file crosses the indirect-block
 * boundary of the ext2 reader.  provision exits immediately when the
 * partition already carries an ext2 superblock.
 */
static int
provision(void)
{
	char *argv[] = { "/boot/provision", "/boot/datameta.bin",
			 "/mnt/emmc/part2", nil };
	int pid, ok;
	Waitmsg *w;

	if(access("/boot/datameta.bin", AEXIST) < 0)
		return -1;
	if(access("/mnt/emmc/part2", AEXIST) < 0)
		return -1;
	pid = rfork(RFPROC|RFFDG);
	if(pid == 0){
		exec("/boot/provision", argv);
		_exits("exec");
	}
	if(pid < 0)
		return -1;
	w = wait();
	if(w == nil)
		return -1;
	ok = w->msg[0] == 0;
	free(w);
	return ok ? 0 : -1;
}

/*
 * Mount the large data partition at /mnt/data.  This runs after the
 * re-root, so it re-attaches emmcfs (the /mnt of the new root is the
 * RAM filesystem and knows nothing about the earlier mount).
 */
static int
datamount(void)
{
	int fd;

	create("/mnt/data", OREAD, DMDIR|0777);
	fd = open("/srv/gefs", ORDWR);
	if(fd < 0)
		return -1;
	if(mount(fd, -1, "/mnt/data", MREPL|MCREATE, "%") < 0){
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/*
 * Start the server for the big data partition.  This has to happen
 * before the re-root: starting a lib9p server after "/" has been
 * replaced with the eMMC filesystem does not work in this kernel, and
 * the same restriction is why rc cannot launch a new file server.
 * Mounting later is just a mount(2) call and works fine.
 */
static int
dataserve(void)
{
	/*
	 * The big partition is gefs.  -A allows an unauthenticated
	 * attach; -S (permissive) lets the admin mount (% spec) bypass
	 * the permission check on the adm-owned root.
	 */
	char *dataargv[] = { "/boot/gefs", "-A", "-S", "-m", "64",
			     "-f", "/mnt/emmc/part2", nil };
	int pid;

	if(access("/mnt/emmc/part2", AEXIST) < 0)
		return -1;
	pid = rfork(RFPROC|RFNOWAIT|RFFDG|RFREND);
	if(pid == 0){
		exec("/boot/gefs", dataargv);
		_exits("exec");
	}
	if(pid < 0 || waitfor("/srv/gefs", 100) < 0)
		return -1;
	return 0;
}

void
main(int argc, char **argv)
{
	long m;
	int fd, pid;

	USED(argc);
	USED(argv);
	open("#c/cons", OREAD);
	open("#c/cons", OWRITE);
	open("#c/cons", OWRITE);
	say("HARVEY-ARM64 FILE-EXEC INIT PASS\n");
	if(bind("#c", "/dev", MAFTER) < 0){
		say("HARVEY-ARM64 DRIVER-NAMESPACE FAIL\n");
		_exits("namespace");
	}
	say("HARVEY-ARM64 DRIVER-DEV PASS\n");
	if(bind("#V", "/dev", MAFTER) < 0)
		say("HARVEY-ARM64 DISPLAY-BIND FAIL\n");
	else
		say("HARVEY-ARM64 DISPLAY-BIND PASS\n");
	if(bind("#e", "/env", MCREATE|MAFTER) < 0 ||
	   bind("#s", "/srv", MREPL|MCREATE) < 0 ||
	   bind("#@", "/mnt", MREPL|MCREATE) < 0){
		say("HARVEY-ARM64 DRIVER-NAMESPACE FAIL\n");
		_exits("namespace");
	}
	say("HARVEY-ARM64 DRIVER-NAMESPACE PASS\n");
	fd = create("/mnt/o9drvman-status", OREAD, DMDIR|0755);
	if(fd >= 0)
		close(fd);
	{
		char *drvmansrv[] = { "/boot/drvman", "drvman", nil };
		char status[16];
		pid = rfork(RFPROC|RFNOWAIT|RFFDG|RFNAMEG|RFENVG|RFREND|RFNOTEG);
		if(pid == 0){
			exec(drvmansrv[0], drvmansrv);
			say("HARVEY-ARM64 DRVMAN EXEC FAIL\n");
			_exits("exec");
		}
		if(pid < 0){
			say("HARVEY-ARM64 DRIVER-MANAGER FAIL\n");
			_exits("manager");
		}
		status[0] = 0;
		for(fd = 0; fd < 100; fd++){
			int sfd = open("/mnt/o9drvman-status/complete", OREAD);
			if(sfd >= 0){
				m = read(sfd, status, sizeof status - 1);
				close(sfd);
				if(m > 0){
					status[m] = 0;
					break;
				}
			}
			sleep(50);
		}
		if(strncmp(status, "pass", 4) != 0){
			say("HARVEY-ARM64 DRIVER-MANAGER FAIL\n");
			_exits("manager");
		}
	}
	say("HARVEY-ARM64 DRIVER-MANAGER PASS\n");
	{
		char *hubsrv[] = { "/boot/hubfs", "hubfs", nil };
		pid = rfork(RFPROC|RFNOWAIT|RFFDG|RFNAMEG|RFENVG|RFREND|RFNOTEG);
		if(pid == 0){
			exec(hubsrv[0], hubsrv);
			_exits("exec");
		}
		if(pid < 0 || waitfor("/srv/hubfs", 50) < 0)
			say("HARVEY-ARM64 HUBFS FAIL\n");
		else if(hubtest() == 0)
			say("HARVEY-ARM64 HUBFS PASS\n");
		else
			say("HARVEY-ARM64 HUBFS FAIL\n");
	}
	{
		char *emmcfsargv[] = { "/boot/emmcfs", "emmcfs", nil };
		pid = rfork(RFPROC|RFNOWAIT|RFFDG|RFNAMEG|RFENVG|RFREND|RFNOTEG);
		if(pid == 0){
			exec(emmcfsargv[0], emmcfsargv);
			say("HARVEY-ARM64 EMMCFS EXEC FAIL\n");
			_exits("exec");
		}
		if(pid < 0 || waitfor("/srv/emmc", 200) < 0)
			say("HARVEY-ARM64 EMMCFS FAIL\n");
		else if(emmctest() == 0){
			say("HARVEY-ARM64 EMMCFS PASS\n");
			/*
			 * The eMMC is one gefs filesystem.  Format the data
			 * partition if it is blank, start the server, and mount
			 * it at /mnt/data.  The root stays the boot-archive
			 * initramfs, so there is no separate root filesystem and
			 * no re-root.
			 */
			if(provision() == 0)
				say("HARVEY-ARM64 EMMC-PROVISION PASS\n");
			else
				say("HARVEY-ARM64 EMMC-PROVISION SKIP\n");
			if(dataserve() == 0)
				say("HARVEY-ARM64 EMMC-DATASRV PASS\n");
			else
				say("HARVEY-ARM64 EMMC-DATASRV FAIL\n");
			if(datamount() == 0)
				say("HARVEY-ARM64 EMMC-DATA PASS\n");
			else
				say("HARVEY-ARM64 EMMC-DATA SKIP\n");
		}else
			say("HARVEY-ARM64 EMMCFS FAIL\n");
	}
	{
		char ctl[256];
		int fd, m;

		char msg[96];

		create("/net", OREAD, DMDIR|0755);
		/*
		 * Attach the ip device once before binding: the first
		 * bind on a cold device fails inside namec on this kernel.
		 */
		if((fd = open("#I", OREAD)) < 0){
			snprint(msg, sizeof msg, "HARVEY-ARM64 NET-IP FAIL %r\n");
			say(msg);
		}else
			close(fd);
		if(bind("#I0", "/net", MREPL|MCREATE) < 0){
			snprint(msg, sizeof msg, "HARVEY-ARM64 NET-IP FAIL %r\n");
			say(msg);
		}else if(bind("#l", "/net", MAFTER|MCREATE) < 0){
			snprint(msg, sizeof msg, "HARVEY-ARM64 NET-ETH FAIL %r\n");
			say(msg);
		}else if((fd = open("/net/ether0/addr", OREAD)) < 0)
			say("HARVEY-ARM64 NET-BIND FAIL\n");
		else{
			m = read(fd, ctl, sizeof ctl - 1);
			close(fd);
			if(m > 0){
				ctl[m] = 0;
				say("HARVEY-ARM64 NET-BIND PASS\n");
			}else
				say("HARVEY-ARM64 NET-BIND FAIL\n");
		}
	}
	{
		char buf[256];
		int fd, m, ok;

		ok = 1;
		if((fd = open("/net/ipifc/clone", ORDWR)) < 0){
			snprint(buf, sizeof buf, "HARVEY-ARM64 NET-IFC FAIL open %r\n");
			say(buf);
			ok = 0;
		}else{
			if(read(fd, buf, sizeof buf - 1) < 0){
				snprint(buf, sizeof buf, "HARVEY-ARM64 NET-IFC FAIL clone %r\n");
				say(buf);
				ok = 0;
			}
			if(ok && write(fd, "bind ether /net/ether0", 22) < 0){
				snprint(buf, sizeof buf, "HARVEY-ARM64 NET-IFC FAIL bind %r\n");
				say(buf);
				ok = 0;
			}
			if(ok && write(fd, "add 192.168.2.248 255.255.255.0", 31) < 0){
				snprint(buf, sizeof buf, "HARVEY-ARM64 NET-IFC FAIL add %r\n");
				say(buf);
				ok = 0;
			}
			close(fd);
		}
		if(ok){
			if((fd = open("/net/ipifc/0/status", OREAD)) < 0)
				say("HARVEY-ARM64 NET-IFC FAIL status\n");
			else{
				m = read(fd, buf, sizeof buf - 1);
				close(fd);
				if(m > 0){
					buf[m] = 0;
					say("HARVEY-ARM64 NET-IFC PASS\n");
				}else
					say("HARVEY-ARM64 NET-IFC FAIL status\n");
			}
		}
	}
	/* Board-initiated ICMP echo: proves route lookup, ARP and TX.  The
	 * probe runs in a child so a lost reply cannot stop init. */
	pid = rfork(RFPROC|RFFDG|RFNOTEG);
	if(pid == 0){
		char path[64], num[32], msg[128];
		int cfd, dfd, n;
		uchar pkt[28];

		memset(pkt, 0, sizeof pkt);
		pkt[20] = 8;			/* EchoRequest after the IP header */
		pkt[24] = 0x48;
		pkt[25] = 0x41;			/* identifier */
		pkt[27] = 1;			/* sequence */

		if((cfd = open("/net/icmp/clone", ORDWR)) < 0)
			say("HARVEY-ARM64 NET-PING FAIL clone\n");
		else{
			n = read(cfd, num, sizeof num - 1);
			if(n <= 0 || write(cfd, "connect 192.168.2.1!1", 21) < 0){
				snprint(msg, sizeof msg, "HARVEY-ARM64 NET-PING FAIL connect %r\n");
				say(msg);
			}
			else{
				num[n] = 0;
				snprint(path, sizeof path, "/net/icmp/%s/data", num);
				if((dfd = open(path, ORDWR)) < 0){
					snprint(msg, sizeof msg, "HARVEY-ARM64 NET-PING FAIL open %r\n");
					say(msg);
				}else{
					if(write(dfd, pkt, sizeof pkt) != sizeof pkt)
						say("HARVEY-ARM64 NET-PING FAIL write\n");
					else if(read(dfd, msg, sizeof msg) <= 0)
						say("HARVEY-ARM64 NET-PING FAIL read\n");
					else
						say("HARVEY-ARM64 NET-PING PASS\n");
					close(dfd);
				}
			}
			close(cfd);
		}
		_exits("ping");
	}
	{
		char *rcargv[] = { "/boot/rc", "-i", "-m", "/boot/rcmain", nil };
		say("HARVEY-ARM64 RC START\n");
		for(;;){
			pid = rfork(RFPROC|RFFDG|RFNOTEG);
			if(pid == 0){
				exec(rcargv[0], rcargv);
				say("HARVEY-ARM64 RC EXEC FAIL\n");
				_exits("rc");
			}
			if(pid < 0){
				say("HARVEY-ARM64 RC LAUNCH FAIL\n");
				break;
			}
			if(waitpid() < 0)
				sleep(1000);
		}
	}
	for(;;)
		sleep(1000);
}
''', INIT_BASE, libc)
    o9drvman, _ = validate_elf(ROOT / "build/arm64-o9/o9drvman", O9DRVMAN_BASE)
    o9xhci, _ = validate_elf(ROOT / "build/arm64-o9/o9xhci", O9XHCI_BASE)
    testxhci, _ = validate_elf(ROOT / "build/arm64-o9/test-xhci", TESTXHCI_BASE)
    testxhci1, _ = validate_elf(ROOT / "build/arm64-o9/test-xhci1", TESTXHCI1_BASE)
    testaudio, _ = validate_elf(ROOT / "build/arm64-o9/test-audio", TESTAUDIO_BASE)
    uartfs, _ = build_uartfs(out, libc)
    gpiofs, _ = build_gpiofs(out, libc)
    gpufs, _ = build_gpufs(out, libc)
    hubfs, _ = build_hubfs(out, libc)
    drvman, _ = build_drvman(out, libc)
    i2cfs, _ = build_i2cfs(out, libc)
    pmicfs, _ = build_pmicfs(out, libc)
    mkdirbin, _ = build_elf(out, libc, "mkdir", ROOT / "sys/src/cmd/mkdir.c",
                            MKDIR_BASE, [])
    mount9bin, _ = build_elf(out, libc, "mount9", ROOT / "sys/src/cmd/mount9.c",
                            MOUNT9_BASE, [])
    mountbin, _ = build_elf(out, libc, "mount", ROOT / "sys/src/cmd/mount.c",
                            MOUNT_BASE,
                            [ROOT / "build/arm64-o9/libauth.a",
                             ROOT / "build/arm64-o9/libauthsrv.a",
                             ROOT / "build/arm64-o9/libsec.a",
                             ROOT / "build/arm64-o9/libndb.a"])
    emmcfs, _ = build_emmcfs(out, libc)
    ext2srv, _ = build_ext2srv(out, libc)
    gmacfs, _ = build_gmacfs(out, libc)
    usbpower, _ = build_usbpower(out, libc)
    bigtest, _ = build_program(out, "bigtest", r'''#include <u.h>
#include <libc.h>

/* Large enough that the file crosses whatever limit exec runs into. */
static char pad[400000] = { 1 };

void
main(void)
{
	pad[1] = pad[0];
	print("BIGTEST OK %d\n", pad[1]);
	exits(nil);
}
''', BIGTEST_BASE, libc)
    rc, rc_entry = build_rc(out, libc)
    echo, _ = build_program(out, "echo", r'''#include <u.h>
#include <libc.h>

void
main(int argc, char **argv)
{
	int i;

	for(i = 1; i < argc; i++){
		if(i > 1)
			write(1, " ", 1);
		write(1, argv[i], strlen(argv[i]));
	}
	write(1, "\n", 1);
	exits(nil);
}
''', ECHO_BASE, libc)
    cat, _ = build_program(out, "cat", r'''#include <u.h>
#include <libc.h>

void
main(int argc, char **argv)
{
	char buf[4096];
	int fd, n, i;

	if(argc < 2){
		fprint(2, "usage: cat file...\n");
		exits("usage");
	}
	for(i = 1; i < argc; i++){
		fd = open(argv[i], OREAD);
		if(fd < 0){
			fprint(2, "cat: %s: %r\n", argv[i]);
			exits("open");
		}
		while((n = read(fd, buf, sizeof buf)) > 0)
			if(write(1, buf, n) != n){
				fprint(2, "cat: write: %r\n");
				exits("write");
			}
		close(fd);
	}
	exits(nil);
}
''', CAT_BASE, libc)
    ls, _ = build_program(out, "ls", r'''#include <u.h>
#include <libc.h>

static void
show(char *dir)
{
	Dir *d;
	int fd, i, n;
	char path[512];

	fd = open(dir, OREAD);
	if(fd < 0){
		fprint(2, "ls: %s: %r\n", dir);
		return;
	}
	n = dirread(fd, &d);
	if(n < 0){
		fprint(1, "%s\n", dir);
		close(fd);
		return;
	}
	while(n > 0){
		for(i = 0; i < n; i++){
			if(strcmp(dir, "/") == 0)
				snprint(path, sizeof path, "/%s", d[i].name);
			else
				snprint(path, sizeof path, "%s/%s", dir, d[i].name);
			fprint(1, "%-24s %8lld %s%s%s\n", d[i].name,
			       d[i].length,
			       (d[i].mode & DMDIR) ? "d" : "-",
			       (d[i].mode & 0400) ? "r" : "-",
			       (d[i].mode & 0200) ? "w" : "-");
		}
		free(d);
		n = dirread(fd, &d);
	}
	close(fd);
}

void
main(int argc, char **argv)
{
	int i;

	if(argc < 2){
		show(".");
		exits(nil);
	}
	for(i = 1; i < argc; i++)
		show(argv[i]);
	exits(nil);
}
''', LS_BASE, libc)
    ping, _ = build_program(out, "ping", r'''#include <u.h>
#include <libc.h>

/*
 * ping: ICMP echo through /net/icmp.
 *
 * The kernel does not deliver notes to user handlers here, so there is
 * no alarm() to interrupt a blocked read.  Instead each echo is sent
 * and the EchoReply counter in /net/icmp/stats is polled for a while;
 * only when it moves is the reply read, which then cannot block.
 */

enum {
	Iphsz	= 20,
	Icmphsz	= 8,
	Payload	= 56,
	Pktsz	= Iphsz + Icmphsz + Payload,
};

static int statfd = -1;

/* Input EchoReply count from /net/icmp/stats, or -1 if unavailable. */
static int
replies(void)
{
	char buf[4096], *p;
	int n;

	if(statfd < 0){
		if((statfd = open("/net/icmp/stats", OREAD)) < 0)
			return -1;
	}
	if(seek(statfd, 0, 0) < 0)
		return -1;
	n = read(statfd, buf, sizeof buf - 1);
	if(n <= 0)
		return -1;
	buf[n] = 0;
	if((p = strstr(buf, "EchoReply:")) == nil)
		return -1;
	return strtoul(p + 10, nil, 10);
}

static void
usage(void)
{
	fprint(2, "usage: ping [-n count] [-i interval] [-t timeout] host\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char host[256], num[32], path[64], pkt[Pktsz], buf[2048], *a;
	int cfd, dfd, i, j, n, seq, sent, recv, count, interval, timeout;
	int before, now;
	uvlong t0, rtt;

	count = 3;
	interval = 1000;
	timeout = 2000;
	host[0] = 0;
	for(i = 1; i < argc; i++){
		a = argv[i];
		if(a[0] == '-' && a[1] != 0){
			if(i + 1 >= argc)
				usage();
			switch(a[1]){
			case 'n': count = atoi(argv[++i]); break;
			case 'i': interval = atoi(argv[++i]); break;
			case 't': timeout = atoi(argv[++i]); break;
			default: usage();
			}
			continue;
		}
		if(host[0] != 0)
			usage();
		snprint(host, sizeof host, "%s", a);
	}
	if(host[0] == 0 || count <= 0 || interval < 0 || timeout <= 0)
		usage();

	if((cfd = open("/net/icmp/clone", ORDWR)) < 0)
		sysfatal("ping: clone: %r");
	n = read(cfd, num, sizeof num - 1);
	if(n <= 0)
		sysfatal("ping: clone: %r");
	num[n] = 0;
	snprint(buf, sizeof buf, "connect %s!1", host);
	if(write(cfd, buf, strlen(buf)) < 0)
		sysfatal("ping: connect %s: %r", host);
	snprint(path, sizeof path, "/net/icmp/%s/data", num);
	if((dfd = open(path, ORDWR)) < 0)
		sysfatal("ping: %s: %r", path);

	print("PING %s: %d data bytes\n", host, Payload);
	sent = recv = 0;
	for(seq = 1; seq <= count; seq++){
		memset(pkt, 0, sizeof pkt);
		pkt[Iphsz] = 8;			/* EchoRequest */
		pkt[Iphsz + 6] = (seq >> 8) & 0xff;
		pkt[Iphsz + 7] = seq & 0xff;
		for(j = 0; j < Payload; j++)
			pkt[Iphsz + Icmphsz + j] = j;

		before = replies();
		t0 = nsec();
		if(write(dfd, pkt, sizeof pkt) != sizeof pkt){
			print("ping: write: %r\n");
			break;
		}
		sent++;

		if(before < 0){
			/* stats unavailable: ride the reply queue directly */
			n = read(dfd, buf, sizeof buf);
			now = n > 0;
		}else{
			uvlong w0;

			/*
			 * sleep() rounds up to the clock tick (10 ms here),
			 * which would show up as the round-trip time.  Poll
			 * tightly for the first few milliseconds, then let
			 * the scheduler take over.
			 */
			now = 0;
			w0 = nsec();
			for(;;){
				if(replies() > before){
					now = 1;
					break;
				}
				if(nsec() - w0 >= (uvlong)timeout * 1000000)
					break;
				if(nsec() - w0 > 5000000)
					sleep(1);
			}
			if(now)
				n = read(dfd, buf, sizeof buf);
		}

		if(now && n > 0){
			rtt = nsec() - t0;
			recv++;
			print("%d bytes from %s: seq %d time %llud.%03llud ms\n",
			      n, host, seq, rtt / 1000000, (rtt / 1000) % 1000);
		}else
			print("ping: seq %d timeout\n", seq);

		if(seq < count && interval > 0)
			sleep(interval);
	}
	print("%d packets transmitted, %d received, %d%% packet loss\n",
	      sent, recv, sent ? (sent - recv) * 100 / sent : 0);
	exits(nil);
}
''', PING_BASE, libc)
    rtc, _ = build_program(out, "rtc", r'''#include <u.h>
#include <libc.h>

/* RK809 (RK808 family) real-time clock through the i2c file server.
 *   rtc            print the date and set /dev/time from it
 *   rtc poweroff   drive DEVCTRL.DEV_OFF
 *   rtc id         print the first registers of the PMIC
 */

static int ctl = -1, dat = -1;

static int
hexval(int c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int
rd(u8 addr, u8 reg, u8 *buf, int n)
{
	char cmd[32], h[128];
	int i, m, a, b;

	snprint(cmd, sizeof cmd, "r %d %d %d", addr, reg, n);
	if(write(ctl, cmd, strlen(cmd)) != strlen(cmd))
		return -1;
	m = read(dat, h, sizeof h - 1);
	if(m < 2*n)
		return -1;
	h[m] = 0;
	for(i = 0; i < n; i++){
		a = hexval(h[2*i]);
		b = hexval(h[2*i+1]);
		if(a < 0 || b < 0)
			return -1;
		buf[i] = (a<<4) | b;
	}
	return 0;
}

static int
wr(u8 addr, u8 *b, int n)
{
	char cmd[64];
	int i, k;

	k = snprint(cmd, sizeof cmd, "w %d", addr);
	for(i = 0; i < n; i++)
		k += snprint(cmd+k, sizeof cmd-k, " %d", b[i]);
	return write(ctl, cmd, strlen(cmd)) == strlen(cmd) ? 0 : -1;
}

static int
bcd(u8 v)
{
	return (v>>4)*10 + (v & 15);
}

static int
leap(int y)
{
	return y%4 == 0 && (y%100 != 0 || y%400 == 0);
}

static long
days(int y, int m, int d)
{
	static int cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
	long n;

	n = (y-1970)*365L + (y-1969)/4;
	n += cum[m-1];
	if(m > 2 && leap(y))
		n++;
	return n + d - 1;
}

static int
findbus(void)
{
	char buf[1024], *p, *q;
	int fd, n;

	fd = open("/mnt/i2c/status", OREAD);
	if(fd < 0)
		return -1;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		return -1;
	buf[n] = 0;
	for(p = buf; (p = strstr(p, "base 0x")) != nil; p++){
		if(strtoul(p+7, nil, 16) != 0xfdd40000)
			continue;
		q = p;
		while(q > buf && q[-1] != '\n')
			q--;
		if(strncmp(q, "i2c", 3) == 0)
			return atoi(q+3);
	}
	return -1;
}

static void
fail(char *s)
{
	fprint(2, "rtc: %s\n", s);
	exits(s);
}

void
main(int argc, char **argv)
{
	char ctlpath[64], datpath[64], b[32];
	u8 t[7], v[2];
	int bus, fd, y, mo, d, h, mi, sec, n;
	long epoch;

	if(access("/mnt/i2c", AEXIST) < 0)
		create("/mnt/i2c", OREAD, DMDIR|0755);
	fd = open("/srv/i2c", OREAD);
	if(fd < 0)
		fail("no i2c service");
	if(mount(fd, -1, "/mnt/i2c", MREPL|MCREATE, "") < 0)
		fail("cannot mount i2c");
	close(fd);
	bus = findbus();
	if(bus < 0)
		fail("PMIC bus not found");
	snprint(ctlpath, sizeof ctlpath, "/mnt/i2c/i2c%d/ctl", bus);
	snprint(datpath, sizeof datpath, "/mnt/i2c/i2c%d/data", bus);
	ctl = open(ctlpath, ORDWR);
	dat = open(datpath, OREAD);
	if(ctl < 0 || dat < 0)
		fail("PMIC bus not usable");

	if(argc > 1 && strcmp(argv[1], "poweroff") == 0){
		v[0] = 0x4b;
		v[1] = 0x01;
		if(wr(0x20, v, 2) < 0)
			fail("poweroff failed");
		fprint(2, "rtc: poweroff requested\n");
		exits(nil);
	}
	if(argc > 1 && strcmp(argv[1], "id") == 0){
		if(rd(0x20, 0, t, 2) < 0)
			fail("read failed");
		print("pmic id %02ux %02ux\n", t[0], t[1]);
		exits(nil);
	}

	if(rd(0x20, 0, t, 7) < 0)
		fail("RTC read failed");
	sec = bcd(t[0] & 0x7f);
	mi = bcd(t[1] & 0x7f);
	h = bcd(t[2] & 0x3f);
	d = bcd(t[3] & 0x3f);
	mo = bcd(t[4] & 0x1f);
	y = 2000 + bcd(t[5]);
	if(mo < 1 || mo > 12 || d < 1 || d > 31)
		fail("RTC not set");
	epoch = days(y, mo, d)*86400L + h*3600 + mi*60 + sec;
	print("%04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, sec);

	fd = open("/dev/time", OWRITE);
	if(fd >= 0){
		n = snprint(b, sizeof b, "%ld", epoch);
		if(write(fd, b, n) != n)
			fprint(2, "rtc: cannot set /dev/time\n");
		close(fd);
	}
	exits(nil);
}
''', RTC_BASE, libc)
    # gefs is mid-port: it needs Plan 9's anonymous-struct member
    # flattening rewritten for C.  Build it only when asked.
    if os.environ.get("ARM64_BUILD_GEFS") == "1":
        gefs, _ = build_gefs(out, libc)
    provision, _ = build_elf(out, libc, "provision",
                             ROOT / "sys/src/cmd/emmc/provision.c",
                             PROVISION_BASE, [])
    emmc, _ = build_elf(out, libc, "emmc",
                        ROOT / "sys/src/cmd/emmc/emmc.c", EMMC_BASE, [])
    gmac, _ = build_elf(out, libc, "gmac",
                        ROOT / "sys/src/cmd/gmac/gmac.c", GMAC_BASE, [])
    sdprobe, _ = build_elf(out, libc, "sdprobe",
                           ROOT / "sys/src/cmd/sdio/sdprobe.c", SDPROBE_BASE, [])
    sdcard, _ = build_elf(out, libc, "sdcard",
                          ROOT / "sys/src/cmd/sdcard/sdcard.c", SDCARD_BASE, [])
    keyrootprobe, _ = build_elf(out, libc, "opteeprobe",
                                ROOT / "sys/src/cmd/auth/keyroot/opteeprobe.c",
                                KEYROOT_PROBE_BASE, build_pq_objs(out))
    factotum, _ = build_factotum(out, libc)
    factotumkey, _ = build_elf(out, libc, "factotumkey",
                               ROOT / "sys/src/cmd/auth/factotumkeytest.c",
                               FACTOTUMKEY_BASE, [])
    keyrootlibs = [
        ROOT / "build/arm64-o9/lib9p.a",
        ROOT / "build/arm64-o9/libthread.a",
        ROOT / "build/arm64-o9/libauth.a",
        ROOT / "build/arm64-o9/libauthsrv.a",
        ROOT / "build/arm64-o9/libsec.a",
        ROOT / "build/arm64-o9/libString.a",
        ROOT / "build/arm64-o9/libip.a",
        ROOT / "build/arm64-o9/libmp.a",
    ]
    keyrootsvc, _ = build_elf(out, libc, "keyroot",
                              ROOT / "sys/src/cmd/auth/keyroot/keyroot.c",
                              KEYROOT_BASE, keyrootlibs,
                              ldflags=["--defsym=threadmain=main"])
    keyroottest, _ = build_elf(out, libc, "keyroottest",
                               ROOT / "sys/src/cmd/auth/keyroottest.c",
                               KEYROOTTEST_BASE, [])
    dp9iktest, _ = build_elf(out, libc, "dp9iktest",
                             ROOT / "sys/src/cmd/auth/dp9iktest.c",
                             DP9IKTEST_BASE,
                             [ROOT / "build/arm64-o9/libmp.a",
                              ROOT / "build/arm64-o9/libauthsrv.a",
                              ROOT / "build/arm64-o9/libsec.a"])
    displaytest, _ = build_program(out, "displaytest", r'''#include <u.h>
#include <libc.h>

static int
wr(char *path, char *text)
{
	int fd, n, len;

	len = strlen(text);
	fd = open(path, OWRITE);
	if(fd < 0)
		return -1;
	n = write(fd, text, len);
	close(fd);
	return n == len ? 0 : -1;
}

void
main(int argc, char **argv)
{
	int fd;
	char *mnt = "/mnt/display";

	USED(argv);
	if(argc > 1 && strcmp(argv[1], "peek") == 0){
		long off, at;
		u32 pixel;
		fd = open("/dev/display/data", OREAD);
		if(fd < 0){
			print("DISPLAYTEST PEEK OPEN FAIL\n");
			exits("open");
		}
		for(off = 0; off < 8294400; off += 2073600){
			at = off;
			if(seek(fd, at, 0) >= 0 && read(fd, &pixel, 4) == 4)
				print("DISPLAYTEST PIXEL %ld %ux\n", at, pixel);
		}
		close(fd);
		exits(nil);
	}
	if(argc > 1 && strcmp(argv[1], "scan") == 0){
		int x, y, bad;
		long at;
		u32 pixel;
		fd = open("/dev/display/data", OREAD);
		if(fd < 0){
			print("DISPLAYTEST SCAN OPEN FAIL\n");
			exits("open");
		}
		bad = 0;
		for(y = 0; y < 1080; y += 60){
			for(x = 0; x < 1920; x += 60){
				at = (long)y * 7680 + (long)x * 4;
				if(seek(fd, at, 0) >= 0 && read(fd, &pixel, 4) == 4 &&
				   pixel != 0xff00ff00){
					if(bad < 40)
						print("DISPLAYTEST BLACK %d %d %ux\n",
						      x, y, pixel);
					bad++;
				}
			}
		}
		close(fd);
		print("DISPLAYTEST SCAN BAD %d\n", bad);
		exits(nil);
	}
	create(mnt, OREAD, DMDIR|0755);
	fd = open("/srv/DisplayViews", OREAD);
	if(fd < 0 || mount(fd, -1, mnt, MREPL|MCREATE, "") < 0){
		if(fd >= 0)
			close(fd);
		print("DISPLAYTEST MOUNT FAIL\n");
		exits("mount");
	}
	close(fd);
	if(wr("/mnt/display/view/ctl", "buffer 480 270\n") < 0){
		print("DISPLAYTEST BUFFER FAIL\n");
		exits("buffer");
	}
	print("DISPLAYTEST BUFFER PASS\n");
	if(wr("/mnt/display/view/buffers/1/ctl", "clear 65280\n") < 0){
		print("DISPLAYTEST CLEAR FAIL\n");
		exits("clear");
	}
	print("DISPLAYTEST CLEAR PASS\n");
	{
		char path[64], cmd[64];
		int id, row, col;
		id = 1;
		for(row = 0; row < 4; row++){
			for(col = 0; col < 4; col++){
				if(wr("/mnt/display/view/ctl", "surface 480 270\n") < 0){
					print("DISPLAYTEST SURFACE FAIL\n");
					exits("surface");
				}
				snprint(path, sizeof path, "/mnt/display/view/surfaces/%d/ctl", id);
				snprint(cmd, sizeof cmd, "place %d %d\n", col * 480, row * 270);
				if(wr(path, cmd) < 0 || wr(path, "attach 1\n") < 0 ||
				   wr(path, "commit\n") < 0){
					print("DISPLAYTEST TILE FAIL\n");
					exits("tile");
				}
				{
					int dfd;
					u32 pixel;
					long at;
					dfd = open("/dev/display/data", OREAD);
					at = (row * 270 + 135) * 7680 + (col * 480 + 240) * 4;
					if(dfd >= 0 && seek(dfd, at, 0) >= 0 &&
					   read(dfd, &pixel, 4) == 4)
						print("DISPLAYTEST TILE %d %d %d %ux\n",
						      col, row, id, pixel);
					if(dfd >= 0)
						close(dfd);
				}
				id++;
			}
		}
	}
	print("DISPLAYTEST TILES PASS\n");
	fd = open("/dev/display/data", OREAD);
	if(fd >= 0){
		long off, at;
		u32 pixel;
		for(off = 0; off < 8294400; off += 2073600){
			at = off;
			if(seek(fd, at, 0) >= 0 && read(fd, &pixel, 4) == 4)
				print("DISPLAYTEST PIXEL %ld %ux\n", at, pixel);
		}
		close(fd);
	}
	print("DISPLAYTEST PASS\n");
	exits(nil);
}
''', DISPLAYTEST_BASE, libc)
    rcmain = (ROOT / "rc/lib/rcmain").read_text()
    rcmain = rcmain.replace("path=(. /bin)", "path=(/boot . /bin)")
    lines = [
        "/* generated by util/arm64/build_userland.py */",
        f"#define ARM64EXECSTUBBASE 0x{STUB_BASE:x}ULL",
        f"#define ARM64EXECSTUBENTRY 0x{stub_entry:x}ULL",
        f"#define ARM64INITENTRY 0x{init_entry:x}ULL",
        f"#define ARM64RCENTRY 0x{rc_entry:x}ULL",
    ]
    emit_array(lines, "arm64execstubelf", stub)
    emit_array(lines, "arm64initelf", init)
    emit_array(lines, "arm64o9drvmanelf", o9drvman)
    emit_array(lines, "arm64o9xhcielf", o9xhci)
    emit_array(lines, "arm64testxhcielf", testxhci)
    emit_array(lines, "arm64testxhci1elf", testxhci1)
    emit_array(lines, "arm64testaudioelf", testaudio)
    emit_array(lines, "arm64uartfself", uartfs)
    emit_array(lines, "arm64gpufself", gpufs)
    emit_array(lines, "arm64gpiofself", gpiofs)
    emit_array(lines, "arm64hubfself", hubfs)
    emit_array(lines, "arm64drvmanelf", drvman)
    emit_array(lines, "arm64i2cfself", i2cfs)
    emit_array(lines, "arm64pmicfself", pmicfs)
    emit_array(lines, "arm64mkdirelf", mkdirbin)
    emit_array(lines, "arm64mountelf", mountbin)
    emit_array(lines, "arm64mount9elf", mount9bin)
    emit_array(lines, "arm64emmcfsimage", emmcfs)
    emit_array(lines, "arm64gmacfsimage", gmacfs)
    emit_array(lines, "arm64usbpowerelf", usbpower)
    emit_array(lines, "arm64rcelf", rc)
    emit_array(lines, "arm64echoelf", echo)
    emit_array(lines, "arm64catelf", cat)
    emit_array(lines, "arm64lself", ls)
    emit_array(lines, "arm64pingelf", ping)
    emit_array(lines, "arm64rtcelf", rtc)
    emit_array(lines, "arm64emmcelf", emmc)
    emit_array(lines, "arm64gmacelf", gmac)
    emit_array(lines, "arm64sdprobeelf", sdprobe)
    emit_array(lines, "arm64opteeprobeelf", keyrootprobe)
    emit_array(lines, "arm64dp9iktestelf", dp9iktest)
    emit_array(lines, "arm64factotumelf", factotum)
    emit_array(lines, "arm64factotumkeyelf", factotumkey)
    emit_array(lines, "arm64keyrootelf", keyrootsvc)
    emit_array(lines, "arm64keyroottestelf", keyroottest)
    emit_array(lines, "arm64displaytestelf", displaytest)
    emit_array(lines, "arm64rcmain", rcmain.encode())
    # The gefs bring-up build embeds the files the re-root path needs
    # (emmcfs, ext2srv) and gefs itself, so the kernel is self-contained
    # and does not depend on a second TFTP transfer.
    if os.environ.get("ARM64_BUILD_GEFS") == "1":
        emit_array(lines, "arm64emmcfs", emmcfs)
        emit_array(lines, "arm64ext2srvelf", ext2srv)
        emit_array(lines, "arm64gmacfself", gmacfs)
        emit_array(lines, "arm64provisionelf", provision)
        emit_array(lines, "arm64bigtestelf", bigtest)
        emit_array(lines, "arm64gefs", gefs)
        lines.append("#define ARM64_EMBED_GEFS 1")
    args.output.write_text("\n".join(lines))
    programs = {
        "o9drvman": o9drvman,
        "o9xhci": o9xhci,
        "test-xhci": testxhci,
        "test-xhci1": testxhci1,
        "test-audio": testaudio,
        "uartfs": uartfs,
        "gpufs": gpufs,
        "gpiofs": gpiofs,
        "hubfs": hubfs,
        "drvman": drvman,
        "i2cfs": i2cfs,
        "pmicfs": pmicfs,
        "mkdir": mkdirbin,
        "mount": mountbin,
        "mount9": mount9bin,
        "emmcfs": emmcfs,
        "ext2srv": ext2srv,
        "gmacfs": gmacfs,
        "usbpower": usbpower,
        "rc": rc,
        "echo": echo,
        "cat": cat,
        "ls": ls,
        "ping": ping,
        "rtc": rtc,
        "emmc": emmc,
        "provision": provision,
        "bigtest": bigtest,
        **({"gefs": gefs} if os.environ.get("ARM64_BUILD_GEFS") == "1" else {}),
        "gmac": gmac,
        "sdprobe": sdprobe,
        "sdcard": sdcard,
        "opteeprobe": keyrootprobe,
        "dp9iktest": dp9iktest,
        "factotum": factotum,
        "factotumkey": factotumkey,
        "keyroot": keyrootsvc,
        "keyroottest": keyroottest,
        "displaytest": displaytest,
        "rcmain": rcmain.encode(),
    }
    if args.ta is not None:
        programs["optee-keyroot.ta"] = args.ta.read_bytes()
    mmcdata = os.environ.get("ARM64_MMCDATA")
    if mmcdata:
        programs["datameta.bin"] = Path(mmcdata).read_bytes()
    wcnfw = ROOT / "sys/src/cmd/sdio/wcnmodem.bin"
    if wcnfw.exists():
        programs["wcnmodem.bin"] = wcnfw.read_bytes()
    if args.archive is not None:
        write_archive(args.archive, programs)
    buildcache.record(stamp, sig)


if __name__ == "__main__":
    main()
