#!/usr/bin/env python3
"""Build a lean, native Harvey CMake from a CMake 3.28 source tree.

The bundled cURL, libarchive and libuv are replaced by small stubs so that
the CMake parser/generator itself builds with the current APEX POSIX
surface.  Features that need those libraries -- downloads, archive
extraction, and `cmake --workflow` -- are unavailable and report a clean
error.

    util/arm64/lean-cmake.py --source /path/to/cmake-3.28.3

The source tree is modified in place (the stubs are copied to
Utilities/cmstub and util/arm64/cmake/lean.diff is applied).  The build
lands in <source>/../<source-name>-lean unless --build is given.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CMKIT = HERE / "cmake"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", required=True, type=Path)
    ap.add_argument("--build", type=Path, default=None)
    ap.add_argument("--jobs", type=int, default=16)
    args = ap.parse_args()

    src = args.source.resolve()
    build = (args.build or (src.parent / (src.name + "-lean"))).resolve()
    if not (src / "Source" / "cmake.cxx").is_file():
        sys.exit(f"{src} does not look like a CMake source tree")

    stub = src / "Utilities" / "cmstub"
    stub.mkdir(parents=True, exist_ok=True)
    for name in ("curl_stub.c", "archive_stub.c", "uv_stub.c"):
        shutil.copyfile(CMKIT / name, stub / name)

    marker = src / "Source" / "Modules" / "CMakeBuildUtilities.cmake"
    if "HARVEY_LEAN_CMAKE" not in marker.read_text(errors="replace"):
        subprocess.run(["patch", "-p1", "-N", "-i",
                        str(CMKIT / "lean.diff")], cwd=src, check=False)
    if "HARVEY_LEAN_CMAKE" not in marker.read_text(errors="replace"):
        sys.exit("failed to apply util/arm64/cmake/lean.diff")

    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja")
    if not cmake or not ninja:
        sys.exit("cmake and ninja are required on the host")

    subprocess.run([
        cmake, "-S", str(src), "-B", str(build), "-G", "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={HERE / 'harvey-aarch64-toolchain.cmake'}",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DHARVEY_LEAN_CMAKE=ON",
        "-DCMake_ENABLE_DEBUGGER=OFF",
        "-DBUILD_CursesDialog=OFF", "-DBUILD_QtDialog=OFF",
        "-DBUILD_TESTING=OFF", "-DCMAKE_USE_OPENSSL=OFF",
        "-DCMAKE_DL_LIBS=",
        "-DCMAKE_INSTALL_PREFIX=/initramfs/cmake",
    ], check=True)
    subprocess.run([ninja, "-C", str(build), "-j", str(args.jobs),
                    "bin/cmake"], check=True)

    binary = build / "bin" / "cmake"
    subprocess.run(["llvm-strip", "--strip-all", str(binary)], check=True)
    print(f"lean native CMake: {binary} ({binary.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
