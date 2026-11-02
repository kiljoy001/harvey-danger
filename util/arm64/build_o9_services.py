#!/usr/bin/env python3
"""Cross-build the Objective-9C driver manager services for ARM64 Harvey."""

from pathlib import Path
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import buildcache


ROOT = Path(__file__).resolve().parents[2]
O9_ROOT = Path(os.environ.get("O9_REPO", ROOT.parent / "objective-9c")).resolve()
OUT = Path(os.environ.get("ARM64_O9_OUT", ROOT / "build/arm64-o9"))
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM64_LD") or shutil.which("ld.lld") or "ld.lld"
AR = os.environ.get("ARM64_AR") or shutil.which("llvm-ar-23") or "llvm-ar"
STRIP = (os.environ.get("ARM64_STRIP") or shutil.which("llvm-strip-23") or
         shutil.which("llvm-strip") or "llvm-strip")
# The repo copy of o9c is a Plan 9 a.out when it is built inside the
# 9front VM.  Prefer a host build (util/arm64/build_o9c.sh, plan9port's
# 9c/9l) when it exists; O9C overrides either.
_host_o9c = ROOT / "build/host/o9c"
if os.environ.get("O9C"):
	O9C = Path(os.environ["O9C"]).resolve()
elif _host_o9c.is_file():
	O9C = _host_o9c.resolve()
