#!/usr/bin/env python3
"""Boot the ARM64 Harvey entry under QEMU and check its serial handoff."""

from pathlib import Path
import os
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "build/arm64/harvey-virt.img"


def main() -> None:
    subprocess.run([sys.executable, str(Path(__file__).with_name("build_virt.py"))],
                   check=True)
    header = IMAGE.read_bytes()[:64]
    if len(header) != 64 or struct.unpack_from("<Q", header, 8)[0] != 0x80000 \
            or struct.unpack_from("<I", header, 56)[0] != 0x644D5241:
        raise RuntimeError("bad ARM64 Image header")

    for memory in (256, 512):
        command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57",
                   "-m", f"{memory}M", "-smp", "1", "-nographic", "-monitor", "none",
                   "-kernel", str(IMAGE)]
        guest = subprocess.Popen(command, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT)
        try:
            try:
                output, _ = guest.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                guest.terminate()
                output, _ = guest.communicate(timeout=3)
        finally:
            if guest.poll() is None:
                guest.kill()
                guest.wait()
        transcript = output.decode("utf-8", errors="replace")
        expected = ("HARVEY-ARM64 ENTRY", "dtb: valid at ",
                    f"bytes 0x{memory * 1024 * 1024:016x}",
                    "uart: 0x0000000009000000", "timer irq: 0x000000000000001b",
                    "HARVEY-ARM64 EARLY-BOOT PASS", "HARVEY-ARM64 TIMER-IRQ PASS")
        if any(marker not in transcript for marker in expected):
            raise RuntimeError(f"ARM64 boot failed with {memory} MiB:\n" + transcript)
        print(f"QEMU virt {memory} MiB: entry, DTB, timer IRQ PASS")

    fault_out = ROOT / "build/arm64-fault"
    env = os.environ.copy()
    env.update(ARM64_OUT=str(fault_out), ARM64_TEST_FAULT="1")
    subprocess.run([sys.executable, str(Path(__file__).with_name("build_virt.py"))],
                   env=env, check=True, stdout=subprocess.DEVNULL)
    command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57",
               "-m", "256M", "-smp", "1", "-nographic", "-monitor", "none",
               "-kernel", str(fault_out / "harvey-virt.img")]
    guest = subprocess.Popen(command, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
    try:
        try:
            output, _ = guest.communicate(timeout=1)
        except subprocess.TimeoutExpired:
            guest.terminate()
            output, _ = guest.communicate(timeout=3)
    finally:
        if guest.poll() is None:
            guest.kill()
            guest.wait()
    transcript = output.decode("utf-8", errors="replace")
    if "HARVEY-ARM64 TIMER-IRQ PASS" not in transcript or \
            transcript.count("ARM64 exception esr 0x00000000f2000000") != 1:
        raise RuntimeError("ARM64 exception vector failed:\n" + transcript)
    print("QEMU virt synchronous exception vector PASS")


if __name__ == "__main__":
    main()
