#!/usr/bin/env python3
"""Publish a board boot set to the LAN TFTP server.

The installed vendor U-Boot on the Orange Pi 3B ignores ``tftpdstp`` and
always uses UDP port 69, so the host's :6969 server is not reachable from
it.  This script copies the boot image, DTB and an 80 MiB-split SDK
archive to the OpenWrt router (which runs a port-69 TFTP server) and
prints the U-Boot commands, including the 100 Mb/s PHY workaround.

    python3 util/arm64/push-tftp.py \
        --image /home/scott/tftp/aspera-tblgen.img \
        --sdk /tmp/opencode/sdk-board.boot \
        --dtb /home/scott/tftp/orangepi-3b-v1.1.dtb

The default destination is ``root@192.168.2.1:/overlay/aspera-serve``.
The router's /tmp is a small tmpfs, so the TFTP root must stay on
/overlay.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import sys
import tarfile
import tempfile

CHUNK = 83886080           # 80 MiB, the U-Boot part size
LOAD_BASE = 0x50000000
LOAD_STEP = 0x05000000


def split_sdk(sdk: Path, out: Path) -> list[Path]:
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("sdk.part.*"):
        old.unlink()
    parts = []
    with sdk.open("rb") as f:
        index = 0
        while True:
            data = f.read(CHUNK)
            if not data:
                break
            part = out / f"sdk.part.{index:02d}"
            part.write_bytes(data)
            parts.append(part)
            index += 1
    if not parts:
        sys.exit(f"{sdk} is empty")
    return parts


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", required=True, type=Path)
    ap.add_argument("--sdk", required=True, type=Path)
    ap.add_argument("--dtb", required=True, type=Path)
    ap.add_argument("--router", default="root@192.168.2.1")
    ap.add_argument("--root", default="/overlay/aspera-serve")
    ap.add_argument("--ssh", default="ssh -o BatchMode=yes")
    ap.add_argument("--keep-parts", action="store_true",
                    help="keep the split parts next to the SDK")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    for p in (args.image, args.sdk, args.dtb):
        if not p.is_file():
            sys.exit(f"missing {p}")

    if args.keep_parts:
        parts = split_sdk(args.sdk, args.sdk.parent / (args.sdk.name + ".parts"))
        tmp = None
    else:
        tmp = Path(tempfile.mkdtemp(prefix="sdkparts-", dir="/tmp/opencode"))
        parts = split_sdk(args.sdk, tmp)

    files = [args.image, args.dtb, *parts]
    total = sum(p.stat().st_size for p in files)
    print(f"{len(files)} files, {total/2**20:.0f} MiB -> "
          f"{args.router}:{args.root}")

    if args.dry_run:
        for p in files:
            print(f"  {p}")
        if tmp is not None:
            shutil.rmtree(tmp, ignore_errors=True)
        return

    ssh = args.ssh.split()
    subprocess.run([*ssh, args.router, f"mkdir -p {args.root}"], check=True)
    with tempfile.NamedTemporaryFile(suffix=".tar", delete=False) as tf:
        tarname = tf.name
    try:
        with tarfile.open(tarname, "w") as tar:
            for p in files:
                tar.add(p, arcname=p.name)
        with open(tarname, "rb") as tar:
            subprocess.run([*ssh, args.router,
                            f"tar xf - -C {args.root}"], stdin=tar,
                           check=True)
    finally:
        Path(tarname).unlink(missing_ok=True)
        if tmp is not None:
            shutil.rmtree(tmp, ignore_errors=True)

    listing = subprocess.run(
        [*ssh, args.router,
         f"cd {args.root} && ls -l {' '.join(p.name for p in files)}"],
        check=True, capture_output=True, text=True)
    print(listing.stdout.rstrip())

    print(f"""
U-Boot (vendor) sequence; the first ping starts the PHY and is interrupted
if it hangs, then the MDIO workaround restores 100 Mb/s transmission:

  setenv ipaddr 192.168.2.250; setenv netmask 255.255.255.0
  setenv serverip 192.168.2.1
  ping 192.168.2.1
  mii write 0 9 0; mii write 0 0 0x1200
  mii write 0 1e 0xa003; mii write 0 1f 0x40f1
  ping 192.168.2.1
  mw.l 0xfe60000c 0x76; mw.l 0xfe600000 0
  tftp 0x40080000 {args.image.name}
""" + "".join(
        f"  tftp 0x{LOAD_BASE + i*LOAD_STEP:08x} {p.name}\n"
        for i, p in enumerate(parts)) + f"""  tftp 0x48000000 {args.dtb.name}
  booti 0x40080000 - 0x48000000
""")


if __name__ == "__main__":
    main()
