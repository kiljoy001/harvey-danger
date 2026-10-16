#!/usr/bin/env python3
"""Run the Objective-9C Aspera desktop inside 9vm with a thin SDL host window."""

import argparse
import fcntl
import os
from pathlib import Path
import subprocess
import time

from prism import REPO_ROOT, KERNEL_BIN, VM_BIN, compile_o9


ROOT = Path(REPO_ROOT)
SOURCE = ROOT / "sys/src/cmd/o9aspera/live.o9"
SHELF_SOURCE = ROOT / "sys/src/cmd/o9aspera/shelf_service.o9"
VIEWER = ROOT / "build/aspera_display_sdl"
VIEWER_SOURCE = ROOT / "util/9vm/aspera_display_sdl.c"
DISPLAY_CLIENT = ROOT / "util/9vm/vmdisplay_client.c"
DISPLAY_HEADER = ROOT / "util/9vm/vmdisplay_client.h"
DISPLAY_ABI = ROOT / "util/9vm/vmdisplay.h"
EXCHANGE = ROOT / "build/aspera-live"


def prepare_viewer() -> None:
    sources = (VIEWER_SOURCE, DISPLAY_CLIENT, DISPLAY_HEADER, DISPLAY_ABI)
    if VIEWER.exists() and all(VIEWER.stat().st_mtime >= source.stat().st_mtime
                               for source in sources):
        return
    flags = subprocess.check_output(["sdl2-config", "--cflags", "--libs"], text=True).split()
    subprocess.run(
        ["gcc", "-O2", "-Wall", "-Wextra", "-Werror", str(VIEWER_SOURCE),
         str(DISPLAY_CLIENT),
         "-o", str(VIEWER), *flags, "-lrt"],
        cwd=ROOT, check=True,
    )


def run(smoke: bool) -> int:
    EXCHANGE.mkdir(parents=True, exist_ok=True)
    lock_path = ROOT / "build/aspera-live.lock"
    with open(lock_path, "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        prepare_viewer()
        guest_binary = ROOT / f"build/aspera-boot-{os.getpid()}"
        log_path = ROOT / f"build/aspera-guest-{os.getpid()}.log"
        shm_name = f"/aspera-display-{os.getpid()}"
        try:
            guest = None
            try:
                device = EXCHANGE / "dev"
                device.mkdir(exist_ok=True)
                for name in ("ctl", "screen0", "screen1", "input"):
                    (device / name).touch()
                (EXCHANGE / "clock").write_text(time.strftime("%H:%M") + "\n")
                note_path = EXCHANGE / ("smoke-note.txt" if smoke else "note.txt")
                if smoke:
                    note_path.unlink(missing_ok=True)
                (EXCHANGE / "notepath").write_text(
                    f"build/aspera-live/{note_path.name}\n")
                compile_o9(str(SHELF_SOURCE), str(EXCHANGE / "shelf-service"))
                compile_o9(str(SOURCE), str(guest_binary))
                with open(log_path, "w+") as log:
                    guest = subprocess.Popen(
                        [VM_BIN, "-m", "1024", "-r", REPO_ROOT,
                         "-boot", str(guest_binary), "-display-shm", shm_name,
                         KERNEL_BIN],
                        cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                    )
                    command = [str(VIEWER), str(EXCHANGE), shm_name]
                    if smoke:
                        command.append("--smoke")
                    result = subprocess.run(command, cwd=ROOT, check=False)
                    try:
                        guest.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        guest.terminate()
                        guest.wait(timeout=5)
                    if smoke and result.returncode == 0:
                        if note_path.read_bytes() != b"Hi!":
                            raise RuntimeError("Aspera notes save/reopen round trip failed")
                        print("Aspera VM presentation and Notes save/reopen PASS")
                    if result.returncode != 0 or guest.returncode != 0:
                        log.seek(0)
                        print(log.read()[-2500:])
                        return result.returncode or guest.returncode or 1
                    return 0
            finally:
                if guest is not None and guest.poll() is None:
                    guest.terminate()
                    try:
                        guest.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        guest.kill()
                        guest.wait()
        finally:
            if smoke:
                (EXCHANGE / "smoke-note.txt").unlink(missing_ok=True)
            guest_binary.unlink(missing_ok=True)
            log_path.unlink(missing_ok=True)
            (Path("/dev/shm") / shm_name.lstrip("/")).unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smoke", action="store_true",
                        help="test guest frame publication and mouse input with SDL dummy")
    args = parser.parse_args()
    try:
        raise SystemExit(run(args.smoke))
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        print(f"Aspera VM error: {error}")
        raise SystemExit(1)
