#!/usr/bin/env python3
"""Link every ARM64 kernel source and boot the linked image in QEMU virt."""

from pathlib import Path
import subprocess
import sys

from test_virt import boot_until


ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "build/arm64-kernel/harvey.img"
ELF = ROOT / "build/arm64-kernel/harvey.elf"
MARKER = "HARVEY-ARM64 FULL-KERNEL-LINK PASS"


def main() -> None:
    subprocess.run([sys.executable, str(Path(__file__).with_name("build_kernel.py"))],
                   check=True)
    undefined = subprocess.check_output(["llvm-nm", "-u", str(ELF)], text=True)
    if undefined.strip():
        raise RuntimeError(f"kernel has undefined symbols:\n{undefined}")
    for memory in (256, 512):
        command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57",
                   "-m", f"{memory}M", "-smp", "1", "-nographic",
                   "-monitor", "none", "-kernel", str(IMAGE)]
        transcript = boot_until(command, MARKER)
        if MARKER not in transcript or "failed" in transcript or \
                "ARM64 exception" in transcript:
            raise RuntimeError(f"full kernel boot failed at {memory} MiB:\n{transcript}")
        print(f"ARM64 linked kernel, QEMU virt {memory} MiB: boot and allocator PASS")


if __name__ == "__main__":
    main()
