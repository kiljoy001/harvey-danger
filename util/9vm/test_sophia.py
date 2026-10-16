#!/usr/bin/env python3
"""Exercise Objective-9C Sophia storage, 9P mounts, and a fresh VM boot."""

from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "sys/src/cmd/o9sophia"
IMAGE = ROOT / "build/sophia-reboot-test.disk"
PRISM = ROOT / "util/9vm/prism.py"


def guest(name: str, marker: str) -> None:
    result = subprocess.run(
        [sys.executable, str(PRISM), "run", str(SOURCE / name),
         "-m", "512", "-t", "60"],
        cwd=ROOT, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=75,
    )
    if result.returncode != 0 or marker not in result.stdout or \
            "panic:" in result.stdout or "suicide:" in result.stdout:
        raise RuntimeError(f"{name} failed:\n{result.stdout[-3000:]}")
    print(marker)


def main() -> int:
    (ROOT / "build").mkdir(exist_ok=True)
    IMAGE.unlink(missing_ok=True)
    try:
        guest("test_store.o9", "SOPHIA-STORE PASS")
        guest("test_fixed.o9", "SOPHIA-FIXED PASS")
        guest("test_service.o9", "SOPHIA-9P PASS")
        guest("test_persist_write.o9", "SOPHIA-BOOT-WRITE PASS")
        guest("test_persist_read.o9", "SOPHIA-BOOT-READ PASS")
    finally:
        IMAGE.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
