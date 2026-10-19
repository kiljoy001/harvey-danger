#!/usr/bin/env python3
"""Cross-build Harvey libc for ARM64 with architecture-specific syscalls."""

from pathlib import Path
import json
import os
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
LIBC = ROOT / "sys/src/libc"
OUT = Path(os.environ.get("ARM64_LIBC_OUT", ROOT / "build/arm64-libc"))
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"
AR = os.environ.get("ARM64_AR") or shutil.which("llvm-ar-23") or "llvm-ar"


def configured_sources() -> list[Path]:
    common = json.loads((LIBC / "libc.json").read_text())[0]["SourceFiles"]
    arch = json.loads((LIBC / "aarch64/build.json").read_text())[0]["SourceFiles"]
    sources = [LIBC / name for name in common if not name.startswith("9syscall/")]
    sources.extend(LIBC / name.replace("$ARCH", "aarch64") for name in arch)
    missing = [source for source in sources if not source.exists()]
    if missing:
        raise RuntimeError(f"missing libc sources: {', '.join(map(str, missing))}")
    return sources


def main() -> None:
    objects = OUT / "objects"
    wrappers = OUT / "9syscall"
    shutil.rmtree(OUT, ignore_errors=True)
    objects.mkdir(parents=True)
    wrappers.mkdir()
    subprocess.run([
        sys.executable, str(ROOT / "util/9vm/mksys.py"),
        "-mode=syscallfiles", "-arch=aarch64", "-o", str(wrappers),
        str(ROOT / "sys/src/sysconf.json"),
    ], check=True, stdout=subprocess.DEVNULL)
    sources = configured_sources() + sorted(wrappers.glob("*.s"))
    flags = [
        "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables", "-mstrict-align", "-fcommon",
        "-O2", "-g", "-D__HARVEY__", "-Wall", "-Wno-parentheses",
        "-Wno-missing-braces", "-Wno-unknown-pragmas",
        "-Wno-unused-function", "-Wno-unused-variable",
    ]
    for include in (ROOT / "aarch64/include", ROOT / "sys/include", LIBC,
                    wrappers):
        flags.extend(("-I", str(include)))
    built = []
    failures = []
    for index, source in enumerate(sources):
        obj = objects / f"{index:03d}-{source.parent.name}-{source.stem}.o"
        result = subprocess.run([CC, *flags, "-c", str(source), "-o", str(obj)],
                                text=True, capture_output=True)
        if result.returncode:
            failures.append((source, result.stderr))
        else:
            built.append(obj)
    if failures:
        print(f"ARM64 libc compile: {len(built)} passed, {len(failures)} failed")
        for source, error in failures:
            print(f"\n{source.relative_to(ROOT)}:\n{error}")
        raise SystemExit(1)
    archive = OUT / "libc.a"
    subprocess.run([AR, "rcs", str(archive), *map(str, built)], check=True)
    print(f"ARM64 libc: {len(built)} objects PASS")
    print(archive)


if __name__ == "__main__":
    main()
