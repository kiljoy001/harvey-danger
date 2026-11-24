#!/usr/bin/env python3
"""Build a bootable SD-card image for one or more ARM64 boards.

The image holds an MBR and one FAT32 partition with the kernel Image, the
SDK boot archive, and a board DTB.  Repeat --dtb to put several boards in
one image under dtbs/; board.dtb is an alias for the default one and a
boot.cmd/boot.scr selects it with the harvey_dtb variable.

Before each DTB is written, /chosen/harvey,bootarchive is stamped with the
SDK load address and size, and --teeshm/--scm/--ram can stamp the other
ranges, so the kernel reserves them without board fallbacks.

Write it to the card from U-Boot with `mmc write` (split into 80 MiB
chunks for TFTP), or with dd from a workstation:

    python3 util/arm64/mk-sdimg.py \
        --image build/arm64-kernel/harvey.img \
        --dtb orangepi-3b=/home/scott/tftp/orangepi-3b-v1.1.dtb \
        --dtb rk3588-evb=/path/rk3588-evb.dtb \
        --sdk /tmp/opencode/sdk-board.boot \
        --out /tmp/opencode/sd.img

It prints the U-Boot commands for both the write and the boot.
"""
from pathlib import Path
import argparse
import atexit
import os
import re
import shutil
import subprocess
import sys
import tempfile

SECTOR = 512
PART_START = 2048          # 1 MiB alignment, no bootloader
UBOOT_PART_START = 0x8000  # 16 MiB, after the Rockchip bootloader area
UBOOT_OFFSET = 0x40        # idbloader starts here; the FIT follows at 0x4000
CHUNK = 83886080           # 80 MiB, the U-Boot TFTP part size


def parse_range(text: str) -> tuple[int, int]:
    try:
        base_text, size_text = text.split(":", 1)
        base = int(base_text, 0)
        size = int(size_text, 0)
    except ValueError:
        sys.exit(f"expected BASE:SIZE, got {text!r}")
    if base < 0 or size <= 0 or base + size > 1 << 64:
        sys.exit(f"bad range {text!r}")
    return base, size


