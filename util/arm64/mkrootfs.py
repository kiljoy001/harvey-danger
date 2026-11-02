#!/usr/bin/env python3
"""Build the eMMC root filesystem image from the ARM64 boot archive.

The root is mounted read-only at boot, so every directory that init or a
device server binds onto has to exist in the image: it cannot create
mount points at run time.

    python3 util/arm64/mkrootfs.py --archive build/arm64-kernel/userland.img \
            --output build/arm64-kernel/rootfs.ext2 [--size 128M]
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Mount points and standard directories.  /boot is aliased to /bin at
# re-root time; the rest are bind targets for kernel devices.
DIRS = ("bin", "boot", "dev", "env", "fd", "mnt", "n", "net", "proc",
        "root", "srv", "tmp")
MODES = {"tmp": "040777"}
NONEXEC = ("rcmain", "wcnmodem.bin")
SKIP = ("datameta.bin",)


def archive_files(path: Path) -> dict[str, bytes]:
    data = path.read_bytes()
    magic, version, count, total, _ = struct.unpack("<8sIIII", data[:24])
    if magic != b"O9BOOT01":
        raise SystemExit(f"{path}: not an O9 boot archive")
    if total != len(data):
        raise SystemExit(f"{path}: length {len(data)} != header {total}")
    files = {}
    off = 24
    for _ in range(count):
        name, offset, size, mode, _res = struct.unpack("<32sIIII",
                                                       data[off:off + 48])
        off += 48
        name = name.split(b"\0")[0].decode()
        files[name] = data[offset:offset + size]
    return files


def build(archive: Path, output: Path, size: str, label: str) -> None:
    files = archive_files(archive)
    if output.exists():
        output.unlink()
    subprocess.run(["truncate", "-s", size, str(output)], check=True)
    # dir_index (htree) and resize_inode are not understood by ext2srv.
    subprocess.run(["mke2fs", "-q", "-t", "ext2", "-b", "1024", "-I", "128",
                    "-O", "^dir_index,^resize_inode,^ext_attr",
                    "-L", label, str(output)],
                   check=True, stderr=subprocess.DEVNULL)

    with tempfile.TemporaryDirectory() as tmp:
        cmds = []
        for d in DIRS:
            cmds.append(f"mkdir /{d}")
            if d in MODES:
                cmds.append(f"sif /{d} mode {MODES[d]}")
        for name, blob in sorted(files.items()):
            if name in SKIP:
                continue
            if "/" in name:
                raise SystemExit(f"archive entry with a path: {name}")
            src = Path(tmp) / name
            src.write_bytes(blob)
            cmds.append(f"write {src} /bin/{name}")
            mode = "0100644" if name in NONEXEC else "0100755"
            cmds.append(f"sif /bin/{name} mode {mode}")
        script = Path(tmp) / "cmds"
        script.write_text("\n".join(cmds) + "\n")
        subprocess.run(["debugfs", "-w", "-f", str(script), str(output)],
                       check=True, capture_output=True)

    st = subprocess.run(["dumpe2fs", "-h", str(output)], capture_output=True,
                        text=True)
    for line in st.stdout.splitlines():
        if line.startswith(("Filesystem state", "Block count",
                            "Inode count", "Block size")):
            print(line)
    print(f"{output}: {output.stat().st_size // (1024 * 1024)} MiB, "
          f"{len(files)} programs in /bin")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--archive",
                    default=str(ROOT / "build/arm64-kernel/userland.img"))
    ap.add_argument("--output",
                    default=str(ROOT / "build/arm64-kernel/rootfs.ext2"))
    ap.add_argument("--size", default="128M")
    ap.add_argument("--label", default="harvey-root")
    a = ap.parse_args(argv)
    build(Path(a.archive), Path(a.output), a.size, a.label)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
