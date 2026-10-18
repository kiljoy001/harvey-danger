#!/usr/bin/env python3
"""Exercise the linked ARM64 kernel's virtio-MMIO 9P transport in QEMU."""

from pathlib import Path
import subprocess
import sys

from test_virt import boot_until


ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "build/arm64-kernel/harvey.img"


def main() -> None:
    subprocess.run([sys.executable, str(Path(__file__).with_name("build_kernel.py"))],
                   check=True)
    for modern in (False, True):
        command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57",
                   "-m", "512M", "-smp", "1", "-nographic", "-monitor", "none"]
        if modern:
            command += ["-global", "virtio-mmio.force-legacy=false"]
        command += ["-fsdev", f"local,id=fs0,path={ROOT / 'util/arm64'},"
                    "security_model=none,readonly=on",
                    "-device", "virtio-9p-device,fsdev=fs0,mount_tag=host9p",
                    "-kernel", str(IMAGE)]
        transcript = boot_until(command, "HARVEY-ARM64 VIRTIO-9P PASS")
        version = 2 if modern else 1
        expected = (f'#9/0: virtio-mmio v{version} 9p tag "host9p"',
                    "HARVEY-ARM64 9RING PASS",
                    "HARVEY-ARM64 VIRTIO-9P FRAMING PASS",
                    "HARVEY-ARM64 VIRTIO-9P PASS")
        if any(marker not in transcript for marker in expected) or \
                "ARM64 exception" in transcript or "failed" in transcript:
            raise RuntimeError(f"virtio-MMIO v{version} failed:\n{transcript}")
        print(f"ARM64 QEMU virt: 9ring and virtio-MMIO v{version} 9P exchange PASS")


if __name__ == "__main__":
    main()
