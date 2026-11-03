#!/usr/bin/env python3
"""Build LLVMCore inside Harvey and link a program against it.

This is the step after test-selfbuild.py: the guest already has a native
llvm-tblgen, so its 26 header-generation commands run first, then 121
translation units compile against those guest-generated headers, then the
seven archives are made.  A small program links against libLLVMCore.a and
prints the module it builds with IRBuilder.

    util/clang-harvey/test-selfcore.py
    util/clang-harvey/test-selfcore.py /tmp/opencode/llvm-thr /path/to/llvm-project

Requires build/llvm-guest/bin/llvm-tblgen, i.e. run test-selfbuild.py first.
Pass --clean to throw away the guest objects and rebuild from scratch.
"""
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
GUEST = REPO / "build" / "llvm-guest"
SRC_LINK = REPO / "build" / "llvm-src"
BUILD_LINK = REPO / "build" / "llvm-build"
VM = REPO / "util" / "9vm" / "9vm"
KERNEL = REPO / "build" / "harvey.32bit"
DEFAULT_BIN = Path("/tmp/opencode/llvm-thr")
DEFAULT_SRC = Path("/home/scott/Repo/llvm-project")

CORETEST_BUILD = """\
/n/host/build/clang-harvey/clang++ -std=c++17 -fno-exceptions -fno-rtti \
-I/n/host/build/llvm-guest/include -I/n/host/build/llvm-src/llvm/include \
-c /n/host/build/llvm-guest/coretest.cpp \
-o /n/host/build/llvm-guest/coretest.o
---
/n/host/build/clang-harvey/clang++ -o /n/host/build/llvm-guest/bin/coretest \
/n/host/build/llvm-guest/coretest.o \
/n/host/build/llvm-guest/lib/libLLVMCore.a \
/n/host/build/llvm-guest/lib/libLLVMBinaryFormat.a \
/n/host/build/llvm-guest/lib/libLLVMBitstreamReader.a \
/n/host/build/llvm-guest/lib/libLLVMRemarks.a \
/n/host/build/llvm-guest/lib/libLLVMDemangle.a \
/n/host/build/llvm-guest/lib/libLLVMSupport.a \
/n/host/build/llvm-guest/lib/libLLVMTargetParser.a
"""


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


def main():
    argv = [a for a in sys.argv[1:] if a != "--clean"]
    clean = len(argv) != len(sys.argv) - 1
    llvm_build = Path(argv[0]) if len(argv) > 0 else DEFAULT_BIN
    llvm_src = Path(argv[1]) if len(argv) > 1 else DEFAULT_SRC

    if not (GUEST / "bin" / "llvm-tblgen").is_file():
        sys.exit("run test-selfbuild.py first: the guest llvm-tblgen is "
                 "missing")

    run([sys.executable, str(HERE / "install-sdk.py"), str(llvm_build)])

    for link, target in ((SRC_LINK, llvm_src), (BUILD_LINK, llvm_build)):
        link.unlink(missing_ok=True)
        link.symlink_to(target)

    GUEST.mkdir(parents=True, exist_ok=True)
    (GUEST / "tmp").mkdir(exist_ok=True)
    if clean:
        # Keep bin/ (the tablegen we need) and gen/; drop objects and the
        # mirrored/generated headers.
        shutil.rmtree(GUEST / "lib", ignore_errors=True)
        shutil.rmtree(GUEST / "include", ignore_errors=True)
        (GUEST / "bin" / "coretest").unlink(missing_ok=True)

    run([sys.executable, str(HERE / "make-guest-build.py"), "--overlay",
         str(llvm_build), "LLVMCore", str(llvm_src)])

    shutil.copyfile(HERE / "coretest.cpp", GUEST / "coretest.cpp")
    (GUEST / "coretest.build").write_text(CORETEST_BUILD)

    run([str(HERE / "harvey-clang"), str(HERE / "run-coretest.c"),
         "-o", str(GUEST / "run-coretest.elf")])

    print("+", VM, "-m", "3072", "-boot", GUEST / "run-coretest.elf", KERNEL)
    proc = subprocess.run(
        [str(VM), "-m", "3072", "-boot", str(GUEST / "run-coretest.elf"),
         str(KERNEL)],
        cwd=REPO, text=True, capture_output=True, timeout=7200)
    output = proc.stdout + proc.stderr
    for line in output.splitlines():
        if line.strip() and any(w in line for w in
                                ("== LLVMCore", "== coretest", "hbuild:",
                                 "ModuleID", "source_filename",
                                 "define i32 @answer", "ret i32 42",
                                 "panic", "suicide")):
            print("  " + line)

    for want in ("== LLVMCore status 0", "== coretest build status 0",
                 "define i32 @answer()", "ret i32 42"):
        if want not in output:
            sys.exit(f"missing {want!r} in the guest output")
    elf = GUEST / "bin" / "coretest"
    if not elf.is_file() or elf.stat().st_size == 0:
        sys.exit("no coretest was produced")
    print(f"selfcore: LLVMCore built on Harvey; {elf.name} "
          f"({elf.stat().st_size} bytes) linked against it and printed "
          f"@answer() -> 42")


if __name__ == "__main__":
    main()
