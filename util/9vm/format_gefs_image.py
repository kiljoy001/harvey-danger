#!/usr/bin/env python3
"""Create and verify an opaque GEFS image using an isolated 9front VM.

The installed 9front boot image is copied before use. The new image is attached
as the second IDE disk and formatted by 9front's own gefs command. Nothing in
the Linux host mounts or interprets the resulting filesystem.
"""

import argparse
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import tempfile
import time

SERIAL_INI = """bootfile=9pc
bootargs=local!/dev/sdC0/fs -A
nobootprompt=local!/dev/sdC0/fs -A
user=glenda
console=0 b115200
mouseport=ps2
"""
PROBE = "9vm-gefs-reboot-probe"
SERVICE = "9vmgefs"


def ninefat_offset(image):
    with image.open("rb") as stream:
        mbr = stream.read(512)
        if mbr[510:512] != b"\x55\xaa":
            raise RuntimeError("base image has no MBR")
        start = None
        for index in range(4):
            entry = mbr[446 + 16 * index:462 + 16 * index]
            if entry[4] == 0x39:
                start = int.from_bytes(entry[8:12], "little")
                break
        if start is None:
            raise RuntimeError("base image has no Plan 9 partition")
        stream.seek((start + 1) * 512)
        table = stream.read(512).split(b"\0")[0].decode("ascii", "replace")
    for line in table.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[:2] == ["part", "9fat"]:
            return (start + int(parts[2])) * 512
    raise RuntimeError("base image has no 9fat partition")


class Console:
    def __init__(self, process, log):
        self.process = process
        self.log = log
        self.buffer = ""

    def expect(self, marker, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            pos = self.buffer.find(marker)
            if pos >= 0:
                self.buffer = self.buffer[pos + len(marker):]
                return
            if self.process.poll() is not None:
                raise RuntimeError("9front VM exited before " + marker)
            ready, _, _ = select.select([self.process.stdout], [], [], 1)
            if ready:
                chunk = os.read(self.process.stdout.fileno(), 65536).decode("latin-1")
                self.log.write(chunk)
                self.log.flush()
                self.buffer += chunk
        raise RuntimeError("9front VM timed out before " + marker)

    def run(self, command, timeout=90):
        self.buffer = ""
        start = self.log.tell()
        self.process.stdin.write((command + "\n").encode())
        self.process.stdin.flush()
        self.expect("term% ", timeout)
        self.log.flush()
        self.log.seek(start)
        output = self.log.read()
        self.log.seek(0, os.SEEK_END)
        for error in ("gefs: create /srv/", "mount:", "can't open", "file does not exist"):
            if error in output:
                raise RuntimeError(f"{command}: {output[-500:]}")
        return output


def boot_and_check(boot_image, image, log_path, verify, check_file=None,
                   expected=None):
    command = [
        "qemu-system-i386", "-m", "1024", "-display", "none",
        "-monitor", "none", "-serial", "stdio", "-net", "none",
        "-no-reboot", "-drive", f"file={boot_image},format=raw,if=ide,index=0",
        "-drive", f"file={image},format=raw,if=ide,index=1",
    ]
    if os.access("/dev/kvm", os.R_OK | os.W_OK):
        command.insert(1, "-enable-kvm")
    with log_path.open("w+") as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        console = Console(process, log)
        try:
            console.expect("init: starting /bin/rc", 180)
            console.expect("init: starting /bin/rc", 180)
            console.expect("term% ", 60)
            if not verify:
                output = console.run("gefs -r glenda -f /dev/sdC1/data", 180)
                if "reaming /dev/sdC1/data" not in output:
                    raise RuntimeError("GEFS did not format the image")
            output = console.run("gefs -c -f /dev/sdC1/data", 180)
            if "\nok\n" not in output.replace("\r", ""):
                raise RuntimeError("GEFS check failed: " + output[-500:])
            console.run(f"gefs -A -n {SERVICE} -f /dev/sdC1/data", 120)
            console.run("mkdir -p /n/gefstest")
            console.run(f"mount -c /srv/{SERVICE} /n/gefstest")
            if not verify:
                console.run(f"echo {PROBE} > /n/gefstest/probe")
            path = check_file or "probe"
            value = expected or PROBE
            output = console.run(f"cat /n/gefstest/{path}")
            if f"\n{value}\n" not in output.replace("\r", ""):
                raise RuntimeError("GEFS read did not return the probe")
            if verify and check_file is None:
                console.run("rm /n/gefstest/probe")
            console.run(f"echo sync >>/srv/{SERVICE}.cmd")
            console.run("sleep 3")
            console.run(f"echo halt >>/srv/{SERVICE}.cmd")
            console.run("sleep 2")
            console.buffer = ""
            console.process.stdin.write(b"fshalt\n")
            console.process.stdin.flush()
            console.expect("done halting", 120)
        finally:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path,
                        help="installed 9front disk image, copied before use")
    parser.add_argument("image", type=Path, help="new GEFS image path")
    parser.add_argument("--size-mib", type=int, default=256)
    args = parser.parse_args()
    if args.size_mib < 129:
        parser.error("GEFS needs more than 128 MiB; use at least 129 MiB")
    if not args.base.is_file():
        parser.error("base image does not exist")
    for tool in ("qemu-system-i386", "mcopy", "cp"):
        if shutil.which(tool) is None:
            parser.error(tool + " is required")
    image = args.image.resolve()
    image.parent.mkdir(parents=True, exist_ok=True)
    try:
        fd = os.open(image, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    except FileExistsError:
        parser.error("image already exists; refusing to overwrite it")
    try:
        os.ftruncate(fd, args.size_mib * 1024 * 1024)
        os.close(fd)
        with tempfile.TemporaryDirectory(prefix="9vm-gefs-") as temporary:
            work = Path(temporary)
            boot_image = work / "9front.img"
            subprocess.run(["cp", "--reflink=auto", "--sparse=always",
                            str(args.base), str(boot_image)], check=True)
            ini = work / "plan9.ini"
            ini.write_text(SERIAL_INI)
            offset = ninefat_offset(boot_image)
            subprocess.run(["mcopy", "-o", "-i", f"{boot_image}@@{offset}",
                            str(ini), "::plan9.ini"], check=True)
            boot_and_check(boot_image, image, work / "format.log", False)
            boot_and_check(boot_image, image, work / "verify.log", True)
            with image.open("rb") as stream:
                os.fsync(stream.fileno())
        print(f"GEFS image ready: {image} ({args.size_mib} MiB); 9front reboot verified")
    except Exception:
        image.unlink(missing_ok=True)
        raise


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print("format_gefs_image:", error, file=sys.stderr)
        raise SystemExit(1)
