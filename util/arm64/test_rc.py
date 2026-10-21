#!/usr/bin/env python3
"""Exercise the ARM64 rc shell interactively under QEMU virt."""

from pathlib import Path
import pexpect
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "build/arm64-kernel/harvey.img"
LOG = ROOT / "build/arm64-rc-qemu.log"
COMMAND = (
    "qemu-system-aarch64 -machine virt -cpu cortex-a57 -m 512M -smp 1 "
    "-nographic -monitor none -kernel "
    + str(IMAGE)
)


def send_slow(child, line: str) -> None:
    for ch in line:
        child.send(ch)
        time.sleep(0.04)
    child.send("\r\n")


def main() -> None:
    subprocess.run([
        sys.executable, str(ROOT / "util/arm64/build_kernel.py"),
    ], check=True)

    log = LOG.open("w")
    child = pexpect.spawn(COMMAND, encoding="utf-8", timeout=60,
                          logfile=log, cwd=str(ROOT))
    try:
        child.expect("HARVEY-ARM64 RC START")
        child.expect("% ")
        send_slow(child, "echo hello-rc")
        child.expect("hello-rc")
        child.expect("% ")
        send_slow(child, "whatis cd")
        child.expect("builtin cd")
        child.expect("% ")
        send_slow(child, "exit")
        child.expect("% ")
        send_slow(child, "echo rc-restarted")
        child.expect("rc-restarted")
        child.expect("% ")
        print("ARM64 rc QEMU interactive PASS")
    except Exception:
        print(f"ARM64 rc QEMU interactive FAIL; log: {LOG}",
              file=sys.stderr)
        raise
    finally:
        child.close(force=True)
        log.close()


if __name__ == "__main__":
    main()
