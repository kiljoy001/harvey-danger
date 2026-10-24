#!/usr/bin/env python3
"""Install the Objective-9C desktop and shelf into an existing GEFS image."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from format_gefs_image import Console, SERIAL_INI, ninefat_offset
from prism import compile_o9
from build_aspera_tools import main as build_tools


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PROGRAMS = {
    "desktop": (ROOT / "sys/src/cmd/o9aspera/live.o9", "/bin/aspera-live", ()),
    "shelf": (ROOT / "sys/src/cmd/o9aspera/shelf_service.o9",
              "/bin/aspera-shelf-service", ()),
    "driver-manager": (
        ROOT / "sys/src/cmd/o9drvman/service.o9", "/bin/o9drvman",
        (ROOT / "sys/src/cmd/o9drvman/lifecycle.c",
         ROOT / "sys/src/cmd/o9drvman/health.c",
         ROOT / "sys/src/cmd/o9drvman/binding.c"),
    ),
    "driver-fs": (ROOT / "sys/src/cmd/o9driverfs/service.o9",
                  "/bin/o9driverfs", ()),
}
TOOLS = {name: f"/bin/{name}" for name in ("rc", "ls", "cat", "test", "echo")}
TOOLS["rcmain"] = "/rc/lib/rcmain"
MIN_IMAGE_SIZE = 512 * 1024 * 1024


def provision(base: Path, image: Path, stage: Path, work: Path) -> None:
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
                "dossrv -r -f /dev/sdD0/data asperastage",
                "mkdir -p /n/asperastage /n/asperagefs",
                "mount -c /srv/asperastage /n/asperastage",
                "gefs -c -f /dev/sdC1/data",
                "gefs -A -n asperagefs -f /dev/sdC1/data",
                "mount -c /srv/asperagefs /n/asperagefs",
                "mkdir -p /n/asperagefs/bin",
                "mkdir -p /n/asperagefs/rc/lib",
            ):
                output = console.run(line, 180)
                if line.startswith("gefs -c") and "\nok\n" not in output.replace("\r", ""):
                    raise RuntimeError("GEFS check failed: " + output[-600:])
            for staged, (_, installed, _) in PROGRAMS.items():
                target = f"/n/asperagefs{installed}"
                console.run(f"cp /n/asperastage/{staged} {target}", 180)
                console.run(f"chmod 755 {target}")
                output = console.run(f"cmp /n/asperastage/{staged} {target}", 120)
                if "differ" in output or "cmp:" in output:
                    raise RuntimeError(f"{installed} copy differs: {output[-600:]}")
                print(f"Installed {installed}")
            for staged, installed in (("netd", "/bin/netd"),):
                target = f"/n/asperagefs{installed}"
                console.run(f"cp /n/asperastage/{staged} {target}", 180)
                console.run(f"chmod 755 {target}")
                output = console.run(f"cmp /n/asperastage/{staged} {target}", 120)
                if "differ" in output or "cmp:" in output:
                    raise RuntimeError(f"{installed} copy differs: {output[-600:]}")
                print(f"Installed {installed}")
            for staged, installed in TOOLS.items():
                target = f"/n/asperagefs{installed}"
                console.run(f"cp /n/asperastage/{staged} {target}", 180)
                if staged != "rcmain":
                    console.run(f"chmod 755 {target}")
                output = console.run(f"cmp /n/asperastage/{staged} {target}", 120)
                if "differ" in output or "cmp:" in output:
                    raise RuntimeError(f"{installed} copy differs: {output[-600:]}")
                print(f"Installed {installed}")
            console.run("echo sync >>/srv/asperagefs.cmd")
            console.run("sleep 3")
            console.run("echo halt >>/srv/asperagefs.cmd")
            console.run("sleep 2")
            console.buffer = ""
            process.stdin.write(b"fshalt\n")
            process.stdin.flush()
            console.expect("done halting", 120)
        finally:
            process.kill()
            process.wait()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True,
                        help="installed 9front VM image for native GEFS writes")
    parser.add_argument("--image", type=Path, required=True,
                        help="existing GEFS image to update")
    parser.add_argument("--front-source", type=Path,
                        default=ROOT.parent / "plan9front",
                        help="9front source checkout containing rc/lib/rcmain")
    args = parser.parse_args()
    base, image = args.base.resolve(), args.image.resolve()
    if not base.is_file() or not image.is_file():
        parser.error("9front base image and GEFS image must exist")
    if image.stat().st_size < MIN_IMAGE_SIZE:
        parser.error("GEFS image is too small for Aspera; use at least 512 MiB")
    rcmain = args.front_source.resolve() / "rc/lib/rcmain"
    if not rcmain.is_file():
        parser.error("9front rc/lib/rcmain not found")
    for staged, (source, _, extras) in PROGRAMS.items():
        compile_o9(str(source), str(ROOT / "build" / f"aspera-{staged}"),
                   extra_sources=tuple(map(str, extras)))
    build_tools()
    subprocess.run([sys.executable, str(ROOT / "util/9vm/build_netd.py")],
                   check=True)
    with tempfile.TemporaryDirectory(prefix="aspera-install-", dir=ROOT / "build") as tmp:
        work = Path(tmp)
        stage = work / "stage.img"
        with stage.open("wb") as stream:
            stream.truncate(32 * 1024 * 1024)
        subprocess.run(["mkfs.vfat", "-F", "16", str(stage)], check=True,
                       stdout=subprocess.DEVNULL)
        for staged in PROGRAMS:
            subprocess.run(["mcopy", "-o", "-i", str(stage),
                            str(ROOT / "build" / f"aspera-{staged}"),
                            "::" + staged], check=True)
        subprocess.run(["mcopy", "-o", "-i", str(stage),
                        str(ROOT / "build/netd"), "::netd"], check=True)
        for staged in TOOLS:
            source = rcmain if staged == "rcmain" else ROOT / "build" / f"aspera-{staged}"
            subprocess.run(["mcopy", "-o", "-i", str(stage),
                            str(source), "::" + staged], check=True)
        provision(base, image, stage, work)
    print(f"Aspera desktop installed in {image}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"install_aspera: {error}", file=sys.stderr)
        raise SystemExit(1)
