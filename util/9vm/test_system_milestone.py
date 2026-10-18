#!/usr/bin/env python3
"""Verify isolated namespaces and GEFS persistence across Aspera reboots."""

from pathlib import Path
import os
import shutil
import signal
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
VM = ROOT / "util/9vm/9vm"
KERNEL = ROOT / "build/harvey.32bit"
SOURCE_IMAGE = ROOT / "build/aspera-desktop-gefs.img"


def run_boot(image: Path, mode: str) -> str:
    marker = f"ASPERA-SYSTEM-MILESTONE-{mode.upper()} PASS"
    command = [
        str(VM), "-m", "512", "-r", "/", "-gefs-image", str(image),
        "-c", f"bootargs=local!#9/0 O9_SYSTEM_MILESTONE_TEST={mode}",
        str(KERNEL),
    ]
    process = subprocess.Popen(
        command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, start_new_session=True,
    )
    try:
        output, _ = process.communicate(timeout=15)
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        if marker not in output or "ASPERA-SYSTEM-MILESTONE PASS" not in output:
            raise RuntimeError(f"Aspera {mode} boot timed out:\n{output[-4000:]}")
    finally:
        if process.poll() is None:
            os_killpg(process.pid)
            process.wait(timeout=5)

    required = (
        "[9vm] GEFS root mode: only the image file is exported",
        "ASPERA-NAMESPACE-CHILD-A PASS private mount visible",
        "ASPERA-NAMESPACE-CHILD-B PASS private mount absent",
        "ASPERA-NAMESPACE-ISOLATION PASS",
        marker,
        "ASPERA-SYSTEM-MILESTONE PASS",
    )
    forbidden = (
        "ASPERA-SYSTEM-MILESTONE FAIL", "panic:", "suicide:",
        "init: mounting #~/host", "In-process hostfs online serving",
    )
    missing = [item for item in required if item not in output]
    present = [item for item in forbidden if item in output]
    if missing or present:
        raise RuntimeError(
            f"Aspera {mode} boot failed; missing={missing}, forbidden={present}:\n"
            f"{output[-5000:]}"
        )
    print(f"Aspera namespace and persistent storage {mode}: PASS")
    return output


def os_killpg(pid: int) -> None:
    try:
        os.killpg(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass


def main() -> int:
    for path in (VM, KERNEL, SOURCE_IMAGE):
        if not path.is_file():
            raise RuntimeError(f"missing prerequisite: {path.relative_to(ROOT)}")
    with tempfile.TemporaryDirectory(prefix="aspera-system-", dir=ROOT / "build") as name:
        image = Path(name) / "root.img"
        shutil.copyfile(SOURCE_IMAGE, image)
        run_boot(image, "write")
        run_boot(image, "verify")
    print("ASPERA-SYSTEM-MILESTONE PASS namespace isolation and reboot persistence")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"test_system_milestone: {error}", file=sys.stderr)
        raise SystemExit(1)
