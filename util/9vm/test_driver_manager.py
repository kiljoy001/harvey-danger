#!/usr/bin/env python3
"""Build and run the Objective-9C driver manager 9P integration test."""

from pathlib import Path
import os
import select
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
PRISM = ROOT / "util/9vm/prism.py"
SOURCE = ROOT / "sys/src/cmd/o9drvman/test_service.o9"
WORKLOAD = ROOT / "build/o9drvman-test-service"
CHILD_SOURCE = ROOT / "sys/src/cmd/o9drvman/test_driver.o9"
CHILD = ROOT / "build/o9drvman-test-child"
DRIVERFS_SOURCE = ROOT / "sys/src/cmd/o9driverfs/service.o9"
DRIVERFS = ROOT / "build/o9driverfs-managed"
LIFECYCLE = ROOT / "sys/src/cmd/o9drvman/lifecycle.c"
HEALTH = ROOT / "sys/src/cmd/o9drvman/health.c"
BINDING = ROOT / "sys/src/cmd/o9drvman/binding.c"
VM = ROOT / "util/9vm/9vm"
KERNEL = ROOT / "build/harvey.32bit"
MARKER = "O9DRVMAN-9P PASS"


def boot_until(command: list[str], marker: str) -> str:
    guest = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
    output = bytearray()
    deadline = time.monotonic() + 60
    try:
        while marker.encode() not in output and len(output) < 1024 * 1024:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select(
                    [guest.stdout], [], [], remaining)[0]:
                break
            chunk = os.read(guest.stdout.fileno(), 4096)
            if not chunk:
                break
            output.extend(chunk)
    finally:
        if guest.poll() is None:
            guest.terminate()
        try:
            guest.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            guest.kill()
            guest.communicate()
    return output.decode("utf-8", errors="replace")


def main() -> int:
    subprocess.run([
        sys.executable, str(PRISM), "build", str(CHILD_SOURCE),
        "-o", str(CHILD),
    ], cwd=ROOT, check=True)
    subprocess.run([
        sys.executable, str(PRISM), "build", str(DRIVERFS_SOURCE),
        "-o", str(DRIVERFS),
    ], cwd=ROOT, check=True)
    subprocess.run([
        sys.executable, str(PRISM), "build", str(SOURCE),
        "--c-source", str(LIFECYCLE),
        "--c-source", str(HEALTH),
        "--c-source", str(BINDING),
        "-o", str(WORKLOAD),
    ], cwd=ROOT, check=True)
    command = [str(VM), "-m", "512", "-r", str(ROOT),
               "-boot", str(WORKLOAD), str(KERNEL)]
    transcript = boot_until(command, MARKER)
    if MARKER not in transcript:
        print(transcript[-6000:], file=sys.stderr)
        print("test_driver_manager: integration test failed", file=sys.stderr)
        return 1
    forbidden = ("O9DRVMAN-9P FAIL", "panic:", "suicide:")
    found = [text for text in forbidden if text in transcript]
    if found:
        print(transcript[-6000:], file=sys.stderr)
        print(f"test_driver_manager: forbidden output {found}", file=sys.stderr)
        return 1
    print("Aspera Objective-9C driver manager 9P integration: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"test_driver_manager: {error}", file=sys.stderr)
        raise SystemExit(1)
