#!/usr/bin/env python3
"""Build and smoke-test a lean, native Harvey CMake in QEMU.

Runs `cmake --version`, a `cmake -P` script, and a `Ninja`
configure/generate of a `project(... NONE)` from `/initramfs/cmake/bin`, the
install layout CMake uses to locate its Modules.  The Ninja run exercises
`execute_process` (the version probe), select/poll and waitpid.  The build
comes from lean-cmake.py, which replaces bundled cURL/libarchive/libuv with
stubs.

    util/arm64/test-cmake-native.py --source /path/to/cmake-3.28.3

`--binary` skips the build and tests an existing lean cmake.

The lean patch teaches KWSys to reap children with waitpid on Harvey
because Plan 9 does not deliver SIGCHLD.
"""
from pathlib import Path
import argparse
import shutil
import struct
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", type=Path)
    ap.add_argument("--binary", type=Path)
    ap.add_argument("--stage", default="/tmp/opencode/cmake-native")
    ap.add_argument("--mem", type=int, default=2048)
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--deadline", type=int, default=300)
    args = ap.parse_args()

    if args.binary:
        binary = args.binary
    elif args.source:
        subprocess.run([sys.executable, str(HERE / "lean-cmake.py"),
                        "--source", str(args.source)], check=True)
        binary = (args.source.resolve().parent /
                  (args.source.resolve().name + "-lean") / "bin" / "cmake")
    else:
        sys.exit("give --source or --binary")
    if not binary.is_file():
        sys.exit(f"missing {binary}")

    stage = Path(args.stage)
    shutil.rmtree(stage, ignore_errors=True)
    install = stage / "install" / "cmake"
    for d in (install / "bin", install / "share" / "cmake-3.28" / "Modules"):
        d.mkdir(parents=True, exist_ok=True)
    (install / "bin" / "cmake").write_bytes(binary.read_bytes())
    (install / "bin" / "cmake").chmod(0o755)
    # Only the modules a `project(... NONE)` configure needs: the guest
    # ramfs is backed by the kernel heap, and the full Modules tree (1200
    # files, 8 MiB) exhausts it during untar.
    modules = install / "share" / "cmake-3.28" / "Modules"
    module_files = [
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
    for name in module_files:
        src = args.source / "Modules" / name if args.source else None
        if src and src.is_file():
            dst = modules / name
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(src.read_bytes())
    (modules / "CMake.cmake").touch(exist_ok=True)
    shutil.copyfile(HERE / "cmake" / "Platform-Harvey.cmake",
                    modules / "Platform" / "Harvey.cmake")

    # A fake ninja: CMake runs `<make> --version` during the generator
    # check, so it must exit 0 and print a version.
    ninja_c = stage / "ninja.c"
    ninja_c.write_text('#include <stdio.h>\n'
                       'int main(void){printf("1.11.1\\n");return 0;}\n')
    subprocess.run([str(HERE / "aarch64-clang"), str(ninja_c),
                    "-o", str(install / "bin" / "ninja")], check=True)

    (install / "probe.cmake").write_text(
        'message(STATUS "CMSCRIPT-OK ${CMAKE_VERSION}")\n'
        'if(NOT CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)\n'
        '  message(FATAL_ERROR "version check failed")\n'
        'endif()\n'
        'message(STATUS "CMSCRIPT-DONE")\n')

    if args.source:
        proj = install / "proj"
        proj.mkdir(parents=True, exist_ok=True)
        (proj / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(native NONE)\n"
            'message(STATUS "NPROJ-OK")\n'
            "add_custom_target(hello COMMAND ${CMAKE_COMMAND} -E echo hi)\n")

    tar = stage / "install.tar"
    with tarfile.open(tar, "w", format=tarfile.USTAR_FORMAT) as t:
        for path in sorted((stage / "install").rglob("*")):
            t.add(str(path), arcname=str(path.relative_to(stage / "install")))
    data = tar.read_bytes()
    boot = stage / "install.boot"
    boot.write_bytes(
        struct.pack("<8sIIII", b"O9BOOT01", 1, 1, 24 + 48 + len(data), 0) +
        struct.pack("<32sIIII", b"install.tar", 24 + 48, len(data), 0o444, 0) +
        data)

    def run(args_guest, marker):
        runner = stage / "runner.elf"
        subprocess.run([
            sys.executable, str(HERE / "mkrun.py"), str(binary),
            "--output", str(runner),
            "--untar", "/boot/install.tar:/initramfs",
            "--in-tree", "/initramfs/cmake/bin/cmake",
            "--", *args_guest,
        ], check=True)
        dump = stage / ("log-" + marker.replace(" ", "_"))
        subprocess.run([
            sys.executable, str(HERE / "test_apex_boot.py"),
            "--elf", str(runner), "--archive", str(boot),
            "--marker", marker, "--mem", str(args.mem),
            "--smp", str(args.smp), "--deadline", str(args.deadline),
            "--dump", str(dump),
        ], check=True)
        return dump

    version = run(["--version"], "CMake suite maintained")
    if "cmake version" not in version.read_text(errors="replace"):
        sys.exit("cmake --version did not print a version")

    script = run(["-P", "/initramfs/cmake/probe.cmake"], "CMSCRIPT-DONE")
    text = script.read_text(errors="replace")
    if "CMSCRIPT-OK 3.28" not in text:
        sys.exit("cmake -P did not run the probe script")

    if args.source:
        generate = run([
            "-S", "/initramfs/cmake/proj", "-B", "/initramfs/cmake/proj/build",
            "-G", "Ninja",
            "-DCMAKE_MAKE_PROGRAM=/initramfs/cmake/bin/ninja",
        ], "Build files have been written")
        gtext = generate.read_text(errors="replace")
        if "NPROJ-OK" not in gtext:
            sys.exit("native CMake did not parse the project")
    print("native CMake smoke PASS")


if __name__ == "__main__":
    main()
