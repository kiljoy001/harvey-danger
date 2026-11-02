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
#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct Shape { virtual ~Shape() {} };
struct Square : Shape { int side = 3; };

struct Guard {
    int *seen;
    explicit Guard(int *s) : seen(s) {}
    ~Guard() { *seen += 1; }
};

template <typename T>
T sum(const std::vector<T> &v) {
    T total{};
    for (const auto &x : v)
        total += x;
    return total;
}

int main() {
    std::vector<int> v{3, 1, 4, 1, 5, 9, 2, 6};
    std::sort(v.begin(), v.end());

    std::map<std::string, int> counts;
    for (int i : v)
        counts[std::to_string(i)]++;

    Shape *shape = new Square;
    Square *square = dynamic_cast<Square *>(shape);

    int unwound = 0;
    try {
        Guard g(&unwound);
        throw std::runtime_error("boom");
    } catch (const std::exception &e) {
        unwound += 10;
    }
    std::ostringstream out;
    out << "sum=" << sum(v) << " distinct=" << counts.size()
        << " rtti=" << (square ? square->side : -1)
        << " unwound=" << unwound;
    std::printf("hello from harvey: %s\\n", out.str().c_str());
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

    if "hello from harvey: sum=31 distinct=7 rtti=3 unwound=11" not in output:
        sys.exit("the guest-linked program did not run")
    elf = BUILD / "hello"
    if not elf.is_file() or elf.stat().st_size == 0:
        sys.exit("ld.lld produced no executable")
    print(f"native toolchain: clang++ -> cc1 -> ld.lld -> {elf.name} "
          f"({elf.stat().st_size} bytes) ran and printed "
          f"'hello from harvey: sum=31 distinct=7 rtti=3 unwound=11'")


if __name__ == "__main__":
    main()
