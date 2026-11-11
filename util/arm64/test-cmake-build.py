#!/usr/bin/env python3
"""Build a project end-to-end on Harvey: native CMake + native ninja.

Uses rc (/bin/rc) as the shell, installs cmake and ninja in the root tree,
runs `cmake -G Ninja` and then `ninja hello` inside QEMU, and checks that
the custom target prints `hi`.

    util/arm64/test-cmake-build.py --cmake <lean cmake> --ninja <harvey ninja>

`build-ninja.py` produces the ninja binary; `lean-cmake.py` produces cmake.
The rc binary comes from build/arm64-kernel/userland/rc and rcmain from
rc/lib/rcmain.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

MODULES = [
    "CMake.cmake",
    "CMakeDetermineSystem.cmake",
    "CMakeSystem.cmake.in",
    "CMakeSystemSpecificInitialize.cmake",
    "CMakeSystemSpecificInformation.cmake",
    "CMakeGenericSystem.cmake",
    "CMakeInitializeConfigs.cmake",
    "CMakePrintSystemInformation.cmake",
    "Platform/Linux.cmake",
    "Platform/UnixPaths.cmake",
]

ECHO_C = """\
#include <stdio.h>
int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++)
        printf("%s%s", i > 1 ? " " : "", argv[i]);
    printf("\\n");
    return 0;
}
"""


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cmake", required=True, type=Path)
    ap.add_argument("--ninja", required=True, type=Path)
    ap.add_argument("--cmake-source", type=Path,
                    help="used to copy Modules/ and Platform/Harvey")
    ap.add_argument("--rc", type=Path,
                    default=ROOT / "build" / "arm64-kernel" / "userland" / "rc")
    ap.add_argument("--stage", default="/tmp/opencode/cmake-build")
    ap.add_argument("--mem", type=int, default=4096)
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--deadline", type=int, default=240)
    args = ap.parse_args()

    if not args.cmake.is_file() or not args.ninja.is_file():
        sys.exit("missing cmake or ninja binary")
    if not args.rc.is_file():
        sys.exit(f"missing rc binary at {args.rc}")
    if not (ROOT / "rc" / "lib" / "rcmain").is_file():
        sys.exit("missing rc/lib/rcmain")

    stage = Path(args.stage)
    shutil.rmtree(stage, ignore_errors=True)
    tree = stage / "tree"
    for d in ("cmake/bin", "cmake/share/cmake-3.28/Modules/Platform",
              "bin", "boot", "proj"):
        (tree / d).mkdir(parents=True, exist_ok=True)

    shutil.copyfile(args.cmake, tree / "cmake" / "bin" / "cmake")
    shutil.copyfile(args.ninja, tree / "bin" / "ninja")
    shutil.copyfile(args.rc, tree / "bin" / "rc")
    subprocess.run(["llvm-strip", "--strip-all", str(tree / "bin" / "rc")],
                   check=True)
    shutil.copyfile(ROOT / "rc" / "lib" / "rcmain", tree / "boot" / "rcmain")
    for name in MODULES:
        if args.cmake_source:
            src = args.cmake_source / "Modules" / name
            if src.is_file():
                dst = tree / "cmake/share/cmake-3.28/Modules" / name
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src, dst)
    (tree / "cmake/share/cmake-3.28/Modules/CMake.cmake").touch(
        exist_ok=True)
    shutil.copyfile(HERE / "cmake" / "Platform-Harvey.cmake",
                    tree / "cmake/share/cmake-3.28/Modules/Platform/Harvey.cmake")

    echo_c = stage / "echo.c"
    echo_c.write_text(ECHO_C)
    subprocess.run([str(HERE / "aarch64-clang"), str(echo_c),
                    "-o", str(tree / "bin" / "echo")], check=True)
    subprocess.run(["llvm-strip", "--strip-all",
                    str(tree / "bin" / "echo")], check=True)

    (tree / "proj" / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(native NONE)\n"
        'message(STATUS "NPROJ-OK")\n'
        "add_custom_target(hello COMMAND ${CMAKE_COMMAND} -E echo hi)\n")
    (tree / "proj" / "build.rc").write_text(
        "/cmake/bin/cmake -S /proj -B /proj/build -G Ninja "
        "'-DCMAKE_MAKE_PROGRAM=/bin/ninja'\n"
        "/bin/ninja -C /proj/build hello\n"
        "/bin/echo E2E-DONE\n")

    rc = tree / "bin" / "rc"
    runner = stage / "runner.elf"
    subprocess.run([
        sys.executable, str(HERE / "mkrun.py"), str(rc),
        "--output", str(runner),
        "--tree", f"{tree}:/", "--suffix", "",
        "--in-tree", "/bin/rc", "--env", "SHELL=/bin/rc",
        "--", "/proj/build.rc",
    ], check=True)

    dump = stage / "log"
    subprocess.run([
        sys.executable, str(HERE / "test_apex_boot.py"),
        "--elf", str(runner), "--marker", "E2E-DONE",
        "--mem", str(args.mem), "--smp", str(args.smp),
        "--deadline", str(args.deadline), "--dump", str(dump),
    ], check=True)

    text = dump.read_text(errors="replace")
    for needle in ("NPROJ-OK", "[1/1]", "hi", "E2E-DONE"):
        if needle not in text:
            sys.exit(f"missing {needle!r} in the build output")
    print("native CMake + ninja build PASS")


if __name__ == "__main__":
    main()
