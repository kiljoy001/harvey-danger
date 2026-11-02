#!/usr/bin/env python3
"""Build the guest networking core: ipconfig, ping and the ndb tools."""

import os
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
BUILD = os.path.join(REPO_ROOT, "build")
MAIN9 = os.path.join(BUILD, "libc", "amd64_main9.o")

CC = "clang"
LD = "ld.lld"

CFLAGS = [
    "-c", "-O2", "-mno-red-zone", "-mno-implicit-float", "-ffreestanding",
    "-fno-builtin", "-fno-stack-protector", "-mcmodel=small", "-fcommon",
    "-DIOUNIT=32768", "-Wno-parentheses", "-Wno-missing-braces",
    "-Wno-unknown-pragmas", "-Wno-unused-variable", "-Wno-unused-function",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
]

LDFLAGS = ["-e", "_main", "-static", "-z", "noseparate-code",
           "-z", "max-page-size=0x200000"]


def compile_one(src: str, extra=()) -> str:
    obj = os.path.join(BUILD, "net-" + os.path.splitext(os.path.basename(src))[0] + ".o")
    res = subprocess.run([CC, *CFLAGS, *extra, src, "-o", obj])
    if res.returncode != 0:
        raise SystemExit(f"compile failed: {src}")
    return obj


def link(name: str, objs: list[str], libs: list[str]) -> None:
    out = os.path.join(BUILD, f"aspera-{name}")
    cmd = [LD, *LDFLAGS, "-o", out, MAIN9, *objs, "--start-group",
           *[os.path.join(BUILD, lib) for lib in libs], "--end-group"]
    res = subprocess.run(cmd)
    if res.returncode != 0:
        raise SystemExit(f"link failed: {name}")
    print(f"built {out} ({os.path.getsize(out)} bytes)")


def main() -> None:
    if not os.path.exists(os.path.join(BUILD, "libip.a")):
        subprocess.run(["python3", os.path.join(REPO_ROOT, "util/9vm/build_libip.py")], check=True)

    ipc = os.path.join(REPO_ROOT, "sys/src/cmd/ip/ipconfig")
    objs = [compile_one(os.path.join(ipc, f), (f"-I{ipc}",))
            for f in ("main.c", "ipv6.c", "ppp.c")]
    link("ipconfig", objs,
         ["libndb.a", "libip.a", "libauth.a", "libbio.a", "libsec.a",
          "libthread.a", "libc.a"])

    obj = compile_one(os.path.join(REPO_ROOT, "sys/src/cmd/ip/ping.c"))
    link("ping", [obj], ["libip.a", "libndb.a", "libc.a"])


if __name__ == "__main__":
    main()
