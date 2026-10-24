#!/usr/bin/env python3
import os
import sys
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
O9_REPO = os.path.abspath(os.environ.get(
    "O9_REPO", os.path.join(REPO_ROOT, "..", "objective-9c")))
O9C_BIN = os.path.join(O9_REPO, "o9c/o9c")

BOOT_O9 = os.path.join(REPO_ROOT, "boot/init.o9")
BOOT_C = os.path.join(REPO_ROOT, "build/init.o9.c")
BOOT_OBJ = os.path.join(REPO_ROOT, "build/boot_init.o")
LOCAL_RING_TEST_C = os.path.join(REPO_ROOT, "boot/test_local_9ring.c")
LOCAL_RING_TEST_OBJ = os.path.join(REPO_ROOT, "build/test_local_9ring.o")
BOOT_OUT = os.path.join(REPO_ROOT, "boot/boot")
BUILD_NETD = os.path.join(REPO_ROOT, "util/9vm/build_netd.py")

LIBO9_A = os.path.join(REPO_ROOT, "build/libo9.a")
LIBTHREAD_A = os.path.join(REPO_ROOT, "build/libthread.a")
LIB9P_A = os.path.join(REPO_ROOT, "build/lib9p.a")
LIBAUTH_A = os.path.join(REPO_ROOT, "build/libauth.a")
LIBAUTHSRV_A = os.path.join(REPO_ROOT, "build/libauthsrv.a")
LIBSEC_A = os.path.join(REPO_ROOT, "build/libsec.a")
LIBNDB_A = os.path.join(REPO_ROOT, "build/libndb.a")
LIBBIO_A = os.path.join(REPO_ROOT, "build/libbio.a")
LIBC_A = os.path.join(REPO_ROOT, "build/libc.a")

CC = "clang"
LD = "ld.lld"

CFLAGS = [
    "-c",
    "-O2",
    "-static",
    "-mno-red-zone",
    "-mno-implicit-float",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-mcmodel=small",
    "-Wall",
    "-fcommon",
    "-Wno-parentheses",
    "-Wno-missing-braces",
    "-Wno-unknown-pragmas",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
    f"-I{O9_REPO}",
    f"-I{os.path.join(O9_REPO, 'libtab')}",
]

LDFLAGS = [
    "-e_main",
    "-static",
    "-z", "noseparate-code",
    "-z", "max-page-size=0x200000",
]

def main():
    os.makedirs(os.path.dirname(BOOT_OBJ), exist_ok=True)
    os.makedirs(os.path.dirname(BOOT_OUT), exist_ok=True)

    print(f"Transpiling {BOOT_O9} with o9c...")
    with open(BOOT_C, "w") as f:
        res = subprocess.run([O9C_BIN, BOOT_O9], stdout=f)
        if res.returncode != 0:
            print("o9c transpilation failed!", file=sys.stderr)
            sys.exit(1)

    print("Building guest netd...")
    subprocess.run([sys.executable, BUILD_NETD], check=True)

    print(f"Compiling {BOOT_C}...")
    subprocess.run([CC] + CFLAGS + [BOOT_C, "-o", BOOT_OBJ], check=True)
    subprocess.run([CC] + CFLAGS + [LOCAL_RING_TEST_C, "-o", LOCAL_RING_TEST_OBJ], check=True)

    print(f"Linking {BOOT_OUT}...")
    link_cmd = [
        LD,
        *LDFLAGS,
        BOOT_OBJ,
        LOCAL_RING_TEST_OBJ,
        LIBO9_A,
        LIBTHREAD_A,
        LIB9P_A,
        LIBAUTH_A,
        LIBAUTHSRV_A,
        LIBSEC_A,
        LIBNDB_A,
        LIBBIO_A,
        LIBC_A,
        "-o",
        BOOT_OUT,
    ]
    subprocess.run(link_cmd, check=True)
    print(f"Created {BOOT_OUT} ({os.path.getsize(BOOT_OUT)} bytes)")

if __name__ == "__main__":
    main()
