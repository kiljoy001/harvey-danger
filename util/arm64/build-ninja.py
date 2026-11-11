#!/usr/bin/env python3
"""Cross-build ninja 1.11 for Harvey/ARM64.

Applies util/arm64/ninja/ninja-harvey.diff (st_mtim, sysinfo/load average,
and SHELL for /bin/rc) and runs ninja's configure.py bootstrap with the
Harvey wrapper.  The bootstrap ends by trying to execute the ARM64 binary
on the host, so a non-zero exit is accepted when the binary exists.

    util/arm64/build-ninja.py --source /path/to/ninja-1.11.1
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PATCH = HERE / "ninja" / "ninja-harvey.diff"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", required=True, type=Path)
    ap.add_argument("--output", type=Path,
                    default=Path("/tmp/opencode/ninja-harvey"))
    args = ap.parse_args()

    src = args.source.resolve()
    if not (src / "configure.py").is_file():
        sys.exit(f"{src} does not look like a ninja source tree")

    marker = src / "src" / "disk_interface.cc"
    if "defined(HARVEY)" not in marker.read_text(errors="replace"):
        subprocess.run(["patch", "-p1", "-N", "-i", str(PATCH)],
                       cwd=src, check=False)
    if "defined(HARVEY)" not in marker.read_text(errors="replace"):
        sys.exit(f"failed to apply {PATCH}")

    binary = src / "ninja"
    binary.unlink(missing_ok=True)
    env = dict(os.environ)
    env["CXX"] = str(HERE / "aarch64-clang++")
    proc = subprocess.run([sys.executable, "configure.py", "--bootstrap"],
                          cwd=src, env=env)
    if not binary.is_file():
        sys.exit("ninja bootstrap did not produce a binary")
    # configure.py ends by executing ./ninja on the host; that failure is
    # expected for a cross build.

    out = args.output
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(binary, out)
    subprocess.run(["llvm-strip", "--strip-all", str(out)], check=True)
    print(f"Harvey ninja: {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
