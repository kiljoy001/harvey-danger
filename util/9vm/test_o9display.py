#!/usr/bin/env python3
"""Run Objective-9C display tests inside the Harvey VM.

The mutation lane changes one source expression at a time in a temporary
directory. It requires an explicit guest FAIL result for each mutant.
"""

import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCES = ROOT / "sys/src/cmd/o9display"
PRISM = ROOT / "util/9vm/prism.py"
CASES = (
    ("state", 512),
    ("buffer", 512),
    ("memory", 512),
    ("pool", 512),
    ("treegc", 512),
    ("property", 512),
    ("service", 512),
)
MUTANTS = (
    ("reject_foreign_owner", "state", "state.o9",
     "if(surface.id() == id) {\n                if(surface.owner() != owner) { return false; }",
     "if(surface.id() == id) {\n                if(surface.owner() == owner) { return false; }"),
    ("reject_wrong_width", "state", "state.o9",
     "width != pixelWidth || height != pixelHeight",
     "width == pixelWidth || height != pixelHeight"),
    ("accept_exact_end", "buffer", "buffer.o9",
     "offset > total || count > total - offset",
     "offset > total || count >= total - offset"),
)


def guest_run(source: Path, name: str, ram: int) -> str:
    timeout = 60 if name in ("memory", "pool") else 25
    cmd = [sys.executable, str(PRISM), "run", str(source),
           "--ram", str(ram), "--timeout", str(timeout)]
    process = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True,
                               start_new_session=True)
    try:
        output, _ = process.communicate(timeout=timeout + 10)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise RuntimeError(f"{name}: VM timed out")
    if process.returncode != 0 or "panic:" in output or "suicide:" in output:
        raise RuntimeError(f"{name}: VM failed\n{output[-1800:]}")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutate", action="store_true",
                        help="also run three source mutations")
    args = parser.parse_args()
    for name, ram in CASES:
        output = guest_run(SOURCES / f"test_{name}.o9", name, ram)
        marker = f"O9DISPLAY-{name.upper() if name != 'service' else '9P'} PASS"
        if marker not in output:
            raise RuntimeError(f"{name}: missing {marker}\n{output[-1800:]}")
        print(f"{name}: PASS ({ram} MiB guest)")
    if not args.mutate:
        return 0

    with tempfile.TemporaryDirectory(prefix="o9display_mutant_") as tmp:
        work = Path(tmp)
        for library in ("string.o9", "bytes.o9"):
            (work / library).symlink_to((SOURCES / library).resolve())
        for label, case, target, original, replacement in MUTANTS:
            for filename in ("state.o9", "buffer.o9", f"test_{case}.o9"):
                shutil.copyfile(SOURCES / filename, work / filename)
            path = work / target
            source = path.read_text()
            if source.count(original) != 1:
                raise RuntimeError(f"{label}: mutation site changed")
            path.write_text(source.replace(original, replacement, 1))
            output = guest_run(work / f"test_{case}.o9", label, 512)
            marker = f"O9DISPLAY-{case.upper()} FAIL"
            if marker not in output:
                raise RuntimeError(f"{label}: mutant survived or gave no test result\n{output[-1800:]}")
            print(f"{label}: killed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
