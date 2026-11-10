#!/usr/bin/env python3
"""Build and smoke-test a lean, native Harvey CMake in QEMU.

Runs `cmake --version` and a `cmake -P` script from `/initramfs/cmake/bin`,
the install layout CMake uses to locate its Modules.  The build comes from
lean-cmake.py, which replaces bundled cURL/libarchive/libuv with stubs.

    util/arm64/test-cmake-native.py --source /path/to/cmake-3.28.3

`--binary` skips the build and tests an existing lean cmake.

The configure/generate path is not exercised: CMake's process handling
currently hangs on APEX pipes (KWSys' non-select polling needs descriptor
readiness, which `select()` does not provide yet).
"""
from pathlib import Path
import argparse
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
        binary = (args.source.resolve().parent / "cmake-lean" / "bin" / "cmake")
    else:
        sys.exit("give --source or --binary")
    if not binary.is_file():
        sys.exit(f"missing {binary}")

    stage = Path(args.stage)
    install = stage / "install" / "cmake"
    for d in (install / "bin", install / "share" / "cmake-3.28" / "Modules"):
        d.mkdir(parents=True, exist_ok=True)
    (install / "bin" / "cmake").write_bytes(binary.read_bytes())
    (install / "bin" / "cmake").chmod(0o755)
    for name in ("CMake.cmake", "CMakeSystem.cmake"):
        src = args.source / "Modules" / name if args.source else None
        if src and src.is_file():
            (install / "share" / "cmake-3.28" / "Modules" / name).write_bytes(
                src.read_bytes())
    (install / "share" / "cmake-3.28" / "Modules" / "CMake.cmake").touch(
        exist_ok=True)

    (install / "probe.cmake").write_text(
        'message(STATUS "CMSCRIPT-OK ${CMAKE_VERSION}")\n'
        'if(NOT CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)\n'
        '  message(FATAL_ERROR "version check failed")\n'
        'endif()\n'
        'message(STATUS "CMSCRIPT-DONE")\n')

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
    print("native CMake smoke PASS")


if __name__ == "__main__":
    main()
