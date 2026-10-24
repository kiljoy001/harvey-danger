#!/usr/bin/env python3
"""Create a GEFS filesystem in a 9P-exported image for 9vm.

Boots the Harvey kernel with ``install=1`` and the target image exported
as VirtIO 9P tag ``gefs-image`` (#9/2).  The host tree is exported too
(``-legacy-host-root``) so the guest can copy the staged root.  The guest
(boot/gefs_init.o9):

  1. mounts #9/2 at /mnt,
  2. runs ``gefs -r`` on /mnt/image,
  3. serves it, copies the staged root, syncs and halts,
  4. re-opens the image with a fresh GEFS and verifies /bin/rc.

The kernel has no block driver: GEFS runs in user space on the 9P file.
GEFS requires at least 128 MiB; the default is 256 MiB.

    ./install_gefs.py -o /tmp/9vm/gefs.img -s 256
"""
import argparse
import os
import selectors
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VM = ROOT / "util/9vm/9vm"
KERNEL = ROOT / "build/harvey.32bit"
CMDLINE = "bootargs=local!#9/0 nobootprompt=local gefsroot=1 install=1"
GEFS_MAGIC = b"gefs9."
MIN_MIB = 160

# Minimal bootable root: what the installer copies onto the new filesystem.
STAGE = (
    ("bin/rc", "build/aspera-rc"),
    ("bin/ls", "build/aspera-ls"),
    ("bin/cat", "build/aspera-cat"),
    ("bin/test", "build/aspera-test"),
    ("bin/echo", "build/aspera-echo"),
    ("rc/lib/rcmain", "rc/lib/rcmain"),
    ("boot/boot", "boot/boot"),
    ("boot/gefs", "build/gefs-guest"),
)


def make_stage(stage: Path) -> None:
    import shutil
    for dest, src in STAGE:
        target = stage / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        source = ROOT / src
        if not source.is_file():
            raise SystemExit(f"missing staging source {src}")
        shutil.copy2(source, target)


def run(image: Path, stage: Path, timeout: float) -> str:
    proc = subprocess.Popen(
        [str(VM), "-legacy-host-root", "-r", str(stage),
         "-gefs-image", str(image), "-c", CMDLINE, str(KERNEL)],
        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    output = bytearray()
    deadline = time.monotonic() + timeout
    try:
        while time.monotonic() < deadline:
            if not sel.select(timeout=0.25):
                if proc.poll() is not None:
                    break
                continue
            chunk = os.read(proc.stdout.fileno(), 65536)
            if not chunk:
                break
            output.extend(chunk)
            if b"INSTALL-OK" in output:
                break
    finally:
        sel.close()
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        output.extend(proc.stdout.read())
        proc.stdout.close()
    return output.decode(errors="replace")


def verify(image: Path) -> list[str]:
    problems = []
    with image.open("rb") as f:
        if f.read(len(GEFS_MAGIC)) != GEFS_MAGIC:
            problems.append("no gefs superblock magic at offset 0")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", default="/tmp/9vm/gefs.img",
                    help="image to create (default /tmp/9vm/gefs.img)")
    ap.add_argument("-s", "--size", type=int, default=256,
                    help=f"image size in MiB (default 256, minimum {MIN_MIB})")
    ap.add_argument("--stage", default="/tmp/9vm/stage",
                    help="staged minimal root the guest copies (default /tmp/9vm/stage)")
    ap.add_argument("--timeout", type=float, default=180.0)
    args = ap.parse_args()

    if args.size < MIN_MIB:
        ap.error(f"size must be at least {MIN_MIB} MiB (GEFS needs 128 MiB)")
    for path in (VM, KERNEL):
        if not path.is_file():
            print(f"missing {path.relative_to(ROOT)}; build it first", file=sys.stderr)
            return 1

    image = Path(args.output)
    image.parent.mkdir(parents=True, exist_ok=True)
    with image.open("wb") as f:
        f.truncate(args.size * 1024 * 1024)
    print(f"created {image} ({args.size} MiB)")

    stage = Path(args.stage)
    make_stage(stage)
    log = run(image, stage, args.timeout)
    if "INSTALL-OK" not in log:
        print("install failed:", file=sys.stderr)
        tail = "\n".join(log.strip().splitlines()[-25:])
        print(tail, file=sys.stderr)
        return 1
    for line in log.splitlines():
        if line.startswith(("install:", "reaming", "sdvirtio:")):
            print("  " + line.rstrip())

    problems = verify(image)
    if problems:
        print("verification failed: " + "; ".join(problems), file=sys.stderr)
        return 1
    print(f"{image}: GEFS superblock verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
