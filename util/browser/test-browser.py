#!/usr/bin/env python3
"""Run the browser text pipeline on Harvey/ARM64 under QEMU.

    python3 util/browser/test-browser.py --build

Builds sys/src/cmd/browser into build/arm64-browser/browsertest, wraps it
with mkrun.py, boots it as init on QEMU virt, and expects:

    BROWSER-TEXT-OK
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stage", default="/tmp/opencode/browser-test")
    ap.add_argument("--v8", default=str(ROOT.parent / "v8"),
                    help="V8 checkout containing out/harvey/d8")
    ap.add_argument("--mem", type=int, default=2048)
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--deadline", type=int, default=300)
    ap.add_argument("--idle", type=int, default=60,
                    help="idle seconds before the harness gives up")
    ap.add_argument("--build", action="store_true",
                    help="run build-browser.py first")
    ap.add_argument("args", nargs="*", help="arguments passed to the target")
    args = ap.parse_args()

    if args.build:
        subprocess.run([sys.executable, str(HERE / "build-browser.py")],
                       check=True)

    elf = ROOT / "build" / "arm64-browser" / "browsertest"
    if not elf.exists():
        sys.exit(f"missing {elf}; run util/browser/build-browser.py")

    stage = Path(args.stage)
    stage.mkdir(parents=True, exist_ok=True)

    # stage only what this run needs; the kernel init image has a size limit
    v8 = Path(args.v8).resolve()
    modes = set(args.args)
    tree = stage / "tree" / "js"
    tree.mkdir(parents=True, exist_ok=True)
    if "hello" in modes:
        hello = ROOT / "build" / "arm64-apex" / "hello.elf"
        if not hello.exists():
            sys.exit(f"missing {hello}")
        shutil.copy2(hello, tree / "hello")
    elif "v8hello" in modes:
        v8hello = v8 / "out" / "harvey" / "v8_hello_world"
        if not v8hello.exists():
            sys.exit(f"missing {v8hello}; ninja -C {v8}/out/harvey v8_hello_world")
        subprocess.run(["llvm-strip", "--strip-all", str(v8hello), "-o",
                        str(tree / "v8hello")], check=True)
    elif "apexfree" in modes:
        af = Path("/tmp/opencode/apexfree")
        if not af.exists():
            sys.exit(f"missing {af}")
        shutil.copy2(af, tree / "apexfree")
    else:
        d8 = v8 / "out" / "harvey" / "d8"
        if not d8.exists():
            sys.exit(f"missing {d8}; build V8 with util/v8/build-v8.py")
        subprocess.run(["llvm-strip", "--strip-all", str(d8), "-o",
                        str(tree / "d8")], check=True)

    runner = stage / "runner.elf"
    subprocess.run([sys.executable, str(ROOT / "util/arm64/mkrun.py"),
                    str(elf), "--output", str(runner),
                    "--tree", f"{tree}:/js", "--suffix", "",
                    "--", *args.args], check=True)

    dump = stage / "log"
    subprocess.run([sys.executable,
                    str(ROOT / "util/arm64/test_apex_boot.py"),
                    "--elf", str(runner),
                    "--marker", "BROWSER-TEXT-OK",
                    "--allow-missing",
                    "--mem", str(args.mem), "--smp", str(args.smp),
                    "--deadline", str(args.deadline),
                    "--timeout", str(args.idle),
                    "--dump", str(dump)],
                   check=False)

    text = dump.read_text(errors="replace")
    if "BROWSER-TEXT-OK" not in text:
        for line in text.splitlines()[-40:]:
            print("   ", line)
        sys.exit("browser pipeline did not print BROWSER-TEXT-OK")
    print("browser textfs PASS")


if __name__ == "__main__":
    main()
