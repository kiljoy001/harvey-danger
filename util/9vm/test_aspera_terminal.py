#!/usr/bin/env python3
"""Run the native rc terminal session and close/reopen VM regression."""

import fcntl
from pathlib import Path
import shutil
import subprocess
import sys

from build_aspera_tools import main as build_shell_tools


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def main() -> int:
    exchange = ROOT / "build/aspera-live"
    exchange.mkdir(parents=True, exist_ok=True)
    with (ROOT / "build/aspera-live.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        build_shell_tools()
        guest_bin = exchange / "bin"
        guest_bin.mkdir(exist_ok=True)
        for name in ("rc", "ls", "cat", "test", "echo"):
            shutil.copy2(ROOT / "build" / f"aspera-{name}", guest_bin / name)
        created = []
        try:
            for name in ("env", "fd"):
                path = ROOT / name
                if not path.exists():
                    path.mkdir()
                    created.append(path)
            result = subprocess.run(
                [sys.executable, str(HERE / "prism.py"), "run",
                 str(ROOT / "sys/src/cmd/o9aspera/test_terminal.o9"),
                 "-m", "512", "-t", "25"],
                cwd=ROOT, capture_output=True, text=True, timeout=60,
            )
            if (result.returncode != 0 or
                    "ASPERA-TERMINAL PASS" not in result.stdout or
                    "rc: can't open /env" in result.stdout):
                print(result.stdout[-2500:], file=sys.stderr)
                print(result.stderr[-1000:], file=sys.stderr)
                return 1
            print("Aspera native rc session and close/reopen: PASS")
            return 0
        finally:
            for path in reversed(created):
                path.rmdir()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"Aspera terminal test: {error}", file=sys.stderr)
        raise SystemExit(1)
