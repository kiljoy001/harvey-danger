#!/usr/bin/env python3
"""Verify o9irc binaries and its 9P view in an isolated Aspera VM."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True,
                        help="installed 9front image used to format GEFS")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="o9irc-image-test-", dir=ROOT / "build") as name:
        image = Path(name) / "aspera.img"
        subprocess.run([sys.executable, str(HERE / "format_gefs_image.py"),
                        "--base", str(args.base), str(image)], check=True)
        subprocess.run([sys.executable, str(HERE / "install_o9irc.py"),
                        "--base", str(args.base), "--image", str(image)],
                       check=True)
        subprocess.run([sys.executable, str(HERE / "build_kernel.py")],
                       cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        command = [str(HERE / "9vm"), "-m", "512", "-r", "/",
                   "-gefs-image", str(image), str(ROOT / "build/harvey.32bit")]
        try:
            proc = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, timeout=14)
            output = proc.stdout.decode("utf-8", "replace")
        except subprocess.TimeoutExpired as error:
            output = (error.stdout or b"").decode("utf-8", "replace")
        required = (
            "[9vm] GEFS root mode: only the image file is exported",
            "ASPERA-O9IRC-VIEW PASS",
            "ASPERA-O9IRC-SERVICE PASS",
            "ASPERA-GEFS-ROOT PASS",
        )
        forbidden = ("In-process hostfs online serving", "init: mounting #~/host",
                     "hangup:", "panic:", "suicide:", "aspera: o9irc")
        if (any(marker not in output for marker in required) or
                any(marker in output for marker in forbidden)):
            raise RuntimeError("isolated o9irc boot failed:\n" + output[-4000:])
    print("ASPERA-O9IRC-IMAGE PASS native GEFS install and mounted 9P view")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"test_o9irc_image: {error}", file=sys.stderr)
        raise SystemExit(1)
