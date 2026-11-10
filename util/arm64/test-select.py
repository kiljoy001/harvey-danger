#!/usr/bin/env python3
"""Exercise APEX select()/poll() readiness in QEMU.

The probe program uses pipes (timeout, late data, child writer, EOF) and a
regular file.  select/poll are implemented with a blocking read probe in an
rfork(RFPROC|RFMEM) child; the probe parks one byte for the following
read().

    python3 util/arm64/test-select.py
"""
from pathlib import Path
import argparse
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SOURCE = HERE / "testdata" / "selectprobe.c"

EXPECT = [
    "SEL-TIMEOUT 0 1",
    "SEL-READY 1 1",
    "SEL-BYTE 1 Z",
    "POLL-TIMEOUT 0 1",
    "POLL-READY 1 1",
    "POLL-BYTE 1 Q 51",
    "POLL-CHILD 1 1",
    "POLL-CHILDBYTE 1 W",
    "POLL-EOF 1 1 1",
    "POLL-EOFRD 0",
    "POLL-FILE 1 1",
    "POLL-FILEBYTE 1 F",
    "SELPROBE-DONE",
]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stage", default="/tmp/opencode/select-test")
    ap.add_argument("--mem", type=int, default=2048)
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--deadline", type=int, default=120)
    args = ap.parse_args()

    stage = Path(args.stage)
    stage.mkdir(parents=True, exist_ok=True)
    binary = stage / "selectprobe"
    subprocess.run([str(HERE / "aarch64-clang"), str(SOURCE),
                    "-o", str(binary)], check=True)
    subprocess.run(["llvm-strip", "--strip-all", str(binary)], check=True)

    runner = stage / "runner.elf"
    subprocess.run([sys.executable, str(HERE / "mkrun.py"), str(binary),
                    "--output", str(runner)], check=True)

    dump = stage / "log"
    subprocess.run([
        sys.executable, str(HERE / "test_apex_boot.py"),
        "--elf", str(runner),
        "--marker", "SELPROBE-DONE",
        "--mem", str(args.mem), "--smp", str(args.smp),
        "--deadline", str(args.deadline), "--dump", str(dump),
    ], check=True)

    text = dump.read_text(errors="replace")
    missing = [line for line in EXPECT if line not in text]
    if missing:
        for line in text.splitlines():
            if line.startswith(("SEL", "POLL")):
                print("   ", line)
        sys.exit("missing select/poll results: " + ", ".join(missing))
    print("select/poll PASS")


if __name__ == "__main__":
    main()
