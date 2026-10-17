#!/usr/bin/env python3
"""Boot the ARM64 Harvey entry under QEMU and check its serial handoff."""

from pathlib import Path
import os
import select
import struct
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "build/arm64/harvey-virt.img"


def boot_until(command: list[str], marker: str) -> str:
    guest = subprocess.Popen(command, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
    output = bytearray()
    deadline = time.monotonic() + 8
    try:
        while marker.encode() not in output and len(output) < 1024 * 1024:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([guest.stdout], [], [], remaining)[0]:
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


def main() -> None:
    subprocess.run([sys.executable, str(Path(__file__).with_name("build_virt.py"))],
                   check=True)
    header = IMAGE.read_bytes()[:64]
    if len(header) != 64 or struct.unpack_from("<Q", header, 8)[0] != 0x80000 \
            or struct.unpack_from("<I", header, 56)[0] != 0x644D5241:
        raise RuntimeError("bad ARM64 Image header")

    for cpu, memory in (("cortex-a57", 256), ("cortex-a57", 512),
                        ("cortex-a57", 1536), ("cortex-a35", 512)):
        command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", cpu,
                   "-m", f"{memory}M", "-smp", "1", "-nographic", "-monitor", "none",
                   "-kernel", str(IMAGE)]
        transcript = boot_until(command, "HARVEY-ARM64 EL0-TIMER-IRQ PASS")
        expected = ("HARVEY-ARM64 ENTRY", "dtb: valid at ",
                    f"bytes 0x{memory * 1024 * 1024:016x}",
                    "uart: 0x0000000009000000", "timer irq: 0x000000000000001b",
                    "HARVEY-ARM64 EARLY-BOOT PASS", "HARVEY-ARM64 PHYS-PAGE PASS",
                    "HARVEY-ARM64 MMU-ON PASS",
                    "HARVEY-ARM64 TIMER-IRQ PASS", "HARVEY-ARM64 EL0-SVC PASS",
                    "HARVEY-ARM64 IRQ-MASK PASS",
                    "HARVEY-ARM64 EL0-ISOLATION PASS",
                    "HARVEY-ARM64 EL0-PERMISSIONS PASS",
                    "HARVEY-ARM64 ADDRESS-SPACE SWITCH PASS",
                    "HARVEY-ARM64 CONTEXT-SWITCH PASS",
                    "HARVEY-ARM64 EL0-TIMER-IRQ PASS")
        if any(marker not in transcript for marker in expected) or \
                "ARM64 exception" in transcript or "failed" in transcript:
            raise RuntimeError(f"ARM64 boot failed with {cpu}, {memory} MiB:\n" + transcript)
        print(f"QEMU virt {cpu}, {memory} MiB: MMU, EL0, IRQ mask, context switch PASS")

    fault_out = ROOT / "build/arm64-fault"
    env = os.environ.copy()
    env.update(ARM64_OUT=str(fault_out), ARM64_TEST_FAULT="1",
               ARM64_TEST_RESERVE="1")
    subprocess.run([sys.executable, str(Path(__file__).with_name("build_virt.py"))],
                   env=env, check=True, stdout=subprocess.DEVNULL)
    command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57",
               "-m", "256M", "-smp", "1", "-nographic", "-monitor", "none",
               "-kernel", str(fault_out / "harvey-virt.img")]
    transcript = boot_until(command, "ARM64 exception esr 0x00000000f2000000")
    if "HARVEY-ARM64 RESERVED-PAGE PASS" not in transcript or \
            "HARVEY-ARM64 MMU-ON PASS" not in transcript or \
            "HARVEY-ARM64 TIMER-IRQ PASS" not in transcript or \
            "HARVEY-ARM64 IRQ-MASK PASS" not in transcript or \
            "HARVEY-ARM64 CONTEXT-SWITCH PASS" not in transcript or \
            "HARVEY-ARM64 EL0-TIMER-IRQ PASS" not in transcript or \
            transcript.count("ARM64 exception esr 0x00000000f2000000") != 1:
        raise RuntimeError("ARM64 exception vector failed:\n" + transcript)
    print("QEMU virt reserved page and synchronous exception vector PASS")


if __name__ == "__main__":
    main()
