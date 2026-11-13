#!/usr/bin/env python3
"""Run V8's d8 on Harvey/ARM64 under QEMU.

The V8 checkout is not vendored; build it first with:

    python3 util/v8/build-v8.py --v8 ~/Repo/v8

Then:

    python3 util/v8/test-v8.py --v8 ~/Repo/v8

The test strips d8, wraps it with mkrun.py, boots it as init, and expects:

    D8-OK 3
"""
from pathlib import Path
import argparse
import os
import subprocess
import sys

HERE = Path(__file__).resolve().parent
HARVEY = HERE.parents[1]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--v8", type=Path,
                    default=Path(os.environ.get("V8", HARVEY.parent / "v8")))
    ap.add_argument("--stage", default="/tmp/opencode/v8-test")
    ap.add_argument("--mem", type=int, default=2048)
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--deadline", type=int, default=180)
    ap.add_argument("--build", action="store_true",
                    help="run build-v8.py before testing")
    args = ap.parse_args()

    v8 = args.v8.resolve()
    d8 = v8 / "out" / "harvey" / "d8"
    if args.build:
        subprocess.run([sys.executable, str(HERE / "build-v8.py"),
                        "--v8", str(v8)], check=True)
    if not d8.exists():
        sys.exit(f"missing {d8}; run util/v8/build-v8.py")

    stage = Path(args.stage)
    stage.mkdir(parents=True, exist_ok=True)
    img = stage / "d8"
    subprocess.run(["llvm-strip", "--strip-all", str(d8), "-o", str(img)],
                   check=True)

    runner = stage / "runner.elf"
    subprocess.run([sys.executable, str(HARVEY / "util/arm64/mkrun.py"),
                    str(img), "--output", str(runner),
                    "--", "-e", 'print("D8-OK", 1+2)'], check=True)

    dump = stage / "log"
    subprocess.run([
        sys.executable, str(HARVEY / "util/arm64/test_apex_boot.py"),
        "--elf", str(runner),
        "--marker", "D8-OK 3",
        "--allow-missing",
        "--mem", str(args.mem), "--smp", str(args.smp),
        "--deadline", str(args.deadline), "--dump", str(dump),
    ], check=False)

    text = dump.read_text(errors="replace")
    if "D8-OK 3" not in text:
        for line in text.splitlines()[-30:]:
            print("   ", line)
        sys.exit("d8 did not print D8-OK 3")
    print("V8 d8 PASS")


if __name__ == "__main__":
    main()
