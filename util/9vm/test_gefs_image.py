#!/usr/bin/env python3
"""Verify 9front GEFS formatting and 9vm's isolated image transport."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
VM = ROOT / "util/9vm/9vm"
KERNEL = ROOT / "build/harvey.32bit"
FORMAT = ROOT / "util/9vm/format_gefs_image.py"


def boot(image, extra, marker, use_ring=True):
    cmdline = "bootargs=local!#9/0 nobootprompt=local O9_GEFS_IMAGE_TEST=1"
    if extra:
        cmdline += " " + extra
    command = [str(VM), "-legacy-host-root"] + ([] if use_ring else ["-no9ring"]) + [
               "-m", "512", "-r", str(ROOT),
               "-gefs-image", str(image), "-c", cmdline, str(KERNEL)]
    try:
        result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=8)
        output = result.stdout
    except subprocess.TimeoutExpired as error:
        output = error.stdout or b""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
    if (marker not in output or "o9-gefs-image: FAIL" in output or
            "panic:" in output or "suicide:" in output):
        raise RuntimeError(f"9vm image test failed; expected {marker}:\n{output[-3000:]}")
    if "O9_GEFS_IMAGE_ISOLATION_TEST=1" in extra and \
            "o9-gefs-image: PASS image isolation" not in output:
        raise RuntimeError("9vm image export did not prove isolation")
    if "O9_GEFS_IMAGE_CREATE_DENIAL_TEST=1" in extra and \
            "o9-gefs-image: PASS create isolation" not in output:
        raise RuntimeError("9vm image export did not deny create")
    if '#9/2: virtio9p tag "gefs-image"' not in output:
        raise RuntimeError("9vm did not probe the isolated image device")
    print(marker)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path,
                        help="installed 9front image used to format GEFS")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="9vm-gefs-test-", dir=ROOT / "build") as temp:
        temp = Path(temp)
        gefs = temp / "gefs.img"
        subprocess.run([sys.executable, str(FORMAT), "--base", str(args.base),
                        str(gefs)], check=True)
        boot(gefs, "", "o9-gefs-image: PASS read image")
        boot(gefs, "", "o9-gefs-image: PASS read image", use_ring=False)

        scratch = temp / "scratch.img"
        with scratch.open("wb") as stream:
            stream.truncate(256 * 1024 * 1024)
        (temp / "sibling").write_text("not exported")
        boot(scratch, "O9_GEFS_IMAGE_WRITE_TEST=1 O9_GEFS_IMAGE_ISOLATION_TEST=1 "
             "O9_GEFS_IMAGE_CREATE_DENIAL_TEST=1",
             "o9-gefs-image: PASS write/read")
        boot(scratch, "O9_GEFS_IMAGE_EXPECT_MARKER=1", "o9-gefs-image: PASS read image")
        with scratch.open("rb") as stream:
            if stream.read(15) != b"9vm-image-write":
                raise RuntimeError("image contents did not persist on the host")
        print("9VM-GEFS-IMAGE PASS format, guest transport, reboot persistence")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print("test_gefs_image:", error, file=sys.stderr)
        raise SystemExit(1)
