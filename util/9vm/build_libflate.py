#!/usr/bin/env python3
"""Build Harvey's Plan 9 libflate for Objective-9C guest applications."""

import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "sys/src/libflate"
BUILD = ROOT / "build/libflate"
OUTPUT = ROOT / "build/libflate.a"


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    with (SOURCE / "libflate.json").open() as stream:
        files = json.load(stream)[0]["SourceFiles"]
    objects = []
    for name in files:
        source = SOURCE / name
        output = BUILD / (source.stem + ".o")
        subprocess.run([
            "clang", "-c", "-O2", "-g", "-Wall", "-mno-red-zone",
            "-mno-implicit-float", "-ffreestanding", "-fno-builtin",
            "-fno-stack-protector", "-mcmodel=small",
            f"-I{ROOT / 'amd64/include'}", f"-I{ROOT / 'sys/include'}",
            str(source), "-o", str(output),
        ], check=True)
        objects.append(str(output))
    subprocess.run(["llvm-ar", "rcs", str(OUTPUT), *objects], check=True)
    print(f"libflate.a created at {OUTPUT}")


if __name__ == "__main__":
    main()
