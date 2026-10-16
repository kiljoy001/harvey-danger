#!/usr/bin/env python3
"""VM unit, generated property, mutation, and SDL round-trip checks for Aspera."""

import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "sys/src/cmd/o9aspera"
PRISM = ROOT / "util/9vm/prism.py"
LIVE = ROOT / "util/9vm/aspera_live.py"
DISPLAY_TEST = ROOT / "util/9vm/test_vmdisplay.c"
DISPLAY_CLIENT_TEST = ROOT / "util/9vm/test_vmdisplay_client.c"
DISPLAY_CLIENT = ROOT / "util/9vm/vmdisplay_client.c"
MUTANTS = (
    ("focus-raises-bottom", "desktop.o9",
     "if(targetIdx >= 0 && targetIdx < winList.length() - 1)",
     "if(targetIdx >= 1 && targetIdx < winList.length() - 1)"),
)
SHELF_TEST = SOURCE / "test_shelf_service.o9"
SHELF_MUTANTS = (
    ("shelf-workspace-edge", "((x - 92) / 36) + 1", "((x - 92) / 37) + 1"),
    ("shelf-clock-bound", "hour >= 0 && hour <= 23", "hour >= 0 && hour <= 24"),
)


def run_guest(path: Path) -> str:
    command = [sys.executable, str(PRISM), "run", str(path), "-m", "512", "-t", "35"]
    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True,
                               start_new_session=True)
    try:
        output, _ = process.communicate(timeout=45)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise RuntimeError("Aspera VM test timed out")
    if process.returncode != 0 or "panic:" in output or "suicide:" in output:
        raise RuntimeError(f"Aspera VM failed:\n{output[-2000:]}")
    return output


def mutant_source(work: Path, filename: str, old: str, new: str) -> Path:
    for name in ("test_live.o9", "canvas.o9", "theme.o9", "window.o9",
                 "desktop.o9", "shelf.o9", "note.o9", "font8x14.h"):
        shutil.copyfile(SOURCE / name, work / name)
    (work / "stdlib").symlink_to(ROOT / "stdlib")
    for name in ("bytes.o9", "string.o9", "file.o9", "io.o9", "time.o9"):
        (work / name).symlink_to(ROOT / "stdlib" / name)
    target = work / filename
    source = target.read_text()
    if source.count(old) != 1:
        raise RuntimeError(f"mutation site in {filename} changed")
    target.write_text(source.replace(old, new, 1))
    return work / "test_live.o9"


def shelf_mutant_source(work: Path, old: str, new: str) -> Path:
    for name in ("test_shelf_service.o9", "shelf_service.o9"):
        shutil.copyfile(SOURCE / name, work / name)
    (work / "stdlib").symlink_to(ROOT / "stdlib")
    (work / "bytes.o9").symlink_to(ROOT / "stdlib/bytes.o9")
    target = work / "shelf_service.o9"
    source = target.read_text()
    if source.count(old) != 1:
        raise RuntimeError("shelf mutation site changed")
    target.write_text(source.replace(old, new, 1))
    return work / "test_shelf_service.o9"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutate", action="store_true")
    parser.add_argument("--window", action="store_true",
                        help="also run the headless SDL frame/input check")
    args = parser.parse_args()

    output = run_guest(SOURCE / "test_live.o9")
    if "ASPERA-LIVE-TEST PASS" not in output:
        raise RuntimeError(f"Aspera guest assertions failed:\n{output[-1600:]}")
    print("Aspera VM unit and generated properties: PASS")
    shelf_output = run_guest(SHELF_TEST)
    if "SHELF-9P PASS" not in shelf_output:
        raise RuntimeError(f"Shelf service 9P checks failed:\n{shelf_output[-1600:]}")
    print("Shelf service 9P views and controls: PASS")
    display_test = ROOT / "build/test_vmdisplay"
    subprocess.run(["clang", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                    str(DISPLAY_TEST), "-o", str(display_test)], cwd=ROOT, check=True)
    subprocess.run([str(display_test)], cwd=ROOT, check=True)
    client_test = ROOT / "build/test_vmdisplay_client"
    subprocess.run(["clang", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                    str(DISPLAY_CLIENT_TEST), str(DISPLAY_CLIENT),
                    "-o", str(client_test)], cwd=ROOT, check=True)
    subprocess.run([str(client_test)], cwd=ROOT, check=True)
    if args.mutate:
        for label, filename, old, new in MUTANTS:
            with tempfile.TemporaryDirectory(prefix="aspera-mutant-") as temporary:
                source = mutant_source(Path(temporary), filename, old, new)
                output = run_guest(source)
                if "ASPERA-LIVE-TEST FAIL" not in output:
                    raise RuntimeError(f"{label} survived:\n{output[-1600:]}")
                print(f"{label}: killed")
        for label, old, new in SHELF_MUTANTS:
            with tempfile.TemporaryDirectory(prefix="shelf-mutant-") as temporary:
                output = run_guest(shelf_mutant_source(Path(temporary), old, new))
                if "SHELF-9P FAIL" not in output:
                    raise RuntimeError(f"{label} survived:\n{output[-1600:]}")
                print(f"{label}: killed")
    if args.window:
        env = dict(os.environ, SDL_VIDEODRIVER="dummy")
        subprocess.run([sys.executable, str(LIVE), "--smoke"], cwd=ROOT,
                       env=env, check=True, timeout=70)
        print("Aspera SDL window and input round trip: PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
