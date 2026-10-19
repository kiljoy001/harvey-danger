#!/usr/bin/env python3
"""Build and run the Objective-9C driver manager 9P integration test."""

from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
PRISM = ROOT / "util/9vm/prism.py"
SOURCE = ROOT / "sys/src/cmd/o9drvman/test_service.o9"
CHILD_SOURCE = ROOT / "sys/src/cmd/o9drvman/test_driver.o9"
CHILD = ROOT / "build/o9drvman-test-child"
DRIVERFS_SOURCE = ROOT / "sys/src/cmd/o9driverfs/service.o9"
DRIVERFS = ROOT / "build/o9driverfs-managed"
LIFECYCLE = ROOT / "sys/src/cmd/o9drvman/lifecycle.c"
BINDING = ROOT / "sys/src/cmd/o9drvman/binding.c"
MARKER = "O9DRVMAN-9P PASS"


def main() -> int:
    subprocess.run([
        sys.executable, str(PRISM), "build", str(CHILD_SOURCE),
        "-o", str(CHILD),
    ], cwd=ROOT, check=True)
    subprocess.run([
        sys.executable, str(PRISM), "build", str(DRIVERFS_SOURCE),
        "-o", str(DRIVERFS),
    ], cwd=ROOT, check=True)
    command = [
        sys.executable, str(PRISM), "run", str(SOURCE),
        "--c-source", str(LIFECYCLE),
        "--c-source", str(BINDING),
        "-m", "512", "-t", "35",
    ]
    result = subprocess.run(
        command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=45,
    )
    if result.returncode != 0 or MARKER not in result.stdout:
        print(result.stdout[-6000:], file=sys.stderr)
        print("test_driver_manager: integration test failed", file=sys.stderr)
        return 1
    forbidden = ("O9DRVMAN-9P FAIL", "panic:", "suicide:")
    found = [text for text in forbidden if text in result.stdout]
    if found:
        print(result.stdout[-6000:], file=sys.stderr)
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
