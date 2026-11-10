#!/usr/bin/env python3
"""Build compiler-rt sanitizer runtimes for Harvey/ARM64.

The first runtime is the minimal UBSan runtime.  It only needs atomics and
write(), so it compiles against APEX as is.  The archive is installed in
the SDK's clang resource directory under the name the driver expects for
aarch64-unknown-linux-elf, next to libclang_rt.ubsan_minimal.a.

    python3 util/arm64/build_sanitizers.py
    python3 util/arm64/build_sanitizers.py --sdk build/arm64-sdk

The driver is then able to link

    clang++ -fsanitize=undefined -fsanitize-minimal-runtime ...
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CC = HERE / "aarch64-clang++"
AR = shutil.which("llvm-ar") or "/usr/bin/llvm-ar"
TRIPLE = "aarch64-unknown-linux-elf"

SOURCES = {
    "ubsan_minimal": (
        "ubsan_minimal/ubsan_minimal_handlers.cpp",
        "libclang_rt.ubsan_minimal.a",
    ),
}


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

    for name, (source, archive) in SOURCES.items():
        src = cr / "lib" / source
        if not src.is_file():
            sys.exit(f"missing {src}")
        obj = objdir / f"{name}.o"
        cmd = [str(CC), "-O2", "-fno-exceptions", "-fno-rtti",
               "-I", str(cr / "lib"), "-I", str(cr / "lib" / name),
               "-c", str(src), "-o", str(obj)]
        print("+", " ".join(cmd))
        subprocess.run(cmd, check=True)
        out = libdir / archive
        subprocess.run([AR, "rcs", str(out), str(obj)], check=True)
        print(f"built {out}")


if __name__ == "__main__":
    main()
