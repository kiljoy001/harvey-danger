#!/usr/bin/env python3
"""Boot the APEX aarch64 startup as the ARM64 kernel's /boot/init.

build_userland.py normally embeds the full ARM64 userland.  For this test
the generated header is reused with one substitution: the init image
becomes build/arm64-apex/hello.elf, so the first EL0 program the kernel
execs is the APEX startup (crt0 + __libc_start_main + variant-I TLS).

    python3 util/arm64/test_apex_boot.py

The original userland header is restored afterwards.
"""
from pathlib import Path
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/arm64-kernel"
APEX = ROOT / "build/arm64-apex/hello.elf"
MARKER = "TLS-OK"


def emit_array(image: bytes) -> str:
    lines = [f"static const u8 arm64initelf[{len(image)}] = {{"]
    for offset in range(0, len(image), 16):
        chunk = image[offset:offset + 16]
        lines.append("\t" + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--elf", help="program to run as init (default: the "
                                  "APEX hello built by build_apex.py)")
    ap.add_argument("--dump", help="write the whole transcript here")
    ap.add_argument("--allow-missing", action="store_true",
                    help="do not fail when the marker never appears; run "
                         "until the guest exits")
    ap.add_argument("--marker", default=MARKER,
                    help="string that must appear in the boot transcript")
    ap.add_argument("--archive",
                    help="file to load as the boot archive at 0x50000000 "
                         "(the kernel exposes it under /boot)")
    ap.add_argument("--deadline", type=float, default=0,
                    help="overall wall-clock limit in seconds (default: "
                         "none, but a panic/reboot loop never goes idle)")
    ap.add_argument("--timeout", type=float, default=0,
                    help="seconds of silence to allow before stopping "
                         "(default: 8, or an hour when an archive is used)")
    ap.add_argument("--gdb", action="store_true",
                    help="start QEMU's gdb stub on localhost:1234 so a hang "
                         "can be inspected with gdb")
    ap.add_argument("--smp", type=int, default=1,
                    help="number of QEMU vCPUs (the kernel needs SMP for >1)")
    ap.add_argument("--mem", type=int, default=0,
                    help="guest RAM in MiB (default: 512, or enough to "
                         "hold the boot archive)")
    ap.add_argument("--image-out",
                    help="build the kernel with this ELF as init, write the "
                         "Image here, and do not boot QEMU")
    args = ap.parse_args()

    if args.elf:
        elf = Path(args.elf)
    else:
        subprocess.run([sys.executable, str(HERE / "build_apex.py")],
                       check=True)
        elf = APEX
    image = elf.read_bytes()

    OUT.mkdir(parents=True, exist_ok=True)
    header = OUT / "userland.h"
    if not header.is_file():
        subprocess.run([
            sys.executable, str(HERE / "build_userland.py"),
            "--output", str(header), "--archive", str(OUT / "userland.img"),
        ], check=True)

    # A previous run that was killed mid-boot leaves the substituted image
    # in place.  Keep a pristine copy so the next run restores it instead of
    # treating the substitution as the original.
    backup = header.with_suffix(".h.orig")
    if backup.is_file():
        header.write_text(backup.read_text())
    original = header.read_text()
    backup.write_text(original)
    # build_userland.py has emitted the image both as a {0x..} array and as
    # a concatenated C string literal; match either declaration.
    text, count = re.subn(
        r"static const u8 arm64initelf\[\d+\] =.*?\n\t;",
        emit_array(image), original, flags=re.S)
    if count != 1:
        sys.exit("could not replace arm64initelf in the generated userland")
    header.write_text(text)

    # With ARM64_REUSE_USERLAND=1 build_kernel cannot see that the header
    # changed, so drop the object that includes it.
    (OUT / "sys_src_9_aarch64_boot.c.o").unlink(missing_ok=True)
    transcript = ""
    try:
        env = dict(os.environ, ARM64_REUSE_USERLAND="1")
        subprocess.run([sys.executable, str(HERE / "build_kernel.py")],
                       check=True, env=env)

        if args.image_out:
            shutil.copyfile(OUT / "harvey.img", args.image_out)
            print(f"ARM64 board image: {args.image_out}")
            return

        mem = args.mem
        if mem == 0 and args.archive:
            # The archive sits at 0x50000000 and grows upwards; the virt
            # RAM base is 0x40000000, so the guest needs 256 MiB plus the
            # archive itself.
            size = Path(args.archive).stat().st_size
            mem = 256 + (size >> 20) + 256
            mem = ((mem + 255) // 256) * 256
        if mem == 0:
            mem = 512
        command = ["qemu-system-aarch64", "-machine", "virt",
                   "-cpu", "cortex-a57", "-m", f"{mem}M", "-smp",
                   str(args.smp),
                   "-nographic", "-monitor", "none",
                   "-kernel", str(OUT / "harvey.img")]
        if args.archive:
            command += ["-device",
                        f"loader,file={args.archive},addr=0x50000000"]
        if args.gdb:
            command.append("-s")
        sys.path.insert(0, str(HERE))
        from test_virt import boot_until
        idle = args.timeout or (3600 if args.archive else 8)
        log = open(args.dump, "wb") if args.dump else None
        try:
            transcript = boot_until(command, args.marker, idle=idle,
                                    limit=8 * 1024 * 1024,
                                    total=args.deadline, log=log)
        finally:
            if log is not None:
                log.close()
    finally:
        if args.dump:
            Path(args.dump).write_text(transcript)
        header.write_text(original)
        backup.unlink(missing_ok=True)
        # Make sure the next normal build recompiles the object that
        # embedded our init image.
        (OUT / "sys_src_9_aarch64_boot.c.o").unlink(missing_ok=True)

    lines = [line for line in transcript.splitlines() if line.strip()]
    if args.marker in transcript:
        print("\n".join(lines)[-1500:])
    else:
        # Show both ends: the beginning says how far the program got, the
        # end says how it died.
        print("--- first lines ---")
        print("\n".join(lines[:40]))
        print("--- last lines ---")
        print("\n".join(lines[-15:]))
    if args.marker not in transcript and not args.allow_missing:
        raise SystemExit(f"{elf.name} did not reach {args.marker!r}")
    print(f"ARM64 APEX init: {elf.name} ran under QEMU virt")


if __name__ == "__main__":
    main()
