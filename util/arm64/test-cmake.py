#!/usr/bin/env python3
"""Build and run a small CMake project for Harvey/ARM64.

This is the cross path: the host's CMake drives the Harvey wrapper through
util/arm64/harvey-aarch64-toolchain.cmake, and the resulting static ELF is
booted in QEMU.  The project exercises a C library, a C++20 executable with
<ranges>, the generated compile_commands.json, and a value that must survive
into the guest.

    python3 util/arm64/test-cmake.py [--keep]

A native (guest) CMake needs the bundled libuv, curl and libarchive ports to
finish, so this test covers the toolchain path, not a self-hosted cmake.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TOOLCHAIN = HERE / "harvey-aarch64-toolchain.cmake"

CMAKE = """\
cmake_minimum_required(VERSION 3.20)
project(harveytest LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(greet STATIC greet.c)
target_include_directories(greet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(hello main.cpp)
target_link_libraries(hello PRIVATE greet)

enable_testing()
add_test(NAME smoke COMMAND hello)
"""

GREET_H = """\
#ifndef GREET_H
#define GREET_H
#ifdef __cplusplus
extern "C" {
#endif
const char *greet(void);
#ifdef __cplusplus
}
#endif
#endif
"""

GREET_C = """\
#include "greet.h"
const char *greet(void) { return "CMake-Harvey"; }
"""

MAIN_CPP = """\
#include "greet.h"
#include <cstdio>
#include <string>
#include <vector>
#include <ranges>

int main()
{
    std::vector<int> v{1, 2, 3};
    int sum = 0;
    for (int x : v | std::views::transform([](int n){ return n * 2; }))
        sum += x;
    std::string s = greet();
    std::printf("CMAKE-PROJECT %s %d %s\\n", s.c_str(), sum,
                std::to_string(v.size()).c_str());
    std::printf("CMAKE-PROJECT-DONE\\n");
    return sum == 12 ? 0 : 1;
}
"""


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stage", default="/tmp/opencode/cmake-project")
    ap.add_argument("--mem", type=int, default=2048)
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--deadline", type=int, default=300)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja")
    if not cmake or not ninja:
        sys.exit("cmake and ninja are required on the host")

    stage = Path(args.stage)
    if not args.keep:
        shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True, exist_ok=True)
    (stage / "CMakeLists.txt").write_text(CMAKE)
    (stage / "greet.h").write_text(GREET_H)
    (stage / "greet.c").write_text(GREET_C)
    (stage / "main.cpp").write_text(MAIN_CPP)

    build = stage / "build"
    subprocess.run([
        cmake, "-S", str(stage), "-B", str(build), "-G", "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN}",
        "-DCMAKE_BUILD_TYPE=Release",
    ], check=True)
    subprocess.run([ninja, "-C", str(build)], check=True)

    elf = build / "hello"
    if not elf.is_file():
        sys.exit(f"missing {elf}")
    if not (build / "compile_commands.json").is_file():
        sys.exit("CMake did not generate compile_commands.json")

    runner = stage / "runner.elf"
    subprocess.run([
        sys.executable, str(HERE / "mkrun.py"), str(elf),
        "--output", str(runner),
    ], check=True)

    dump = stage / "log"
    subprocess.run([
        sys.executable, str(HERE / "test_apex_boot.py"),
        "--elf", str(runner),
        "--marker", "CMAKE-PROJECT-DONE",
        "--mem", str(args.mem), "--smp", str(args.smp),
        "--deadline", str(args.deadline), "--dump", str(dump),
    ], check=True)

    text = dump.read_text(errors="replace")
    expected = "CMAKE-PROJECT CMake-Harvey 12 3"
    if expected not in text:
        sys.exit(f"missing {expected!r} in guest output")
    print("CMake cross-build PASS")


if __name__ == "__main__":
    main()
