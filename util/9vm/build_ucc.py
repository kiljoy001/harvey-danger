#!/usr/bin/env python3
"""Build native Harvey OS ELF binaries of ucc and tcc."""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UCC_REPO = Path(os.environ.get("UCC_REPO", ROOT / "../ucc")).resolve()
BUILD = ROOT / "build"
BUILD.mkdir(parents=True, exist_ok=True)

UCC_HOST = UCC_REPO / "ucc"
if not UCC_HOST.is_file():
    sys.exit(f"ucc host binary not found at {UCC_HOST}")

TCC_COMPAT = UCC_REPO / "tcc_compat.c"
if not TCC_COMPAT.is_file():
    sys.exit(f"tcc_compat.c not found at {TCC_COMPAT}")

print("Building native Harvey tcc engine with ucc -mposix...")
tcc_obj = BUILD / "tcc_engine.o"
compat_obj = BUILD / "tcc_compat.o"
tcc_bin = BUILD / "tcc"
ucc_bin = BUILD / "ucc"

cmd_compile_tcc = [
    str(UCC_HOST), "-mposix",
    "-DCONFIG_TCC_STATIC=1",
    "-DCONFIG_TCC_SEMLOCK=0",
    "-DCONFIG_TCC_BACKTRACE=0",
    "-DONE_SOURCE=1",
    "-c", str(UCC_REPO / "tcc.c"),
    "-o", str(tcc_obj)
]
subprocess.run(cmd_compile_tcc, check=True)

cmd_compile_compat = [
    str(UCC_HOST), "-mposix",
    "-c", str(TCC_COMPAT),
    "-o", str(compat_obj)
]
subprocess.run(cmd_compile_compat, check=True)

cmd_link_tcc = [
    str(UCC_HOST), "-mposix",
    str(tcc_obj), str(compat_obj),
    str(UCC_REPO / "libtcc1.a"),
    "-o", str(tcc_bin)
]
subprocess.run(cmd_link_tcc, check=True)

print("Building native Harvey ucc driver with ucc -mposix...")
cmd_link_ucc = [
    str(UCC_HOST), "-mposix",
    str(UCC_REPO / "ucc.c"),
    str(compat_obj),
    "-o", str(ucc_bin)
]
subprocess.run(cmd_link_ucc, check=True)

print(f"Successfully built native Harvey compiler binaries:")
print(f"  {tcc_bin} ({tcc_bin.stat().st_size // 1024} KB)")
print(f"  {ucc_bin} ({ucc_bin.stat().st_size // 1024} KB)")
