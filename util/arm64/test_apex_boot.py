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

    original = header.read_text()
    text, count = re.subn(
        r"static const u8 arm64initelf\[\d+\] = \{.*?\n\};",
        emit_array(image), original, flags=re.S)
    if count != 1:
        sys.exit("could not replace arm64initelf in the generated userland")
    header.write_text(text)

    # With ARM64_REUSE_USERLAND=1 build_kernel cannot see that the header
    # changed, so drop the object that includes it.
    (OUT / "sys_src_9_aarch64_boot.c.o").unlink(missing_ok=True)
    try:
        env = dict(os.environ, ARM64_REUSE_USERLAND="1")
        subprocess.run([sys.executable, str(HERE / "build_kernel.py")],
                       check=True, env=env)

        command = ["qemu-system-aarch64", "-machine", "virt",
                   "-cpu", "cortex-a57", "-m", "512M", "-smp", "1",
                   "-nographic", "-monitor", "none",
                   "-kernel", str(OUT / "harvey.img")]
        sys.path.insert(0, str(HERE))
        from test_virt import boot_until
        transcript = boot_until(command, args.marker)
    finally:
        if args.dump:
            Path(args.dump).write_text(transcript)
        header.write_text(original)
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
