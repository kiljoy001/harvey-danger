#!/usr/bin/env python3
"""Boot the ARM64 Harvey entry under QEMU and check its serial handoff."""

from pathlib import Path
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

    command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57",
               "-m", "512M", "-smp", "1", "-nographic", "-monitor", "none",
               "-kernel", str(IMAGE)]
    guest = subprocess.Popen(command, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
    try:
        try:
            output, _ = guest.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            guest.terminate()
            output, _ = guest.communicate(timeout=3)
    finally:
        if guest.poll() is None:
            guest.kill()
            guest.wait()
    transcript = output.decode("utf-8", errors="replace")
    expected = ("HARVEY-ARM64 ENTRY", "dtb: valid at ",
                "HARVEY-ARM64 EARLY-BOOT PASS")
    if any(marker not in transcript for marker in expected):
        raise RuntimeError("ARM64 boot failed:\n" + transcript)
    print(transcript, end="")


if __name__ == "__main__":
    main()
