#!/usr/bin/env python3
"""Build the AArch64 Harvey microkernel with core 9P namespace services.

The build fails on any compile or link error. It does not substitute stubs for
shared kernel services. The architecture entry code is ARM64 specific; the
shared source inventory is read from the kernel's own JSON manifest.
"""

from pathlib import Path
import json
import os
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
ARCH = ROOT / "sys/src/9/aarch64"
OUT = Path(os.environ.get("ARM64_KERNEL_OUT", ROOT / "build/arm64-kernel"))
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM64_LD", "ld.lld")
OBJCOPY = os.environ.get("ARM64_OBJCOPY", "llvm-objcopy")
AR = os.environ.get("ARM64_AR", "llvm-ar")


def manifest_sources(name: str) -> list[Path]:
    manifest = ROOT / f"sys/src/9/{name}/{name}.json"
    entries = json.loads(manifest.read_text())
    return [(manifest.parent / item).resolve()
            for item in entries[0]["SourceFiles"]]


def build_klibc(flags: list[str]) -> Path:
    libc = ROOT / "sys/src/libc"
    entries = json.loads((libc / "klibc.json").read_text())[0]["SourceFiles"]
    objects = []
    failures = []
    # Kernel libc must not emit SIMD moves for plain integer/text routines.
    # Arithmetic routines with a floating-point ABI still need FP enabled and
    # state management before they can safely be called from kernel context.
    libcflags = [flag for flag in flags if flag != "-D__HARVEY__"]
    for source_name in entries:
        source = libc / source_name
        obj = OUT / ("libc_" + source_name.replace("/", "_") + ".o")
        result = subprocess.run([CC, *libcflags, "-I", str(libc),
                                 "-c", str(source), "-o", str(obj)],
                                text=True, capture_output=True)
        if result.returncode and "requires 'double' type support" in result.stderr:
            fpflags = [flag for flag in libcflags if flag != "-mgeneral-regs-only"]
            result = subprocess.run([CC, *fpflags, "-I", str(libc),
                                     "-c", str(source), "-o", str(obj)],
                                    text=True, capture_output=True)
        if result.returncode:
            failures.append((source_name, result.stderr))
        else:
            objects.append(obj)
    if failures:
        print(f"AArch64 kernel libc compile: {len(objects)} passed, "
              f"{len(failures)} failed")
        for name, error in failures:
            print(f"\n{name}:\n{error}")
        raise SystemExit(1)
    archive = OUT / "klibc.a"
    subprocess.run([AR, "rcs", str(archive), *map(str, objects)], check=True)
    return archive


