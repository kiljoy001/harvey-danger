#!/usr/bin/env python3
"""Verify every generated syscall wrapper, including ARM64 cross output."""

from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SYSCONF = ROOT / "sys/src/sysconf.json"
MKSYS = ROOT / "util/9vm/mksys.py"
CC = shutil.which("clang-23") or shutil.which("clang")


def fail(message: str) -> None:
    raise RuntimeError(message)


def check_arch(calls: list[dict], root: Path, arch: str) -> None:
    out = root / arch
    out.mkdir()
    command = [sys.executable, str(MKSYS), "-mode=syscallfiles"]
    if arch == "aarch64":
        command.append("-arch=aarch64")
    command.extend(("-o", str(out), str(SYSCONF)))
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    generated = {path.stem for path in out.glob("*.s")}
    expected = {call.get("Libname", call["Name"]) for call in calls}
    if generated != expected:
        fail(f"{arch} wrapper set mismatch: {generated ^ expected}")
    for call in calls:
        name = call.get("Libname", call["Name"])
        source = out / f"{name}.s"
        text = source.read_text()
        if arch == "aarch64":
            instructions = (f"mov x8, #{call['Id']}", "svc #0", "ret")
            target = "aarch64-none-elf"
        else:
            instructions = (f"movq ${call['Id']}, %rax", "syscall", "ret")
            target = "x86_64-none-elf"
        required = (f".globl\t{name}", f".type\t{name}, %function",
                    *instructions)
        if not all(fragment in text for fragment in required):
            fail(f"incorrect {arch} wrapper for {name}")
        subprocess.run([
            CC, f"--target={target}", "-c", str(source),
            "-o", str(out / f"{name}.o"),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def main() -> None:
    if CC is None:
        fail("a cross-target clang is required")
    calls = json.loads(SYSCONF.read_text())["Syscalls"]
    with tempfile.TemporaryDirectory(prefix="harvey-syscalls-") as raw:
        root = Path(raw)
        for arch in ("amd64", "aarch64"):
            check_arch(calls, root, arch)
    print(f"amd64 and ARM64 syscall wrappers: {len(calls)} each PASS")


if __name__ == "__main__":
    main()
