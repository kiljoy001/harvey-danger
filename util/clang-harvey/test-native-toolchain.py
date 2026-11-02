#!/usr/bin/env python3
"""Compile, link and run a C++ program entirely inside Harvey.

The guest boots a small harness that:
  1. runs the native clang to compile hello.cpp to hello.o,
  2. runs the native ld.lld to link hello.o against the Harvey runtime,
  3. execs the resulting hello.elf.

Nothing on the host compiler is used for the guest's program; the host only
supplies the two binaries themselves.

    util/clang-harvey/test-native-toolchain.py
    util/clang-harvey/test-native-toolchain.py /tmp/llvm-harvey/bin
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BUILD = REPO / "build" / "clang-harvey"
DEFAULT_BIN = Path("/tmp/opencode/llvm-harvey/bin")
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
    bindir = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BIN
    for name, source in (("clang", bindir / "clang-22"), ("ld.lld", bindir / "lld")):
        if not source.is_file():
            sys.exit(f"missing {source}; build LLVM for Harvey first")
        shutil.copyfile(source, BUILD / name)
        os.chmod(BUILD / name, 0o755)

    (BUILD / "hello.cpp").write_text(HELLO)
    for stale in ("hello.o", "hello.elf"):
        (BUILD / stale).unlink(missing_ok=True)

    run([str(HERE / "harvey-clang"), str(HERE / "run-native-toolchain.c"),
         "-o", str(BUILD / "run-native-toolchain.elf")])

    proc = subprocess.run(
        [str(VM), "-m", "2048", "-boot", str(BUILD / "run-native-toolchain.elf"),
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
    elf = BUILD / "hello.elf"
    if not elf.is_file() or elf.stat().st_size == 0:
        sys.exit("ld.lld produced no executable")
    print(f"native toolchain: clang -> ld.lld -> {elf.name} "
          f"({elf.stat().st_size} bytes) ran and printed "
          f"'hello from harvey'")


if __name__ == "__main__":
    main()
