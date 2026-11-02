#!/usr/bin/env python3
"""Build the ndb connection server, resolver and query tools for the guest."""

import os
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
BUILD = os.path.join(REPO_ROOT, "build")
NDB = os.path.join(REPO_ROOT, "sys/src/cmd/ndb")
MAIN9 = os.path.join(BUILD, "libc", "amd64_main9.o")

CC = "clang"
LD = "ld.lld"

CFLAGS = [
    "-c",
    "-O2",
    "-mno-red-zone",
    "-mno-implicit-float",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-mcmodel=small",
    "-fcommon",
    "-DIOUNIT=32768",
    "-Wno-parentheses",
    "-Wno-missing-braces",
    "-Wno-unknown-pragmas",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
    f"-I{NDB}",
]

LDFLAGS = [
    "-e", "_main",
    "-static",
    "-z", "noseparate-code",
    "-z", "max-page-size=0x200000",
]

LIBS = [
    "libauth.a",
    "libndb.a",
    "libip.a",
    "lib9p.a",
    "libbio.a",
    "libString.a",
    "libsec.a",
    "libthread.a",
    "libc.a",
]

DNS_SOURCES = [
    "convDNS2M.c",
    "convM2DNS.c",
    "dblookup.c",
    "dn.c",
    "dnarea.c",
    "dnnotify.c",
    "dnresolve.c",
    "dns.c",
    "dnserver.c",
    "dnudpserver.c",
]

PROGRAMS = {
    "cs": ["cs.c"],
    "csquery": ["csquery.c"],
    "dnsquery": ["dnsquery.c"],
    "ipquery": ["ipquery.c"],
    "dns": DNS_SOURCES,
}


def compile_one(src: str) -> str:
    obj = os.path.join(BUILD, "ndb-" + os.path.splitext(src)[0] + ".o")
    res = subprocess.run([CC, *CFLAGS, os.path.join(NDB, src), "-o", obj])
    if res.returncode != 0:
        raise SystemExit(f"compile failed: {src}")
    return obj


def link_program(name: str, sources: list[str]) -> None:
    objs = [compile_one(s) for s in sources]
    out = os.path.join(BUILD, f"aspera-{name}")
    cmd = (
        [LD, *LDFLAGS, "-o", out, MAIN9, *objs,
         "--start-group", *[os.path.join(BUILD, lib) for lib in LIBS], "--end-group"]
    )
    res = subprocess.run(cmd)
    if res.returncode != 0:
        raise SystemExit(f"link failed: {name}")
    print(f"built {out} ({os.path.getsize(out)} bytes)")


def main() -> None:
    for name, sources in PROGRAMS.items():
        link_program(name, sources)


if __name__ == "__main__":
    main()
