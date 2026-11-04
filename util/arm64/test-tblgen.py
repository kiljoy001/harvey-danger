#!/usr/bin/env python3
"""Run llvm-tblgen on Harvey/ARM64 and compare its output with the host tool.

The ARM64 kernel starts its first process without descriptors or
arguments, so the TableGen binary and the .td tree are embedded in a
launcher (see mkrun.py), written to a ramfs and exec'd with a command line.
The generated value-type fragment is captured from the console and must be
byte-identical to what the host build of the same revision produces.

    util/arm64/test-tblgen.py
    util/arm64/test-tblgen.py /tmp/opencode/llvm-arm64/bin/llvm-tblgen
"""
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEFAULT_TBLGEN = Path("/tmp/opencode/llvm-arm64/bin/llvm-tblgen")
HOST_TBLGEN = Path("/tmp/opencode/llvm-host/bin/llvm-tblgen")
LLVM = ROOT.parent / "llvm-project"
INCLUDE = LLVM / "llvm" / "include"
RUNNER = ROOT / "build" / "arm64-run" / "runner.elf"
LOG = ROOT / "build" / "arm64-run" / "tblgen.log"


def run(cmd: list[str]) -> None:
    print("+", " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True)


def main() -> None:
    tblgen = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_TBLGEN
    if not tblgen.is_file():
        sys.exit(f"missing {tblgen}; build it with the aarch64 toolchain")
    if not HOST_TBLGEN.is_file():
        sys.exit(f"missing host reference {HOST_TBLGEN}")

    run([sys.executable, str(HERE / "mkrun.py"), str(tblgen),
         "--tree", f"{INCLUDE}:/td",
         "--", "-gen-vt", "-I", "/td", "/td/llvm/CodeGen/ValueTypes.td"])
    run([sys.executable, str(HERE / "test_apex_boot.py"),
         "--elf", str(RUNNER), "--marker", "MARKER-NEVER-MATCHES", "--allow-missing",
         "--dump", str(LOG)])

    transcript = LOG.read_bytes().replace(b"\r\n", b"\n")
    marker = b"/*===- TableGen'erated file"
    start = transcript.find(marker)
    if start < 0:
        sys.exit("tablegen produced no output")
    guest = transcript[start:]
    for tail in (b"[init] PID", b"panic:"):
        end = guest.find(tail)
        if end > 0:
            guest = guest[:end]

    reference = subprocess.run(
        [str(HOST_TBLGEN), "-gen-vt", "-I", str(INCLUDE),
         str(INCLUDE / "llvm" / "CodeGen" / "ValueTypes.td")],
        check=True, capture_output=True).stdout

    (ROOT / "build" / "arm64-run" / "genvt-guest.inc").write_bytes(guest)
    if guest != reference:
        sys.exit(f"tablegen output differs "
                 f"(guest {len(guest)} bytes, host {len(reference)})")
    print(f"tblgen: {len(guest)} bytes generated on Harvey/ARM64, "
          f"identical to the host tool")


if __name__ == "__main__":
    main()
