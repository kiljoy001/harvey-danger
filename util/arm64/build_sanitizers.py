#!/usr/bin/env python3
"""Build compiler-rt sanitizer runtimes for Harvey/ARM64.

Two UBSan runtimes are installed in the SDK's clang resource directory:

- libclang_rt.ubsan_minimal.a from compiler-rt's ubsan_minimal handlers,
  used with -fsanitize=undefined -fsanitize-minimal-runtime;
- libclang_rt.ubsan_standalone.a from util/arm64/ubsan_standalone.c, used
  with the normal -fsanitize=undefined.  It implements the handler ABI
  directly (file:line:col and the check name) instead of pulling in
  sanitizer_common's Linux syscall layer.

    python3 util/arm64/build_sanitizers.py
    python3 util/arm64/build_sanitizers.py --sdk build/arm64-sdk

install-sdk.py --with-sanitizers calls this before writing the tar/boot
archive.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CXX = HERE / "aarch64-clang++"
CC = HERE / "aarch64-clang"
AR = shutil.which("llvm-ar") or "/usr/bin/llvm-ar"
TRIPLE = "aarch64-unknown-linux-elf"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--compiler-rt",
                    default="/home/scott/Repo/llvm-project/compiler-rt")
    ap.add_argument("--sdk", default=str(ROOT / "build" / "arm64-sdk"))
    ap.add_argument("--version-dir", default="22",
                    help="lib/clang/<version> directory in the SDK")
    ap.add_argument("--objdir", default="/tmp/opencode/sanitizer-obj")
    args = ap.parse_args()

    cr = Path(args.compiler_rt)
    sdk = Path(args.sdk)
    libdir = sdk / "lib" / "clang" / args.version_dir / "lib" / TRIPLE
    objdir = Path(args.objdir)
    objdir.mkdir(parents=True, exist_ok=True)
    libdir.mkdir(parents=True, exist_ok=True)

    builds = [
        (CXX, cr / "lib" / "ubsan_minimal" / "ubsan_minimal_handlers.cpp",
         "libclang_rt.ubsan_minimal.a",
         ["-fno-exceptions", "-fno-rtti",
          "-I", str(cr / "lib"),
          "-I", str(cr / "lib" / "ubsan_minimal")]),
        (CC, HERE / "ubsan_standalone.c",
         "libclang_rt.ubsan_standalone.a",
         ["-fno-sanitize=all"]),
    ]

    for compiler, src, archive, flags in builds:
        if not src.is_file():
            sys.exit(f"missing {src}")
        obj = objdir / (archive.removesuffix(".a") + ".o")
        cmd = [str(compiler), "-O2", *flags, "-c", str(src), "-o", str(obj)]
        print("+", " ".join(cmd))
        subprocess.run(cmd, check=True)
        out = libdir / archive
        out.unlink(missing_ok=True)
        subprocess.run([AR, "rcs", str(out), str(obj)], check=True)
        print(f"built {out}")


if __name__ == "__main__":
    main()
