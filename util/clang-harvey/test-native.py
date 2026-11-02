#!/usr/bin/env python3
"""Prove that clang runs on Harvey and emits an object file.

Copies the cross-built clang binary into the guest-visible build tree, builds
a tiny boot harness, boots 9vm, and checks that the guest produced a valid
x86-64 relocatable containing `add`.

    util/clang-harvey/test-native.py            # use /tmp/llvm-harvey/bin/clang-22
    util/clang-harvey/test-native.py PATH       # use a specific clang binary
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BUILD = REPO / "build" / "clang-harvey"
DEFAULT_CLANG = Path("/tmp/opencode/llvm-harvey/bin/clang-22")
VM = REPO / "util" / "9vm" / "9vm"
KERNEL = REPO / "build" / "harvey.32bit"


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


def main():
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CLANG
    if not source.is_file():
        sys.exit(f"clang binary not found: {source}")

    BUILD.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, BUILD / "clang")
    os.chmod(BUILD / "clang", 0o755)
    shutil.copyfile(HERE / "test-native-clang.c", BUILD / "test.c")
    for stale in (BUILD / "test.o",):
        stale.unlink(missing_ok=True)

    run([str(HERE / "harvey-clang"), str(HERE / "run-native-clang.c"),
         "-o", str(BUILD / "run-native-clang.elf")])

    proc = subprocess.run(
        [str(VM), "-m", "2048", "-boot", str(BUILD / "run-native-clang.elf"),
         str(KERNEL)],
        cwd=REPO, text=True, capture_output=True, timeout=300)
    output = proc.stdout + proc.stderr
    for line in output.splitlines():
        if line.strip() and ("clang" in line or "error" in line or "panic" in line):
            print("  " + line)

    obj = BUILD / "test.o"
    if not obj.is_file():
        sys.exit("native clang produced no object file")
    symbols = subprocess.run(["llvm-nm", str(obj)], check=True,
                             text=True, capture_output=True).stdout
    if " T add" not in symbols:
        sys.exit(f"object file has no 'add' symbol:\n{symbols}")
    print(f"native clang produced {obj} ({obj.stat().st_size} bytes) with 'add'")


if __name__ == "__main__":
    main()
