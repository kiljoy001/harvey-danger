#!/usr/bin/env python3
"""Build a real LLVM translation unit on Harvey, with Harvey's own compiler.

This is the first self-hosting probe.  The guest compiles
llvm/lib/Support/CommandLine.cpp (an ordinary part of LLVM, not a toy),
compiles a driver that uses llvm::cl, links both against the prebuilt
libLLVMSupport.a, and runs the result:

    llvm-on-harvey: count=7 name=harvey

The guest sees the LLVM source and build trees through two symlinks:

    build/llvm-src   -> the llvm-project checkout
    build/llvm-build -> a Harvey-target LLVM build (the one passed to
                        install-sdk.py by default)

The compile flags are taken from that build's own ninja graph and rewritten
from host paths to the guest's /n/host paths, so the guest compiles the file
exactly the way the cross build did.

    util/clang-harvey/test-selfhost.py
    util/clang-harvey/test-selfhost.py /tmp/opencode/llvm-harvey /path/to/llvm-project
"""
import shlex
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BUILD = REPO / "build" / "clang-harvey"
GUEST = REPO / "build" / "llvm-guest"
SRC_LINK = REPO / "build" / "llvm-src"
BUILD_LINK = REPO / "build" / "llvm-build"
DEFAULT_BIN = Path("/tmp/opencode/llvm-harvey")
DEFAULT_SRC = Path("/home/scott/Repo/llvm-project")
VM = REPO / "util" / "9vm" / "9vm"
KERNEL = REPO / "build" / "harvey.32bit"

DRIVER = """\
/* A driver that exercises llvm/Support/CommandLine, compiled on Harvey. */
#include "llvm/Support/CommandLine.h"
#include <cstdio>
#include <string>

static llvm::cl::opt<int> Count("count", llvm::cl::desc("how many"),
                                llvm::cl::init(1));
static llvm::cl::opt<std::string> Name("name", llvm::cl::desc("a name"));

int main(int argc, char **argv) {
    llvm::cl::ParseCommandLineOptions(argc, argv, "harvey llvm demo\\n");
    int count = Count;
    std::printf("llvm-on-harvey: count=%d name=%s\\n", count, Name.c_str());
    return 0;
}
"""


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


def compile_argv(llvm_build: Path) -> list[str]:
    """Rewrite the build's own compile command into a guest command."""
    out = subprocess.run(["ninja", "-C", str(llvm_build), "-t", "commands",
                          "llvm-tblgen"],
                         text=True, capture_output=True, check=True).stdout
    line = next(l for l in out.splitlines() if "Support/CommandLine.cpp" in l)
    args = shlex.split(line)

    def guest(a: str) -> str:
        return (a.replace(str(llvm_build), "/n/host/build/llvm-build")
                 .replace(str(SRC_LINK.resolve()), "/n/host/build/llvm-src")
                 .replace(str(DEFAULT_SRC), "/n/host/build/llvm-src"))

    dropped_values = {"-o", "-MF", "-MT"}
    flags, i = [], 1
    while i < len(args):
        a = args[i]
        if a in dropped_values:
            i += 2
            continue
        if a == "-MD" or (a.startswith("-W") and a != "-Wno-pass-failed"):
            i += 1
            continue
        if not a.startswith("-"):
            i += 1
            continue
        flags.append(guest(a))
        i += 1

    return ["/n/host/build/clang-harvey/clang++", *flags,
            "-fno-exceptions", "-fno-rtti",
            "-c", "/n/host/build/llvm-src/llvm/lib/Support/CommandLine.cpp",
            "-o", "/n/host/build/llvm-guest/CommandLine.o"]


def main():
    llvm_build = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BIN
    llvm_src = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_SRC
    if not (llvm_build / "lib" / "libLLVMSupport.a").is_file():
        sys.exit(f"missing {llvm_build}/lib/libLLVMSupport.a")
    if not (llvm_src / "llvm" / "lib" / "Support" / "CommandLine.cpp").is_file():
        sys.exit(f"missing LLVM sources under {llvm_src}")

    run([sys.executable, str(HERE / "install-sdk.py"), str(llvm_build)])

    GUEST.mkdir(parents=True, exist_ok=True)
    (GUEST / "tmp").mkdir(exist_ok=True)
    for link, target in ((SRC_LINK, llvm_src), (BUILD_LINK, llvm_build)):
        link.unlink(missing_ok=True)
        link.symlink_to(target)
    (GUEST / "cl.cpp").write_text(DRIVER)

    argv = compile_argv(llvm_build)
    (GUEST / "ccargs.txt").write_text("\n".join(argv) + "\n")
    for stale in ("CommandLine.o", "cl.o", "cl", "selfhost.elf"):
        (GUEST / stale).unlink(missing_ok=True)

    run([str(HERE / "harvey-clang"), str(HERE / "run-selfhost.c"),
         "-o", str(GUEST / "selfhost.elf")])

    proc = subprocess.run(
        [str(VM), "-m", "3072", "-boot", str(GUEST / "selfhost.elf"), str(KERNEL)],
        cwd=REPO, text=True, capture_output=True, timeout=1800)
    output = proc.stdout + proc.stderr
    for line in output.splitlines():
        if line.strip() and any(w in line for w in
                                ("==", "llvm-on-harvey", "error", "panic",
                                 "fault", "suicide")):
            print("  " + line)

    if "llvm-on-harvey: count=7 name=harvey" not in output:
        sys.exit("the guest did not build and run the LLVM driver")
    elf = GUEST / "cl"
    print(f"selfhost: CommandLine.cpp -> CommandLine.o -> {elf.name} "
          f"({elf.stat().st_size} bytes) ran and printed "
          f"'llvm-on-harvey: count=7 name=harvey'")


if __name__ == "__main__":
    main()
