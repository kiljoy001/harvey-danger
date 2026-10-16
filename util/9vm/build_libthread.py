#!/usr/bin/env python3
import os
import sys
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
LIBTHREAD_DIR = os.path.join(REPO_ROOT, "sys/src/libthread")
BUILD_DIR = os.path.join(REPO_ROOT, "build/libthread")
OUT_LIB = os.path.join(REPO_ROOT, "build/libthread.a")

CC = "clang"
AR = "llvm-ar"

CFLAGS = [
    "-c",
    "-mno-red-zone",
    "-mno-implicit-float",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-mcmodel=small",
    "-Wall",
    "-Wno-parentheses",
    "-Wno-missing-braces",
    "-Wno-unknown-pragmas",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    "-O2",
    "-g",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
    f"-I{LIBTHREAD_DIR}",
]

SOURCES = [
    "amd64.c",
    "channel.c",
    "chanprint.c",
    "create.c",
    "debug.c",
    "dial.c",
    "exec.c",
    "exit.c",
    "id.c",
    "iocall.c",
    "ioclose.c",
    "iodial.c",
    "ioopen.c",
    "ioproc.c",
    "ioread.c",
    "ioreadn.c",
    "iosleep.c",
    "iowrite.c",
    "kill.c",
    "lib.c",
    "main.c",
    "note.c",
    "ref.c",
    "rendez.c",
    "sched.c",
]

def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    objs = []
    for s in SOURCES:
        src = os.path.join(LIBTHREAD_DIR, s)
        base = os.path.splitext(s)[0]
        obj = os.path.join(BUILD_DIR, f"{base}.o")
        objs.append(obj)
        res = subprocess.run([CC] + CFLAGS + [src, "-o", obj])
        if res.returncode != 0:
            print(f"Failed to compile {src}")
            sys.exit(1)

    subprocess.run([AR, "rcs", OUT_LIB] + objs, check=True)
    print(f"libthread.a created successfully at {OUT_LIB} ({os.path.getsize(OUT_LIB)} bytes)")

if __name__ == "__main__":
    main()
