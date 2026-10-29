#!/usr/bin/env python3
"""Build the userspace TPM 2.0 file server and its test client."""
import os
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
SRC_DIR = os.path.join(REPO_ROOT, "sys/src/cmd/tpm")
BUILD_DIR = os.path.join(REPO_ROOT, "build/tpm")

CC = "clang"
CFLAGS = [
    "-c", "-O2", "-mno-red-zone", "-mno-implicit-float",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-mcmodel=small", "-fcommon", "-Wno-unknown-pragmas",
    "-Wno-parentheses", "-Wno-implicit-int",
    "-Wno-implicit-function-declaration",
    "-Wno-incompatible-function-pointer-types",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
    f"-I{SRC_DIR}",
]
LDFLAGS = ["-e_main", "-static", "-z", "noseparate-code",
           "-z", "max-page-size=0x200000"]


def compile_one(name: str) -> str:
    obj = os.path.join(BUILD_DIR, name.replace(".c", ".o"))
    subprocess.run([CC] + CFLAGS + [os.path.join(SRC_DIR, name),
                                    "-o", obj], check=True)
    return obj


def link_one(out: str, objects, libs) -> None:
    subprocess.run(["ld.lld", *LDFLAGS, *objects]
                   + [os.path.join(REPO_ROOT, "build", lib) for lib in libs]
                   + ["-o", os.path.join(REPO_ROOT, "build", out)],
                   check=True)


def main() -> None:
    os.makedirs(BUILD_DIR, exist_ok=True)
    objs = [compile_one(name) for name in
            ("tpmfs.c", "tpm_mock.c", "tpm_tis.c", "tpm_crb.c", "tpm2_sapi.c")]
    link_one("aspera-tpmfs", objs,
             ("lib9p.a", "libthread.a", "libauth.a", "libsec.a",
              "libndb.a", "libbio.a", "libc.a"))
    subprocess.run(["llvm-ar", "rcs", os.path.join(REPO_ROOT, "build", "libtpm2.a"),
                    os.path.join(BUILD_DIR, "tpm2_sapi.o")], check=True)

    obj = compile_one("tpmtest.c")
    link_one("aspera-tpmtest", [obj], ("libc.a",))
    print("built build/aspera-tpmfs and build/aspera-tpmtest")


if __name__ == "__main__":
    main()
