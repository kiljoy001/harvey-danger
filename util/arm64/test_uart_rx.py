#!/usr/bin/env python3
"""Send a byte through QEMU's serial input into the shared console queue."""

from pathlib import Path
import os
import select
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "build/arm64-kernel/harvey.img"


def until(guest: subprocess.Popen, marker: bytes, output: bytearray) -> None:
    deadline = time.monotonic() + 8
    while marker not in output:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([guest.stdout], [], [], remaining)[0]:
            raise RuntimeError(f"timed out waiting for {marker!r}:\n"
                               + output.decode(errors="replace"))
        chunk = os.read(guest.stdout.fileno(), 4096)
        if not chunk:
            raise RuntimeError("guest exited:\n" + output.decode(errors="replace"))
        output.extend(chunk)
        if b"ARM64 exception" in output or b"panic:" in output:
            raise RuntimeError("guest faulted:\n" + output.decode(errors="replace"))


def main() -> None:
    env = os.environ.copy()
    env["ARM64_TEST_CONSOLE_RX"] = "1"
    subprocess.run([sys.executable, str(Path(__file__).with_name("build_kernel.py"))],
                   cwd=ROOT, env=env, check=True)
    command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57",
               "-m", "256M", "-smp", "1", "-nographic", "-monitor", "none",
               "-kernel", str(IMAGE)]
    guest = subprocess.Popen(command, stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = bytearray()
    try:
        until(guest, b"HARVEY-ARM64 SERIAL-CONSOLE PASS", output)
        guest.stdin.write(b"Z")
        guest.stdin.flush()
        until(guest, b"HARVEY-ARM64 UART-RX PASS", output)
        print("ARM64 QEMU PL011 -> #t/eia0 -> console input queue PASS")
    finally:
        if guest.poll() is None:
            guest.terminate()
        try:
            guest.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            guest.kill()
            guest.communicate()


if __name__ == "__main__":
    main()
