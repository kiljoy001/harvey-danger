#!/usr/bin/env python3
"""Convert the linked Inferno kernel (Plan 9 a.out) to an ELF 9vm can load.

    ./mkelf.py [--kernel PATH] [--out-dir DIR]

Writes two files into the output directory:

  ipc.elf       for 9vm: the converted ELF, program headers intact
  ipc-qemu.elf  same image with the Multiboot AOUT_KLUDGE bit cleared

9vm's loader reads the ELF program headers, which aout2elf.py emits.  QEMU
honours the embedded Multiboot header instead, so with AOUT_KLUDGE still set
it would load the raw a.out layout; the second file exists for that case.

Defaults: kernel  <repo>/../purgatorio/os/pc/ipc  (override: $PURGATORIO)
          out-dir $NINEVM_WORK or /tmp/9vm-inferno
"""
import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kernel", type=Path, default=None,
                    help="purgatorio os/pc/ipc (default: sibling checkout)")
    ap.add_argument("--out-dir", type=Path,
                    default=Path(os.environ.get("NINEVM_WORK", "/tmp/9vm-inferno")))
    args = ap.parse_args()

    repo = HERE.parents[2]
    purg = Path(os.environ.get("PURGATORIO", str(repo.parent / "purgatorio")))
    kernel = args.kernel or (purg / "os/pc/ipc")
    if not kernel.is_file():
        sys.exit(f"kernel not found: {kernel} (build it in os/pc first)")
    args.out_dir.mkdir(parents=True, exist_ok=True)

    out = args.out_dir / "ipc.elf"
    qemu = args.out_dir / "ipc-qemu.elf"
    subprocess.check_call([sys.executable, str(HERE / "aout2elf.py"),
                           str(kernel), str(out)])

    blob = bytearray(out.read_bytes())
    off = blob.find(b"\x02\xb0\xad\x1b")
    if off < 0:
        sys.exit("no Multiboot header in converted ELF")
    magic, flags, _ = struct.unpack_from("<3I", blob, off)
    if not flags & 0x10000:
        sys.exit(f"AOUT_KLUDGE not set (flags=0x{flags:x})")
    newflags = flags & ~0x10000
    struct.pack_into("<II", blob, off + 4, newflags,
                     (-(magic + newflags)) & 0xFFFFFFFF)
    qemu.write_bytes(blob)
    print(f"{out} (9vm), {qemu} (QEMU; multiboot flags "
          f"0x{flags:x} -> 0x{newflags:x})")


if __name__ == "__main__":
    main()