def build_library(name: str, flags: list[str]) -> Path:
    directory = ROOT / f"sys/src/{name}"
    entries = json.loads((directory / f"k{name}.json").read_text())[0]["SourceFiles"]
    objects = []
    failures = []
    libflags = ["--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
                "-mgeneral-regs-only", "-mstrict-align",
                "-fno-stack-protector", "-fcommon", "-O2", "-g",
                "-Wno-parentheses", "-Wno-missing-braces",
                "-I", str(ROOT / "aarch64/include"),
                "-I", str(ROOT / "sys/include"),
                "-I", str(ROOT / "sys/src/libc")]
    for source_name in entries:
        source = directory / source_name
        obj = OUT / (name + "_" + source_name.replace("/", "_") + ".o")
        result = subprocess.run([CC, *libflags, "-I", str(directory),
                                 "-c", str(source), "-o", str(obj)],
                                text=True, capture_output=True)
        if result.returncode:
            failures.append((source_name, result.stderr))
        else:
            objects.append(obj)
    if failures:
        print(f"AArch64 {name} compile: {len(objects)} passed, "
              f"{len(failures)} failed")
        for source_name, error in failures:
            print(f"\n{source_name}:\n{error}")
        raise SystemExit(1)
    archive = OUT / f"k{name}.a"
    subprocess.run([AR, "rcs", str(archive), *map(str, objects)], check=True)
    return archive


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    if os.environ.get("ARM64_REUSE_USERLAND") != "1":
        subprocess.run([
            sys.executable, str(ROOT / "util/arm64/build_user_probe.py"),
            "--output", str(OUT / "userprobe.h"),
        ], check=True)
        subprocess.run([
            sys.executable, str(ROOT / "util/arm64/build_userland.py"),
            "--output", str(OUT / "userland.h"),
        ], check=True)
    elif not (OUT / "userprobe.h").is_file() or not (OUT / "userland.h").is_file():
        raise SystemExit("ARM64_REUSE_USERLAND=1 needs existing generated headers")
    for mode, name in (("errstr.h", "errstr.h"), ("systab.c", "systab.c"),
                       ("sys.h", "sys.h")):
        subprocess.run([sys.executable, str(ROOT / "util/9vm/mksys.py"),
                        f"-mode={mode}", "-o", str(OUT / name),
                        str(ROOT / "sys/src/sysconf.json")],
                       check=True, stdout=subprocess.DEVNULL)

    flags = [
        "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables", "-mgeneral-regs-only",
        "-mstrict-align",
        "-fcommon", "-fno-omit-frame-pointer", "-O2", "-g", "-D__HARVEY__",
        "-DARM64_FULL_KERNEL",
        "-DKERNDATE=1433623937", "-Wall", "-Wno-parentheses",
        "-Wno-missing-braces", "-Wno-unknown-pragmas",
        "-Wno-unused-function", "-Wno-unused-variable",
        "-Wno-unused-parameter",
    ]
    if os.environ.get("ARM64_RK3566_NPU_PROBE") == "1":
        flags.append("-DARM64_RK3566_NPU_PROBE")
    for path in (OUT, ARCH, ROOT / "sys/src/9/port", ROOT / "sys/src/9/ip",
                 ROOT / "aarch64/include", ROOT / "sys/include",
                 ROOT / "sys/src/libc/9syscall"):
        flags.extend(("-I", str(path)))

    sources = [ARCH / name for name in
               ("l.S", "user.S", "context.S", "return.S", "main.c", "fdt.c", "timer.c",
                "phys.c", "mmu.c", "user.c", "context.c", "arch.c",
                "config.c", "qmalloc.c", "platform.c", "procarch.c", "syscall.c",
                "boot.c", "devpnp.c")]
    core_devices = {
        "dev.c", "devcap.c", "devcons.c", "devdup.c", "devenv.c",
        "devmnt.c", "devpipe.c", "devproc.c", "devramfs.c",
        "devroot.c", "devsegment.c", "devsrv.c", "devtab.c",
    }
    sources.extend(source for source in manifest_sources("port")
                   if not source.name.startswith("dev") or source.name in core_devices)
    sources.append(ROOT / "sys/src/9/port/dev9ring.c")
    sources.append(ROOT / "sys/src/9/port/devvirtio9p.c")
    sources.append(OUT / "systab.c")
    objects = []
    failures = []
    for source in sources:
        label = source.relative_to(ROOT) if source.is_relative_to(ROOT) else source.name
        obj = OUT / (str(label).replace("/", "_") + ".o")
        result = subprocess.run([CC, *flags, "-c", str(source), "-o", str(obj)],
                                text=True, capture_output=True)
        if result.returncode:
            failures.append((str(label), result.stderr))
        else:
            objects.append(obj)
    if failures:
        print(f"AArch64 full kernel compile: {len(objects)} passed, {len(failures)} failed")
        for label, error in failures:
            print(f"\n{label}:\n{error}")
        raise SystemExit(1)

    klibc = build_klibc(flags)
    klibip = build_library("libip", flags)
    klibsec = build_library("libsec", flags)
    elf = OUT / "harvey.elf"
    image = OUT / "harvey.img"
    subprocess.run([LD, "--error-limit=0", "-T", str(ARCH / "virt.ld"),
                    *map(str, objects), "--start-group",
                    str(klibc), str(klibip), str(klibsec), "--end-group",
                    "-o", str(elf)], check=True)
    subprocess.run([OBJCOPY, "-O", "binary", str(elf), str(image)], check=True)
    print(f"AArch64 full kernel linked: {elf}")
    print(f"AArch64 boot image: {image}")


if __name__ == "__main__":
    main()
