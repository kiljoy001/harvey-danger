#!/usr/bin/env python3
"""Verify the packaged driver manager and filesystem survive a heartbeat."""

import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
VM = HERE / "9vm"
KERNEL = ROOT / "build/harvey.32bit"
DEFAULT_IMAGE = ROOT / "build/aspera-desktop-gefs.img"


def terminate(pid: int) -> None:
    try:
        os.killpg(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass


def boot(image: Path) -> None:
    command = [
        str(VM), "-m", "512", "-r", "/", "-gefs-image", str(image),
        str(KERNEL),
    ]
    process = subprocess.Popen(
        command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    try:
        output, _ = process.communicate(timeout=32)
    except subprocess.TimeoutExpired as error:
        output = error.stdout or b""
    finally:
        if process.poll() is None:
            terminate(process.pid)
            process.wait(timeout=5)
    text = output.decode("utf-8", "replace")
    required = (
        "[9vm] GEFS root mode: only the image file is exported",
        "ASPERA-DRIVERS-STARTED",
        "ASPERA-GEFS-ROOT PASS",
    )
    forbidden = (
        "aspera: driver", "o9driverfs:", "panic:", "suicide:",
        "[init] PID 1 exited",
    )
    missing = [marker for marker in required if marker not in text]
    present = [marker for marker in forbidden if marker in text]
    if missing or present:
        raise RuntimeError(
            f"driver image boot failed; missing={missing}, forbidden={present}:\n"
            + text[-5000:]
        )


def provision(base: Path, image: Path) -> None:
    subprocess.run([
        sys.executable, str(HERE / "format_gefs_image.py"), "--base",
        str(base), str(image),
    ], cwd=ROOT, check=True)
    subprocess.run([
        sys.executable, str(HERE / "install_aspera.py"), "--base",
        str(base), "--image", str(image),
    ], cwd=ROOT, check=True)
    subprocess.run([sys.executable, str(HERE / "build_kernel.py")],
                   cwd=ROOT, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=DEFAULT_IMAGE,
                        help="already provisioned GEFS image")
    parser.add_argument("--base", type=Path,
                        help="build a disposable image using this 9front VM")
    args = parser.parse_args()
    for path in (VM, KERNEL):
        if not path.is_file():
            parser.error(f"missing prerequisite: {path}")
    if args.base is None:
        image = args.image.resolve()
        if not image.is_file():
            parser.error(f"image does not exist: {image}")
        boot(image)
    else:
        base = args.base.resolve()
        if not base.is_file():
            parser.error(f"base image does not exist: {base}")
        with tempfile.TemporaryDirectory(prefix="aspera-drivers-", dir=ROOT / "build") as name:
            image = Path(name) / "root.img"
            provision(base, image)
            boot(image)
    print("ASPERA-DRIVER-IMAGE PASS mounted view and heartbeat renewal")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"test_driver_image: {error}", file=sys.stderr)
        raise SystemExit(1)
