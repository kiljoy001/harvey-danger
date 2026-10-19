#!/usr/bin/env python3
"""Exercise the kernel FDT parser with QEMU's real DTB and malformed copies."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/arm64"
DTB = OUT / "qemu-virt.dtb"
BOARD = OUT / "test-board.dtb"
TEST = OUT / "test-fdt"


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["qemu-system-aarch64", "-machine", f"virt,dumpdtb={DTB}",
                    "-cpu", "cortex-a57", "-m", "512M", "-nographic"], check=True)
    subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(BOARD),
                    str(ROOT / "util/arm64/testdata/board.dts")], check=True)
    subprocess.run(["cc", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror",
                    "-D_XOPEN_SOURCE=700",
                    "-I", str(ROOT / "sys/src/9/aarch64"),
                    "-I", str(ROOT / "aarch64/include"),
                    str(Path(__file__).with_suffix(".c")),
                    str(ROOT / "sys/src/9/aarch64/fdt.c"), "-o", str(TEST)], check=True)
    subprocess.run([str(TEST), str(DTB)], check=True)
    subprocess.run([str(TEST), str(BOARD), "board"], check=True)


if __name__ == "__main__":
    main()