else:
	O9C = (O9_ROOT / "o9c/o9c").resolve()
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
    "libmp": ROOT / "sys/src/libmp/libmp.json",
    "libString": ROOT / "sys/src/libString/libString.json",
    "libip": ROOT / "sys/src/libip/libip.json",
    "libthread": ROOT / "sys/src/libthread/libthread.json",
    "lib9p": ROOT / "sys/src/lib9p/lib9p.json",
    "libauth": ROOT / "sys/src/libauth/libauth.json",
    "libauthsrv": ROOT / "sys/src/libauthsrv/libauthsrv.json",
    "libsec": ROOT / "sys/src/libsec/libsec.json",
    "libndb": ROOT / "sys/src/libndb/libndb.json",
    "libbio": ROOT / "sys/src/libbio/libbio.json",
    "libavl": ROOT / "sys/src/libavl/libavl.json",
    "libmemdraw": ROOT / "sys/src/libmemdraw/libmemdraw.json",
    "libdraw": ROOT / "sys/src/libdraw/libdraw.json",
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
    "o9display": {
        "source": ROOT / "sys/src/cmd/o9display/service.o9",
        "extra": [],
        "base": 0x3A00000,
    },
    "o9aspera": {
        "source": ROOT / "sys/src/cmd/o9aspera/main.o9",
        "extra": [],
        "base": 0x4600000,
    },
    "o9aspera-screen": {
        "source": ROOT / "sys/src/cmd/o9aspera/screen.o9",
        "extra": [],
        "base": 0x4E00000,
    },
    "o9xhci": {
        "source": ROOT / "sys/src/cmd/o9xhci/service.o9",
        "extra": [
            ROOT / "sys/src/cmd/o9xhci/xhci.c",
            ROOT / "sys/src/cmd/o9xhci/binding.c",
            ROOT / "sys/src/cmd/o9drvman/health.c",
        ],
        "base": 0x5600000,
    },
    "test-xhci": {
        "source": ROOT / "sys/src/cmd/o9xhci/test_guest.o9",
        "extra": [
            ROOT / "sys/src/cmd/o9xhci/xhci.c",
            ROOT / "sys/src/cmd/o9xhci/binding.c",
            ROOT / "sys/src/cmd/o9drvman/health.c",
        ],
        "base": 0x5E00000,
    },
    "test-xhci1": {
        "source": ROOT / "sys/src/cmd/o9xhci/test_otg.o9",
        "extra": [
            ROOT / "sys/src/cmd/o9xhci/xhci.c",
            ROOT / "sys/src/cmd/o9xhci/binding.c",
            ROOT / "sys/src/cmd/o9drvman/health.c",
        ],
        "base": 0x6600000,
    },
    "test-audio": {
        "source": ROOT / "sys/src/cmd/o9xhci/test_audio.o9",
        "extra": [
            ROOT / "sys/src/cmd/o9xhci/xhci.c",
            ROOT / "sys/src/cmd/o9xhci/binding.c",
            ROOT / "sys/src/cmd/o9drvman/health.c",
        ],
        "base": 0x6E00000,
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


def compile_source(source: Path, obj: Path, extra_includes=()) -> bool:
    include_flags = []
    for include in [*INCLUDES, source.parent, *extra_includes]:
        include_flags.extend(("-I", str(include)))
    try:
        return buildcache.run_cached(
            [CC, *FLAGS, *include_flags, "-c", str(source), "-o", str(obj)],
            [obj], sources=[source])
    except RuntimeError as error:
        raise RuntimeError(f"compiling {display_path(source)} failed:\n"
                           f"{error}")


def archive(name: str, sources: list[Path]) -> tuple[Path, bool]:
    destination = OUT / f"{name}.a"
    if os.environ.get("ARM64_REUSE_LIBS") == "1" and destination.exists():
        return destination, False
    directory = OUT / name
    directory.mkdir(exist_ok=True)
    objects = []
    changed = False
    for index, source in enumerate(sources):
        obj = directory / f"{index:03d}-{source.stem}.o"
        changed |= compile_source(source, obj)
        objects.append(obj)
    if changed or not destination.exists():
        run([AR, "rcs", str(destination), *map(str, objects)])
        print(f"ARM64 {name}: {len(objects)} objects")
    return destination, changed


def manifest_sources(manifest: Path) -> list[Path]:
    entry = json.loads(manifest.read_text())[0]
    directory = manifest.parent
    sources = [directory / name.replace("$ARCH", "aarch64")
               for name in entry["SourceFiles"]]
    missing = [source for source in sources if not source.exists()]
    if missing:
        raise RuntimeError("missing sources: " + ", ".join(map(str, missing)))
    return sources


def transpile(source: Path, destination: Path) -> bool:
    stamp = destination.with_suffix(destination.suffix + ".sig")
    sig = buildcache.signature([str(O9C)], [source])
    if destination.exists() and buildcache.up_to_date(stamp, sig):
        return False
    environment = os.environ.copy()
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    with destination.open("w") as output:
        result = subprocess.run([str(O9C), str(source)], stdout=output,
                                stderr=subprocess.PIPE, text=True,
                                env=environment)
    if result.returncode:
        raise RuntimeError(f"transpiling {display_path(source)} failed:\n"
                           f"{result.stderr}")
    buildcache.record(stamp, sig)
    return True


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


def build_service(name: str, settings: dict, libraries: list[Path],
                  relink: bool = False) -> tuple[Path, bool]:
    directory = OUT / f"{name}-objects"
    directory.mkdir(exist_ok=True)
    generated = directory / f"{name}.o9.c"
    changed = transpile(settings["source"], generated)
    sources = [generated, *settings["extra"]]
    objects = []
    for index, source in enumerate(sources):
        obj = directory / f"{index:02d}-{source.stem}.o"
        changed |= compile_source(source, obj, [settings["source"].parent])
        objects.append(obj)
    script = directory / f"{name}.ld"
    linker_script(script, settings["base"])
    destination = OUT / name
    if not changed and not relink and destination.exists():
        return destination, False
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
    return destination, True


def main() -> None:
    if not O9C.is_file():
        raise SystemExit(f"Objective-9C compiler not found: {O9C}")
    OUT.mkdir(parents=True, exist_ok=True)
    run([sys.executable, str(ROOT / "util/arm64/build_libc.py")])
    libraries = []
    relink = False
    for name, sources in (("libo9", LIBO9_SOURCES),):
        path, changed = archive(name, sources)
        libraries.append(path)
        relink |= changed
    for name, manifest in LIBRARIES.items():
        path, changed = archive(name, manifest_sources(manifest))
        libraries.append(path)
        relink |= changed
    libraries.append(ROOT / "build/arm64-libc/libc.a")
    built = 0
    for name, settings in SERVICES.items():
        _, changed = build_service(name, settings, libraries, relink)
        built += changed
    print(f"ARM64 Objective-9C services PASS ({built} rebuilt)")


if __name__ == "__main__":
    main()
