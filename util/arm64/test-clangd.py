#!/usr/bin/env python3
"""Run clangd --check in the guest against a small C++ project.

This is the smoke test for the threaded clangd build.  It packs a
compile_commands.json and a C++ file into the runner, unpacks the SDK
archive as /initramfs/sdk, and runs

    /initramfs/sdk/bin/clangd --check=/initramfs/t/main.cpp \
        --compile-commands-dir=/initramfs/t

The required output is ``All checks completed, 0 errors``.

    python3 util/arm64/test-clangd.py --sdk-archive /tmp/opencode/sdk-clangd.boot
    python3 util/arm64/test-clangd.py --board-image /home/scott/tftp/clangd-test.img

The SDK archive must contain bin/clangd; build it with

    install-sdk.py --board-sdk --with-tools --boot-archive <archive>
"""
from pathlib import Path
import argparse
import json
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

MAIN = '''#include <vector>
#include <string>
#include <cstdio>

int main() {
    std::vector<std::string> v{"hello", "clangd"};
    for (const auto &s : v)
        std::printf("%s\\n", s.c_str());
    return 0;
}
'''

FLAGS = (
    "--target=aarch64-unknown-linux-elf -std=c++23 -nostdlibinc "
    "-mstrict-align -mno-outline-atomics -fno-stack-protector "
    "-D_SUSV2_SOURCE -D_POSIX_SOURCE -D_LIMITS_EXTENSION -D_GNU_SOURCE "
    "-DHARVEY -D_WCHAR_T "
    "-I{root}/include -I{root}/include/machine -I{root}/include/target "
    "-I{root}/include/c++ -I{root}/include/c++/v1 "
    "-I{root}/include/apex -I{root}/include/apex/aarch64 "
    "-include {root}/include/compat.h"
)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sdk-archive", default="/tmp/opencode/sdk-clangd.boot",
                    help="boot archive holding /boot/sdk.tar with clangd")
    ap.add_argument("--sdk-root", default="/initramfs/sdk",
                    help="guest SDK root inside the unpacked archive")
    ap.add_argument("--clangd", default="/tmp/opencode/sdk-clangd/bin/clangd",
                    help="host path used as the runner's nominal ELF; it is "
                         "not embedded because --in-tree names the guest copy")
    ap.add_argument("--stage", default="/tmp/opencode/clangdtest")
    ap.add_argument("--dump", default="/tmp/opencode/clangd.log")
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem", type=int, default=4096)
    ap.add_argument("--deadline", type=int, default=1800)
    ap.add_argument("--board-image", default="",
                    help="write a board Image and stop instead of running QEMU")
    args = ap.parse_args()

    archive = Path(args.sdk_archive)
    if not archive.is_file():
        sys.exit(f"missing {archive}; build it with install-sdk.py --boot-archive")
    if not Path(args.clangd).is_file():
        sys.exit(f"missing {args.clangd}; build clangd first")

    stage = Path(args.stage)
    shutil.rmtree(stage, ignore_errors=True)
    tdir = stage / "t"
    tdir.mkdir(parents=True)
    (tdir / "main.cpp").write_text(MAIN)
    command = (f"{args.sdk_root}/bin/clang++ {FLAGS.format(root=args.sdk_root)} "
               f"-resource-dir={args.sdk_root}/lib/clang/22 -c main.cpp")
    (tdir / "compile_commands.json").write_text(json.dumps([{
        "directory": "/initramfs/t",
        "file": "main.cpp",
        "command": command,
    }], indent=2) + "\n")

    runner = stage / "runner.elf"
    subprocess.run([
        sys.executable, str(HERE / "mkrun.py"), args.clangd,
        "--in-tree", f"{args.sdk_root}/bin/clangd",
        "--untar", "/boot/sdk.tar:/initramfs",
        "--tree", f"{tdir}:/initramfs/t",
        "--suffix", "", "--output", str(runner),
        "--", "--check=/initramfs/t/main.cpp",
        "--compile-commands-dir=/initramfs/t",
    ], check=True)

    if args.board_image:
        subprocess.run([
            sys.executable, str(HERE / "test_apex_boot.py"),
            "--elf", str(runner), "--image-out", args.board_image,
        ], check=True)
        return

    dump = Path(args.dump)
    subprocess.run([
        sys.executable, str(HERE / "test_apex_boot.py"),
        "--elf", str(runner), "--archive", str(archive),
        "--mem", str(args.mem), "--smp", str(args.smp),
        "--marker", "All checks completed, 0 errors",
        "--deadline", str(args.deadline), "--dump", str(dump),
    ], check=True)
    text = dump.read_text(errors="replace")
    for line in text.splitlines():
        if "clangd" in line or "All checks completed" in line:
            print(line)
    if "All checks completed, 0 errors" not in text:
        sys.exit("clangd check did not complete cleanly")


if __name__ == "__main__":
    main()
