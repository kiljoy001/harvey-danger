#!/usr/bin/env python3
"""Cross-build the Objective-9C driver manager services for ARM64 Harvey."""

from pathlib import Path
import json
import os
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
O9_ROOT = Path(os.environ.get("O9_REPO", ROOT.parent / "objective-9c")).resolve()
OUT = Path(os.environ.get("ARM64_O9_OUT", ROOT / "build/arm64-o9"))
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM64_LD") or shutil.which("ld.lld") or "ld.lld"
AR = os.environ.get("ARM64_AR") or shutil.which("llvm-ar-23") or "llvm-ar"
STRIP = (os.environ.get("ARM64_STRIP") or shutil.which("llvm-strip-23") or
         shutil.which("llvm-strip") or "llvm-strip")
O9C = O9_ROOT / "o9c/o9c"
BIG_PAGE = 0x200000

FLAGS = [
    "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
    "-fno-stack-protector", "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables", "-fno-vectorize",
    "-fno-slp-vectorize", "-mstrict-align", "-fsigned-char",
    "-fcommon", "-include", str(ROOT / "util/arm64/o9_compat.h"),
    "-O2", "-g", "-D__HARVEY__", "-DMONO_PLAN9", "-Wall",
    "-Wno-parentheses", "-Wno-missing-braces", "-Wno-unknown-pragmas",
    "-Wno-unused-function", "-Wno-unused-variable",
]
INCLUDES = [
    ROOT / "aarch64/include", ROOT / "sys/include", O9_ROOT,
    O9_ROOT / "libtab",
]

LIBRARIES = {
    "libthread": ROOT / "sys/src/libthread/libthread.json",
    "lib9p": ROOT / "sys/src/lib9p/lib9p.json",
    "libauth": ROOT / "sys/src/libauth/libauth.json",
    "libauthsrv": ROOT / "sys/src/libauthsrv/libauthsrv.json",
    "libsec": ROOT / "sys/src/libsec/libsec.json",
    "libndb": ROOT / "sys/src/libndb/libndb.json",
    "libbio": ROOT / "sys/src/libbio/libbio.json",
}

LIBO9_SOURCES = [
    O9_ROOT / name for name in (
        "o9_dispatch.c", "o9_runtime.c", "o9_filetree.c", "o9_view.c",
        "o9_crypto.c", "monocypher.c", "o9_tab_discard.c",
    )
] + [
    O9_ROOT / "libtab" / name for name in (
        "tab_error.c", "tab_create.c", "tab_row.c", "tab_rowmap.c",
        "tab_iter.c", "tab_codec.c", "tab_open.c", "tab_serialize.c",
        "tab_persist.c", "tab_emit.c", "tab_text.c", "tab_writer.c",
    )
]

SERVICES = {
    "o9drvman": {
        "source": ROOT / "sys/src/cmd/o9drvman/service.o9",
        "extra": [
            ROOT / "sys/src/cmd/o9drvman/lifecycle.c",
            ROOT / "sys/src/cmd/o9drvman/health.c",
            ROOT / "sys/src/cmd/o9drvman/binding.c",
        ],
        "base": 0x600000,
    },
    "o9driverfs": {
        "source": ROOT / "sys/src/cmd/o9driverfs/service.o9",
        "extra": [],
        "base": 0xE00000,
    },
    "o9display": {
        "source": ROOT / "sys/src/cmd/o9display/service.o9",
        "extra": [],
        "base": 0x3A00000,
    },
}


def run(command: list[str], *, capture: bool = False,
        env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=True, text=True,
                          capture_output=capture, env=env)


def display_path(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def compile_source(source: Path, obj: Path, extra_includes=()) -> None:
    include_flags = []
    for include in [*INCLUDES, source.parent, *extra_includes]:
        include_flags.extend(("-I", str(include)))
    result = subprocess.run(
        [CC, *FLAGS, *include_flags, "-c", str(source), "-o", str(obj)],
        text=True, capture_output=True,
    )
    if result.returncode:
        raise RuntimeError(f"compiling {display_path(source)} failed:\n"
                           f"{result.stderr}")


def archive(name: str, sources: list[Path]) -> Path:
    directory = OUT / name
    directory.mkdir()
    objects = []
    for index, source in enumerate(sources):
        obj = directory / f"{index:03d}-{source.stem}.o"
        compile_source(source, obj)
        objects.append(obj)
    destination = OUT / f"{name}.a"
    run([AR, "rcs", str(destination), *map(str, objects)])
    print(f"ARM64 {name}: {len(objects)} objects")
    return destination


def manifest_sources(manifest: Path) -> list[Path]:
    entry = json.loads(manifest.read_text())[0]
    directory = manifest.parent
    sources = [directory / name.replace("$ARCH", "aarch64")
               for name in entry["SourceFiles"]]
    missing = [source for source in sources if not source.exists()]
    if missing:
        raise RuntimeError("missing sources: " + ", ".join(map(str, missing)))
    return sources


def transpile(source: Path, destination: Path) -> None:
    environment = os.environ.copy()
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    with destination.open("w") as output:
        result = subprocess.run([str(O9C), str(source)], stdout=output,
                                stderr=subprocess.PIPE, text=True,
                                env=environment)
    if result.returncode:
        raise RuntimeError(f"transpiling {display_path(source)} failed:\n"
                           f"{result.stderr}")


def linker_script(path: Path, base: int) -> None:
    path.write_text(f"""ENTRY(_main)
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
""")


def build_service(name: str, settings: dict, libraries: list[Path]) -> Path:
    directory = OUT / f"{name}-objects"
    directory.mkdir()
    generated = directory / f"{name}.o9.c"
    transpile(settings["source"], generated)
    sources = [generated, *settings["extra"]]
    objects = []
    for index, source in enumerate(sources):
        obj = directory / f"{index:02d}-{source.stem}.o"
        compile_source(source, obj, [settings["source"].parent])
        objects.append(obj)
    script = directory / f"{name}.ld"
    linker_script(script, settings["base"])
    destination = OUT / name
    command = [
        LD, "-static", "-z", "max-page-size=4096", "-T", str(script),
        "-e", "_main", "-u", "_main", *map(str, objects),
        "--start-group", *map(str, libraries), "--end-group",
        "-o", str(destination),
    ]
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f"linking {name} failed:\n{result.stderr}")
    run([STRIP, "--strip-all", str(destination)])
    print(f"ARM64 {name}: {destination.stat().st_size} bytes")
    return destination


def main() -> None:
    if not O9C.is_file():
        raise SystemExit(f"Objective-9C compiler not found: {O9C}")
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    run([sys.executable, str(ROOT / "util/arm64/build_libc.py")])
    libraries = [archive("libo9", LIBO9_SOURCES)]
    libraries.extend(archive(name, manifest_sources(manifest))
                     for name, manifest in LIBRARIES.items())
    libraries.append(ROOT / "build/arm64-libc/libc.a")
    for name, settings in SERVICES.items():
        build_service(name, settings, libraries)
    print("ARM64 Objective-9C services PASS")


if __name__ == "__main__":
    main()
