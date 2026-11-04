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
    subprocess.run([sys.executable, str(HERE / "build_apex.py")], check=True)
    image = APEX.read_bytes()

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
        transcript = boot_until(command, MARKER)
    finally:
        header.write_text(original)
        # Make sure the next normal build recompiles the object that
        # embedded our init image.
        (OUT / "sys_src_9_aarch64_boot.c.o").unlink(missing_ok=True)

    tail = "\n".join(line for line in transcript.splitlines()
                     if line.strip())[-1500:]
    print(tail)
    if MARKER not in transcript:
        raise SystemExit("the APEX init did not run, or its TLS value was "
                         "wrong")
    if "apex-hello argc=" not in transcript or "tls=11" not in transcript:
        raise SystemExit("unexpected APEX init output")
    print("ARM64 APEX startup: /boot/init ran, printed through APEX and read "
          "its thread-local variable")


if __name__ == "__main__":
    main()
