#!/usr/bin/env python3
"""Build llvm-as inside Harvey and assemble a .ll file.

This sits on top of Core: the guest tablegen generates the headers, 490
translation units of AsmParser/BitWriter/MC/Core/Support compile, the
archives are made, and llvm-as turns test.ll into bitcode.

    util/clang-harvey/test-selfas.py
    util/clang-harvey/test-selfas.py /tmp/opencode/llvm-thr /path/to/llvm-project

Requires the guest llvm-tblgen from test-selfbuild.py.  Pass --clean to
remove the guest objects first.
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

TEST_LL = """\
define i32 @answer() {
entry:
  ret i32 42
}
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
        shutil.rmtree(GUEST / "lib", ignore_errors=True)
        shutil.rmtree(GUEST / "include", ignore_errors=True)
        (GUEST / "bin" / "llvm-as").unlink(missing_ok=True)

    run([sys.executable, str(HERE / "make-guest-build.py"), "--overlay",
         str(llvm_build), "llvm-as", str(llvm_src)])
    run([sys.executable, str(HERE / "make-guest-build.py"), "--overlay",
         str(llvm_build), "llvm-dis", str(llvm_src)])

    (GUEST / "test.ll").write_text(TEST_LL)
    run([str(HERE / "harvey-clang"), str(HERE / "run-astest.c"),
         "-o", str(GUEST / "run-astest.elf")])

    bc = GUEST / "test.bc"
    rt = GUEST / "roundtrip.ll"
    bc.unlink(missing_ok=True)
    rt.unlink(missing_ok=True)
    print("+", VM, "-m", "3584", "-boot", GUEST / "run-astest.elf", KERNEL)
    proc = subprocess.run(
        [str(VM), "-m", "3584", "-boot", str(GUEST / "run-astest.elf"),
         str(KERNEL)],
        cwd=REPO, text=True, capture_output=True, timeout=7200)
    output = proc.stdout + proc.stderr
    for line in output.splitlines():
        if line.strip() and any(w in line for w in
                                ("== llvm-as", "== llvm-dis", "hbuild:",
                                 "error:", "panic", "suicide")):
            print("  " + line)

    if "== llvm-as build status 0" not in output:
        sys.exit("the guest llvm-as build failed")
    if "== llvm-as run status 0" not in output:
        sys.exit("llvm-as failed")
    if "== llvm-dis build status 0" not in output:
        sys.exit("the guest llvm-dis build failed")
    if "== llvm-dis run status 0" not in output:
        sys.exit("llvm-dis failed")
    if not bc.is_file() or bc.stat().st_size == 0:
        sys.exit("llvm-as produced no bitcode")
    if bc.read_bytes()[:4] != b"BC\xc0\xde":
        sys.exit("test.bc does not start with the bitcode magic")
    text = rt.read_text()
    for want in ("define i32 @answer()", "ret i32 42"):
        if want not in text:
            sys.exit(f"roundtrip.ll lost {want!r}")
    elf = GUEST / "bin" / "llvm-as"
    diself = GUEST / "bin" / "llvm-dis"
    print(f"selfas: {elf.name} ({elf.stat().st_size} bytes) and "
          f"{diself.name} ({diself.stat().st_size} bytes) built on Harvey; "
          f"test.ll -> test.bc ({bc.stat().st_size} bytes) -> roundtrip.ll")


if __name__ == "__main__":
    main()
