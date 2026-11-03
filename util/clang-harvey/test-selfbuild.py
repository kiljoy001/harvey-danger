#!/usr/bin/env python3
"""Build LLVM's TableGen inside Harvey, with Harvey's own compiler.

This is the self-hosting step after test-selfhost.py.  It takes the cross
build's ninja command graph for llvm-tblgen, rewrites it for the guest, and
runs it there with hbuild: all compiles, then the archives, then the link.
The result is a native llvm-tblgen in the guest:

    llvm-tblgen --version

The four BLAKE3 assembly objects are reused from the cross build, because
the guest driver's config file force-includes a C header and the assembler
cannot parse it; they were compiled for the same target with the same flags.

    util/clang-harvey/test-selfbuild.py
    util/clang-harvey/test-selfbuild.py /tmp/opencode/llvm-thr /path/to/llvm-project
"""
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
GUEST = REPO / "build" / "llvm-guest"
DOOBJ = GUEST / "lib"
SRC_LINK = REPO / "build" / "llvm-src"
BUILD_LINK = REPO / "build" / "llvm-build"
VM = REPO / "util" / "9vm" / "9vm"
KERNEL = REPO / "build" / "harvey.32bit"
DEFAULT_BIN = Path("/tmp/opencode/llvm-thr")
DEFAULT_SRC = Path("/home/scott/Repo/llvm-project")


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


def main():
    llvm_build = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BIN
    llvm_src = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_SRC
    if not (llvm_build / "bin" / "llvm-ar").is_file():
        sys.exit(f"missing {llvm_build}/bin/llvm-ar")
    if not (llvm_src / "llvm" / "lib" / "Support").is_dir():
        sys.exit(f"missing LLVM sources under {llvm_src}")

    run([sys.executable, str(HERE / "install-sdk.py"), str(llvm_build)])

    for link, target in ((SRC_LINK, llvm_src), (BUILD_LINK, llvm_build)):
        link.unlink(missing_ok=True)
        link.symlink_to(target)

    GUEST.mkdir(parents=True, exist_ok=True)
    (GUEST / "tmp").mkdir(exist_ok=True)
    for stale in (DOOBJ, GUEST / "utils", GUEST / "include",
                  GUEST / "bin" / "llvm-tblgen"):
        if stale.is_dir():
            shutil.rmtree(stale)
        else:
            stale.unlink(missing_ok=True)

    run([sys.executable, str(HERE / "make-guest-build.py"),
         str(llvm_build), "llvm-tblgen", str(llvm_src)])

    run([str(HERE / "harvey-clang"), str(HERE / "hbuild.c"),
         "-o", str(GUEST / "hbuild")])
    run([str(HERE / "harvey-clang"), str(HERE / "run-selfbuild.c"),
         "-o", str(GUEST / "run-selfbuild.elf")])

    print("+", VM, "-m", "3072", "-boot", GUEST / "run-selfbuild.elf", KERNEL)
    proc = subprocess.run(
        [str(VM), "-m", "3072", "-boot", str(GUEST / "run-selfbuild.elf"),
         str(KERNEL)],
        cwd=REPO, text=True, capture_output=True, timeout=14400)
    output = proc.stdout + proc.stderr
    for line in output.splitlines():
        if line.strip() and any(w in line for w in
                                ("hbuild:", "== hbuild", "LLVM version",
                                 "error:", "panic", "suicide", "fault")):
            print("  " + line)

    if "== hbuild status 0" not in output:
        sys.exit("the guest build failed")
    if "LLVM version" not in output:
        sys.exit("llvm-tblgen --version did not run in the guest")
    elf = GUEST / "bin" / "llvm-tblgen"
    if not elf.is_file() or elf.stat().st_size == 0:
        sys.exit("no llvm-tblgen was produced")
    print(f"selfbuild: {len(list(GUEST.rglob('*.o')))} objects -> "
          f"{elf.name} ({elf.stat().st_size} bytes) built and run on Harvey")


if __name__ == "__main__":
    main()
