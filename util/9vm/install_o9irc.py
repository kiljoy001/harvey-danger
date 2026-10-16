#!/usr/bin/env python3
"""Build o9irc and provision its binaries with native 9front GEFS."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from format_gefs_image import Console, SERIAL_INI, ninefat_offset

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PROGRAMS = {"o9irc-ircfs": "ircfs", "o9irc-gui": "gui", "o9irc-cli": "irc"}


def provision(base, image, stage, work):
    boot = work / "9front.img"
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(base),
                    str(boot)], check=True)
    ini = work / "plan9.ini"
    ini.write_text(SERIAL_INI)
    subprocess.run(["mcopy", "-o", "-i", f"{boot}@@{ninefat_offset(boot)}",
                    str(ini), "::plan9.ini"], check=True)
    command = [
        "qemu-system-i386", "-m", "1024", "-display", "none", "-monitor",
        "none", "-serial", "stdio", "-net", "none", "-no-reboot",
        "-drive", f"file={boot},format=raw,if=ide,index=0",
        "-drive", f"file={image},format=raw,if=ide,index=1",
        "-drive", f"file={stage},format=raw,if=ide,index=2",
    ]
    if os.access("/dev/kvm", os.R_OK | os.W_OK):
        command.insert(1, "-enable-kvm")
    with (work / "9front.log").open("w+") as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        console = Console(process, log)
        try:
            console.expect("init: starting /bin/rc", 180)
            console.expect("init: starting /bin/rc", 180)
            console.expect("term% ", 60)
            for line in (
                "dossrv -r -f /dev/sdD0/data o9ircstage",
                "mkdir -p /n/o9ircstage /n/o9ircgefs",
                "mount -c /srv/o9ircstage /n/o9ircstage",
                "gefs -c -f /dev/sdC1/data",
                "gefs -A -n o9ircgefs -f /dev/sdC1/data",
                "mount -c /srv/o9ircgefs /n/o9ircgefs",
                "mkdir -p /n/o9ircgefs/bin",
            ):
                result = console.run(line, 180)
                if line.startswith("gefs -c") and "\nok\n" not in result.replace("\r", ""):
                    raise RuntimeError("GEFS check failed: " + result[-600:])
            for source, target in PROGRAMS.items():
                dest = f"/n/o9ircgefs/bin/{source}"
                console.run(f"cp /n/o9ircstage/{target} {dest}", 180)
                console.run(f"chmod 755 {dest}")
                result = console.run(f"cmp /n/o9ircstage/{target} {dest}", 120)
                if "differ" in result or "cmp:" in result:
                    raise RuntimeError(f"{source} copy differs: {result[-600:]}")
                print(f"Installed {source}")
            console.run("echo sync >>/srv/o9ircgefs.cmd")
            console.run("sleep 3")
            console.run("echo halt >>/srv/o9ircgefs.cmd")
            console.run("sleep 2")
            console.buffer = ""
            process.stdin.write(b"fshalt\n")
            process.stdin.flush()
            console.expect("done halting", 120)
        finally:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--base", type=Path, required=True,
                        help="installed 9front formatter image")
    parser.add_argument("--source", type=Path, default=ROOT.parent / "o9irc")
    args = parser.parse_args()
    image = args.image.resolve()
    base = args.base.resolve()
    if not image.is_file() or not base.is_file():
        parser.error("GEFS image and 9front base image must exist")
    subprocess.run([sys.executable, str(HERE / "build_o9irc.py"),
                    "--source", str(args.source)], check=True)
    with tempfile.TemporaryDirectory(prefix="aspera-o9irc-", dir=ROOT / "build") as temp:
        work = Path(temp)
        stage = work / "stage.img"
        with stage.open("wb") as stream:
            stream.truncate(32 * 1024 * 1024)
        subprocess.run(["mkfs.vfat", "-F", "16", str(stage)], check=True,
                       stdout=subprocess.DEVNULL)
        for source, target in PROGRAMS.items():
            subprocess.run(["mcopy", "-o", "-i", str(stage),
                            str(ROOT / "build" / source), "::" + target],
                           check=True)
        provision(base, image, stage, work)
    print(f"o9irc installed in {image}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"install_o9irc: {error}", file=sys.stderr)
        raise SystemExit(1)
