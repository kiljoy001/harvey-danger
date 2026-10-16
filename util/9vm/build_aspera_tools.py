#!/usr/bin/env python3
"""Cross-build a minimal native rc command set for the Aspera GEFS image."""

import json
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
CFLAGS = [
    "-c", "-O2", "-static", "-mno-red-zone", "-mno-implicit-float",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-mcmodel=small", "-fcommon", "-Wno-unknown-pragmas",
    f"-I{ROOT / 'amd64/include'}", f"-I{ROOT / 'sys/include'}",
]
LDFLAGS = ["-e_main", "-static", "-z", "noseparate-code",
           "-z", "max-page-size=0x200000"]


def compile_file(source: Path, output: Path, extra=()) -> None:
    subprocess.run(["clang", *CFLAGS, *extra, str(source), "-o", str(output)],
                   check=True)


def build_rc() -> None:
    source = ROOT / "sys/src/cmd/rc"
    work = BUILD / "aspera-rc-build"
    work.mkdir(parents=True, exist_ok=True)
    for path in source.iterdir():
        if path.suffix in (".c", ".h", ".y"):
            shutil.copy2(path, work / path.name)
    subprocess.run(["bison", "-y", "-d", "-Wno-yacc", "syn.y"],
                   cwd=work, check=True)
    shutil.copy2(work / "y.tab.h", work / "x.tab.h")
    names = json.loads((source / "rc.json").read_text())[0]["SourceFiles"]
    objects = []
    for name in names:
        obj = work / f"{name}.o"
        compile_file(work / name, obj, (f"-I{work}",))
        objects.append(obj)

    string_source = ROOT / "sys/src/libString"
    string_work = BUILD / "aspera-libString"
    string_work.mkdir(parents=True, exist_ok=True)
    string_names = json.loads((string_source / "libString.json").read_text())[0]["SourceFiles"]
    string_objects = []
    for name in string_names:
        obj = string_work / f"{name}.o"
        compile_file(string_source / name, obj)
        string_objects.append(obj)
    string_lib = BUILD / "aspera-libString.a"
    subprocess.run(["llvm-ar", "rcs", str(string_lib),
                    *map(str, string_objects)], check=True)
    subprocess.run(["ld.lld", *LDFLAGS, *map(str, objects),
                    str(string_lib), str(BUILD / "libc.a"),
                    "-o", str(BUILD / "aspera-rc")], check=True)


def build_simple(name: str) -> None:
    obj = BUILD / f"aspera-{name}.o"
    compile_file(ROOT / f"sys/src/cmd/{name}.c", obj)
    libs = [str(BUILD / "libbio.a")] if name == "ls" else []
    subprocess.run(["ld.lld", *LDFLAGS, str(obj), *libs,
                    str(BUILD / "libc.a"), "-o", str(BUILD / f"aspera-{name}")],
                   check=True)


def main() -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    build_rc()
    for name in ("ls", "cat", "test", "echo"):
        build_simple(name)
    print("Built native Aspera rc, ls, cat, test, and echo")


if __name__ == "__main__":
    main()
