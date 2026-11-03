#!/usr/bin/env python3
"""Lay out a Harvey SDK for the native clang.

Copies the cross-built clang, clang++ and ld.lld into build/clang-harvey,
installs clang's own resource headers where the driver expects them, drops
the driver configuration files next to the binary, and creates the /tmp that
the guest's read-only root lacks.

    util/clang-harvey/install-sdk.py                 # /tmp/opencode/llvm-harvey/bin
    util/clang-harvey/install-sdk.py /path/to/bin    # a bin/ with clang-22 and lld

The result is a guest directory where `clang++ hello.cpp -o hello` works with
no flags: the driver finds clang++.cfg through its InstalledDir, derives its
resource directory from there, and invokes the ld.lld sitting beside it.
"""
import os
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
DEFAULT_BIN = Path("/tmp/opencode/llvm-harvey")
BUILD = REPO / "build" / "clang-harvey"
SDK = HERE / "sdk"

# The guest mounts this checkout at /n/host, so these are the guest-visible
# equivalents of the build tree.
CLANG_VERSION = "22"


def main():
    llvm_build = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BIN
    clang = llvm_build / "bin" / "clang-22"
    lld = llvm_build / "bin" / "lld"
    for path in (clang, lld):
        if not path.is_file():
            sys.exit(f"missing {path}; build LLVM for Harvey first")

    BUILD.mkdir(parents=True, exist_ok=True)
    ar = llvm_build / "bin" / "llvm-ar"
    tools = (("clang", clang), ("clang++", clang), ("ld.lld", lld))
    if ar.is_file():
        tools += (("llvm-ar", ar), ("llvm-ranlib", ar))
    for name, src in tools:
        dst = BUILD / name
        dst.unlink(missing_ok=True)
        shutil.copyfile(src, dst)
        os.chmod(dst, 0o755)

    for cfg in SDK.glob("*.cfg"):
        shutil.copyfile(cfg, BUILD / cfg.name)

    headers = llvm_build / "lib" / "clang" / CLANG_VERSION / "include"
    if not headers.is_dir():
        sys.exit(f"missing clang resource headers at {headers}")
    resource = REPO / "build" / "lib" / "clang" / CLANG_VERSION / "include"
    if resource.exists():
        shutil.rmtree(resource)
    resource.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(headers, resource)

    # The guest's / is a read-only ramfs, so clang's default /tmp does not
    # exist.  Create it on the host side of the mount.
    (REPO / "tmp").mkdir(exist_ok=True)

    print(f"clang, clang++ and ld.lld  -> {BUILD}")
    print(f"resource headers           -> {resource}")
    print(f"guest /tmp                 -> {REPO / 'tmp'}")


if __name__ == "__main__":
    main()
