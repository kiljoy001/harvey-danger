#!/usr/bin/env python3
"""Compile, link and run a C++ program entirely inside Harvey.

The guest runs one command with no flags at all:

    clang++ hello.cpp -o hello

The driver loads clang++.cfg from its own directory, compiles with cc1,
and invokes the adjacent ld.lld.  The harness then execs the result.

    util/clang-harvey/install-sdk.py
    util/clang-harvey/test-native-toolchain.py
    util/clang-harvey/test-native-toolchain.py /tmp/llvm-harvey
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BUILD = REPO / "build" / "clang-harvey"
DEFAULT_BIN = Path("/tmp/opencode/llvm-harvey")
VM = REPO / "util" / "9vm" / "9vm"
KERNEL = REPO / "build" / "harvey.32bit"

HELLO = """\
#include <cstdio>
#include <string>

int main() {
    std::string who = "harvey";
    std::printf("hello from %s\\n", who.c_str());
    return 0;
}
"""


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


def main():
    llvm_build = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BIN
    run([sys.executable, str(HERE / "install-sdk.py"), str(llvm_build)])

    (BUILD / "hello.cpp").write_text(HELLO)
    for stale in ("hello.o", "hello"):
        (BUILD / stale).unlink(missing_ok=True)

    run([str(HERE / "harvey-clang"), str(HERE / "run-native-clang-full.c"),
         "-o", str(BUILD / "run-native-clang-full.elf")])

    proc = subprocess.run(
        [str(VM), "-m", "2048", "-boot", str(BUILD / "run-native-clang-full.elf"),
         str(KERNEL)],
        cwd=REPO, text=True, capture_output=True, timeout=600)
    output = proc.stdout + proc.stderr
    for line in output.splitlines():
        if line.strip() and any(w in line for w in
                                ("compiled", "linked", "hello from", "error",
                                 "panic", "fault", "suicide")):
            print("  " + line)

    if "hello from harvey" not in output:
        sys.exit("the guest-linked program did not run")
    elf = BUILD / "hello"
    if not elf.is_file() or elf.stat().st_size == 0:
        sys.exit("ld.lld produced no executable")
    print(f"native toolchain: clang++ -> cc1 -> ld.lld -> {elf.name} "
          f"({elf.stat().st_size} bytes) ran and printed "
          f"'hello from harvey'")


if __name__ == "__main__":
    main()