def stamp_range(path: Path, prop: str, base: int, size: int) -> None:
    """Stamp a two-u64 /chosen property, creating /chosen if needed."""
    if subprocess.run(["fdtget", "-p", str(path), "/chosen"],
                      stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode != 0:
        subprocess.run(["fdtput", "-c", str(path), "/chosen"], check=True)
    subprocess.run([
        "fdtput", "-t", "u", str(path), "/chosen", prop,
        str((base >> 32) & 0xFFFFFFFF), str(base & 0xFFFFFFFF),
        str((size >> 32) & 0xFFFFFFFF), str(size & 0xFFFFFFFF),
    ], check=True)


def dtb_specs(specs: list[str]) -> list[tuple[str, Path]]:
    """Parse repeated [NAME=]PATH arguments into unique (name, path)."""
    out = []
    seen = set()
    for spec in specs:
        if "=" in spec:
            name, path = spec.split("=", 1)
        else:
            path = spec
            name = Path(path).stem
        name = re.sub(r"[^A-Za-z0-9._-]", "_", name).strip("._-")
        if not name:
            sys.exit(f"bad DTB name in {spec!r}")
        if name in seen:
            sys.exit(f"duplicate DTB name {name}")
        seen.add(name)
        out.append((name, Path(path)))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", default="build/arm64-kernel/harvey.img")
    ap.add_argument("--dtb", action="append", metavar="[NAME=]PATH",
                    help="board DTB; repeat for a multi-board image "
                         "(default: /home/scott/tftp/orangepi-3b-v1.1.dtb)")
    ap.add_argument("--default-dtb", default="",
                    help="DTB written as board.dtb and used by boot.cmd "
                         "(default: first --dtb)")
    ap.add_argument("--boot-script", action="store_true",
                    help="write boot.cmd/boot.scr even for one DTB")
    ap.add_argument("--sdk", default="/tmp/opencode/sdk-board.boot")
    ap.add_argument("--out", default="/tmp/opencode/sd.img")
    ap.add_argument("--uboot", default="",
                    help="u-boot-rockchip.bin to place at sector 0x40; the "
                         "FAT partition then starts at 16 MiB so the FIT at "
                         "sector 0x4000 is preserved")
    ap.add_argument("--size-mb", type=int, default=0,
                    help="image size (default: files + 10%)")
    ap.add_argument("--parts-dir", default="",
                    help="also split the image into 80 MiB TFTP parts here")
    ap.add_argument("--sdk-load", default="0x50000000",
                    help="U-Boot load address for sdk.tar")
    ap.add_argument("--teeshm", default="",
                    help="BASE:SIZE stamped as /chosen/harvey,teeshm")
    ap.add_argument("--scm", default="",
                    help="BASE:SIZE stamped as /chosen/harvey,scm")
    ap.add_argument("--ram", default="",
                    help="BASE:SIZE stamped as /chosen/harvey,ram fallback")
    ap.add_argument("--no-stamp", action="store_true",
                    help="do not add the harvey,* /chosen properties")
    args = ap.parse_args()

    dtbs = dtb_specs(args.dtb or ["/home/scott/tftp/orangepi-3b-v1.1.dtb"])
    for name, path in (("image", args.image), ("sdk", args.sdk)):
        if not Path(path).is_file():
            sys.exit(f"missing {name}: {path}")
    for name, path in dtbs:
        if not path.is_file():
            sys.exit(f"missing dtb {name}: {path}")
    for tool in ("sfdisk", "mkfs.vfat", "mcopy", "mtype", "mmd",
                 "fdtget", "fdtput"):
        if shutil.which(tool) is None:
            sys.exit(f"missing {tool}")

    image = Path(args.image)
    sdk = Path(args.sdk)
    sdk_load = int(args.sdk_load, 0)

    names = [name for name, _ in dtbs]
    default_name = args.default_dtb or names[0]
    if default_name not in names:
        sys.exit(f"--default-dtb {default_name} is not one of: " +
                 ", ".join(names))

    # Stamp the ranges into copies; never rewrite the caller's DTBs.
    stamped = []
    for name, dtb in dtbs:
        with tempfile.NamedTemporaryFile(suffix=".dtb", delete=False) as tf:
            work = Path(tf.name)
        shutil.copyfile(dtb, work)
        atexit.register(work.unlink, missing_ok=True)
        if not args.no_stamp:
            stamp_range(work, "harvey,bootarchive", sdk_load,
                        sdk.stat().st_size)
            if args.teeshm:
                base, size = parse_range(args.teeshm)
                stamp_range(work, "harvey,teeshm", base, size)
            if args.scm:
                base, size = parse_range(args.scm)
                stamp_range(work, "harvey,scm", base, size)
            if args.ram:
                base, size = parse_range(args.ram)
                stamp_range(work, "harvey,ram", base, size)
        stamped.append((name, work))

    need = (image.stat().st_size + sdk.stat().st_size +
            sum(work.stat().st_size for _, work in stamped))
    part_start = UBOOT_PART_START if args.uboot else PART_START
    size = (args.size_mb * 1024 * 1024) if args.size_mb else \
        need + need // 10
    size = max(size, part_start * SECTOR + need + 1024 * 1024)
    size = (size + 1024 * 1024 - 1) // (1024 * 1024) * 1024 * 1024
    out = Path(args.out)
    out.unlink(missing_ok=True)
    with open(out, "wb") as f:
        f.truncate(size)

    sectors = size // SECTOR
    part_sectors = sectors - part_start
    subprocess.run(["sfdisk", "-q", "--no-reread", str(out)], check=True,
                   input=f"label: dos\nstart={part_start}, "
                         f"size={part_sectors}, type=0c\n".encode())
    if args.uboot:
        ublob = Path(args.uboot).read_bytes()
        # The SPI SPL reads the FIT at sector 0x4000.  The image is written
        # at sector 0x40, so the FIT must sit at byte 0x7f8000.
        fit = ublob.find(b"\xd0\x0d\xfe\xed")
        if fit != 0x7F8000:
            sys.exit(f"u-boot image has its FIT at {fit:#x}, expected "
                     "0x7f8000 so the SPI SPL finds it at sector 0x4000")
        # A FIT without the TF-A loadables starts U-Boot but aborts in the
        # OP-TEE SMC (flush_shm_dcache) and resets the board.
        if b"atf-1" not in ublob[fit:fit + 0x8000]:
            sys.exit("u-boot FIT has no ATF loadables (atf-1); build it "
                     "with BL31 or the board resets in the OP-TEE SMC")
        end = UBOOT_OFFSET * SECTOR + len(ublob)
        if end > part_start * SECTOR:
            sys.exit("u-boot image overlaps the FAT partition")
        with open(out, "r+b") as f:
            f.seek(UBOOT_OFFSET * SECTOR)
            f.write(ublob)
    subprocess.run(["mkfs.vfat", "-F", "32", "-n", "HARVEY",
                    "--offset", str(part_start), str(out)], check=True,
                   stdout=subprocess.DEVNULL)

    offset = part_start * SECTOR
    part_image = f"{out}@@{offset}"
    subprocess.run(["mmd", "-i", part_image, "::dtbs"], check=True)

    subprocess.run(["mcopy", "-i", part_image, str(image), "::Image"],
                   check=True)
    subprocess.run(["mcopy", "-i", part_image, str(sdk), "::sdk.tar"],
                   check=True)
    default_work = dict(stamped)[default_name]
    subprocess.run(["mcopy", "-i", part_image, str(default_work),
                    "::board.dtb"], check=True)
    for name, work in stamped:
        subprocess.run(["mcopy", "-i", part_image, str(work),
                        f"::dtbs/{name}.dtb"], check=True)

    boot_script = args.boot_script or len(stamped) > 1
    boot_scr = None
    if boot_script:
        script = (
            "# Generated by mk-sdimg.py\n"
            "# Pick a board with: setenv harvey_dtb dtbs/<name>.dtb\n"
            'if test -z "${harvey_dtb}"; then\n'
            f"\tsetenv harvey_dtb dtbs/{default_name}.dtb\n"
            "fi\n"
            "fatload mmc 1:1 0x40080000 Image\n"
            "fatload mmc 1:1 0x48000000 ${harvey_dtb}\n"
            f"fatload mmc 1:1 0x{sdk_load:x} sdk.tar\n"
            "booti 0x40080000 - 0x48000000\n"
        )
        with tempfile.NamedTemporaryFile("w", suffix=".cmd",
                                         delete=False) as tf:
            tf.write(script)
            bootcmd = Path(tf.name)
        atexit.register(bootcmd.unlink, missing_ok=True)
        subprocess.run(["mcopy", "-i", part_image, str(bootcmd),
                        "::boot.cmd"], check=True)
        if shutil.which("mkimage") is not None:
            boot_scr = Path(str(bootcmd) + ".scr")
            subprocess.run(["mkimage", "-A", "arm64", "-T", "script",
                            "-C", "none", "-n", "harvey", "-d",
                            str(bootcmd), str(boot_scr)], check=True)
            atexit.register(boot_scr.unlink, missing_ok=True)
            subprocess.run(["mcopy", "-i", part_image, str(boot_scr),
                            "::boot.scr"], check=True)

    # Read each file back so a silent failure cannot pass.
    verify = ["Image", "board.dtb", "sdk.tar"]
    verify += [f"dtbs/{name}.dtb" for name in names]
    if boot_script:
        verify.append("boot.cmd")
    if boot_scr is not None:
        verify.append("boot.scr")
    for name in verify:
        if subprocess.run(["mtype", "-i", part_image, f"::{name}"],
                          stdout=subprocess.DEVNULL).returncode != 0:
            sys.exit(f"{name} missing from the image")

    blocks = size // SECTOR
    print(f"sd image: {out} ({size} bytes, {blocks} blocks)")
    print(f"  partition 1 at sector {part_start}, {part_sectors} sectors")
    print(f"  boards: {', '.join(names)} (default {default_name})")
    print()
    print("U-Boot write (after loading the parts at 0x50000000 + i*0x05000000):")
    print(f"  mmc dev 1")
    print(f"  mmc write 0x50000000 0 0x{blocks:X}")
    print()
    print("U-Boot boot:")
    print("  fatload mmc 1:1 0x40080000 Image")
    print(f"  fatload mmc 1:1 0x48000000 dtbs/{default_name}.dtb")
    print(f"  fatload mmc 1:1 0x{sdk_load:x} sdk.tar")
    print("  booti 0x40080000 - 0x48000000")
    print()
    print("  setenv bootcmd 'fatload mmc 1:1 0x40080000 Image; "
          f"fatload mmc 1:1 0x48000000 dtbs/{default_name}.dtb; "
          f"fatload mmc 1:1 0x{sdk_load:x} sdk.tar; "
          "booti 0x40080000 - 0x48000000'; saveenv")
    if len(names) > 1:
        print()
        print("Boards in this image (set harvey_dtb before booting):")
        for name in names:
            print(f"  setenv harvey_dtb dtbs/{name}.dtb")
        if boot_scr is not None:
            print()
            print("  boot.scr is in the image and loads sdk.tar for you.")
        else:
            print()
            print("  boot.cmd is in the image; install it with:")
            print("  mkimage -A arm64 -T script -C none -n harvey "
                  "-d boot.cmd boot.scr")

    if args.parts_dir:
        pdir = Path(args.parts_dir)
        shutil.rmtree(pdir, ignore_errors=True)
        pdir.mkdir(parents=True)
        n = 0
        with open(out, "rb") as f:
            while True:
                chunk = f.read(CHUNK)
                if not chunk:
                    break
                (pdir / f"sd.part.{n:02d}").write_bytes(chunk)
                n += 1
        print(f"\nsplit into {n} parts under {pdir}")


if __name__ == "__main__":
    main()
