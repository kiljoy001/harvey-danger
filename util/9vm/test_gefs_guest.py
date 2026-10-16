#!/usr/bin/env python3
"""Cross-check GEFS writes in Harvey and 9front across fresh VM boots."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

from format_gefs_image import SERIAL_INI, boot_and_check, ninefat_offset

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
KERNEL = ROOT / "build/harvey.32bit"
VM = HERE / "9vm"
CONTENT = "Harvey GEFS persistent probe"


def guest_boot(image, workload, verify=False, ring=True):
    cmd = [str(VM), "-m", "512", "-r", str(ROOT), "-boot", str(workload),
           "-gefs-image", str(image)]
    if not ring:
        cmd.append("-no9ring")
    if verify:
        cmd.extend(["-c", "bootargs=local!#9/0 nobootprompt=local "
                    "O9_GEFS_VERIFY=1"])
    cmd.append(str(KERNEL))
    proc = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=30)
    output = proc.stdout
    for marker in ("GEFS-GUEST-CHECK PASS", "GEFS-GUEST-MOUNT PASS"):
        if marker not in output:
            raise RuntimeError(f"{marker} missing:\n{output[-3000:]}")
    for marker in ("GEFS-GUEST-CHECK FAIL", "GEFS-GUEST-MOUNT FAIL",
                   "assert failed:", "suicide:", "panic:"):
        if marker in output:
            raise RuntimeError(f"guest failed ({marker}):\n{output[-3000:]}")
    if proc.returncode != 0:
        raise RuntimeError(f"9vm exited {proc.returncode}:\n{output[-3000:]}")
    print("Harvey", "verify" if verify else "write", "PASS",
          "(ring)" if ring else "(VirtIO root)")


def ninefront_verify(base, image, temp):
    boot = temp / "9front.img"
    subprocess.run(["cp", "--reflink=auto", "--sparse=always",
                    str(base), str(boot)], check=True)
    ini = temp / "plan9.ini"
    ini.write_text(SERIAL_INI)
    offset = ninefat_offset(boot)
    subprocess.run(["mcopy", "-o", "-i", f"{boot}@@{offset}",
                    str(ini), "::plan9.ini"], check=True)
    boot_and_check(boot, image, temp / "9front-verify.log", True,
                   "gefs-probe", CONTENT)
    print("9front cross-read PASS")


def production_boot(image, mode):
    cmd = [str(VM), "-m", "512", "-r", "/", "-gefs-image", str(image),
           "-c", f"bootargs=local!#9/0 O9_GEFS_ROOT_TEST={mode}", str(KERNEL)]
    try:
        proc = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=10)
        output = proc.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b"").decode("utf-8", "replace")
    markers = ("[9vm] GEFS root mode: only the image file is exported",
               f"ASPERA-GEFS-ROOT-{mode.upper()} PASS",
               "ASPERA-GEFS-ROOT PASS")
    if (any(marker not in output for marker in markers) or
            any(marker in output for marker in ("init: mounting #~/host",
                "init: mounting #9/0", "In-process hostfs online serving",
                "panic:", "suicide:", "aspera: root "))):
        raise RuntimeError(f"Objective-9C GEFS root {mode} failed:\n" +
                           output[-3000:])
    print(f"Aspera GEFS root {mode} PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True,
                        help="installed 9front image with GEFS")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="9vm-gefs-guest-", dir=ROOT / "build") as path:
        temp = Path(path)
        image = temp / "gefs.img"
        workload = temp / "guest"
        subprocess.run([sys.executable, str(HERE / "format_gefs_image.py"),
                        "--base", str(args.base), str(image)], check=True)
        subprocess.run([sys.executable, str(HERE / "build_gefs_guest.py")],
                        cwd=ROOT, check=True)
        subprocess.run([sys.executable, str(HERE / "prism.py"), "build",
                        str(HERE / "test_gefs_guest.o9"), "-o", str(workload)],
                        cwd=ROOT, check=True)
        guest_boot(image, workload)
        guest_boot(image, workload, verify=True)
        guest_boot(image, workload, verify=True, ring=False)
        ninefront_verify(args.base, image, temp)
        production_boot(image, "write")
        production_boot(image, "verify")
    print("9VM-GEFS-GUEST PASS check, write, reboot, cross-OS read")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print("test_gefs_guest:", error, file=sys.stderr)
        raise SystemExit(1)
