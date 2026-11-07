#!/usr/bin/env python3
"""Self-build an LLVM tool with the guest's own clang on Harvey/ARM64.

``--tool`` selects llvm-as (default) or llvm-dis.  The script stages the LLVM
headers, the prebuilt ARM64 LLVM archives and the tool's source into the SDK
archive, then boots a guest driver that

  1. compiles the TU with the SDK clang++ (the guest compiler),
  2. links it with the SDK ld.lld against those archives,
  3. runs the resulting llvm-as on a .ll file, and
  4. compares the bitcode byte for byte with the host build of the same
     revision.

The base SDK tar must come from ``util/arm64/install-sdk.py --tar`` and must
not already contain the ``sdk/l/`` overlay.  The host reference is produced
with the host ``llvm-as`` built from the same checkout.

    python3 util/arm64/test-selfbuild.py \
        --sdk-tar /tmp/opencode/sdk-full.tar \
        --llvm /home/scott/Repo/llvm-project \
        --build /tmp/opencode/llvm-arm64-full \
        --host-llvm-as /tmp/opencode/llvm-host/bin/llvm-as
"""
from pathlib import Path
import argparse
import shutil
import struct
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

# The archive order from LLVM's bin/llvm-as link line.
ARCHIVES = """AsmParser BitWriter Core Support Analysis FrontendHLSL
ProfileData Symbolize DebugInfoGSYM DebugInfoPDB DebugInfoCodeView
DebugInfoMSF DebugInfoBTF DebugInfoDWARF Object IRReader BitReader Remarks
BitstreamReader MCParser MC TextAPI DebugInfoDWARFLowLevel BinaryFormat
TargetParser Demangle""".split()

TEST_LL = """\
; ModuleID = 'guest'
source_filename = "guest"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-unknown-linux-elf"

define i32 @add(i32 %a, i32 %b) {
entry:
  %r = add i32 %a, %b
  ret i32 %r
}
"""


def run(cmd: list) -> None:
    print("+", " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sdk-tar", default="/tmp/opencode/sdk-full.tar",
                    help="clean SDK ustar from install-sdk.py --tar")
    ap.add_argument("--llvm", default="/home/scott/Repo/llvm-project",
                    help="LLVM source checkout")
    ap.add_argument("--build", default="/tmp/opencode/llvm-arm64-full",
                    help="ARM64 LLVM build tree (headers and archives)")
    ap.add_argument("--host-llvm-as",
                    default="/tmp/opencode/llvm-host/bin/llvm-as",
                    help="host llvm-as from the same revision")
    ap.add_argument("--tool", default="llvm-as",
                    choices=["llvm-as", "llvm-dis"],
                    help="LLVM tool to self-build")
    ap.add_argument("--host-tool", default="",
                    help="host reference for --tool (default: "
                         "/tmp/opencode/llvm-host/bin/<tool>)")
    ap.add_argument("--stage", default="/tmp/opencode/selfbuild-stage")
    ap.add_argument("--boot", default="/tmp/opencode/sdk-selfbuild.boot")
    ap.add_argument("--dump", default="/tmp/opencode/selfbuild.log")
    ap.add_argument("--mem", type=int, default=3072)
    ap.add_argument("--smp", type=int, default=1,
                    help="QEMU vCPUs (the guest kernel supports SMP)")
    ap.add_argument("--deadline", type=int, default=900)
    ap.add_argument("--keep", action="store_true",
                    help="keep the staged tree and archive")
    args = ap.parse_args()

    sdk_tar = Path(args.sdk_tar)
    llvm = Path(args.llvm)
    build = Path(args.build)
    host_as = Path(args.host_llvm_as)
    tool = args.tool
    host_tool = Path(args.host_tool or
                     f"/tmp/opencode/llvm-host/bin/{tool}")
    if not sdk_tar.is_file():
        sys.exit(f"missing {sdk_tar}; run install-sdk.py --tar first")
    if not host_as.is_file():
        sys.exit(f"missing host reference {host_as}")
    if not host_tool.is_file():
        sys.exit(f"missing host reference {host_tool}")
    with tarfile.open(sdk_tar) as tar:
        if any(n.startswith("sdk/l/") for n in tar.getnames()):
            sys.exit(f"{sdk_tar} already has the sdk/l overlay; "
                     "regenerate it with install-sdk.py --tar")

    stage = Path(args.stage)
    shutil.rmtree(stage, ignore_errors=True)
    l = stage / "l"
    (l / "lib").mkdir(parents=True)
    (l / "src").mkdir()
    shutil.copytree(llvm / "llvm" / "include", l / "include")
    shutil.copytree(build / "include", l / "include-gen")
    shutil.copy2(llvm / "llvm" / "tools" / tool / f"{tool}.cpp", l / "src")
    for name in ARCHIVES:
        shutil.copy2(build / "lib" / f"libLLVM{name}.a", l / "lib")
    (l / "test.ll").write_text(TEST_LL)
    run([str(host_as), str(l / "test.ll"), "-o", str(l / "host.bc")])
    if tool == "llvm-dis":
        shutil.copy2(l / "host.bc", l / "test.bc")
        run([str(host_tool), str(l / "test.bc"), "-o", str(l / "host.ll")])

    # The guest runner's ustar parser reads the 100-byte name field and the
    # 155-byte prefix; a name of exactly 100 bytes would lose its NUL.
    for path in l.rglob("*"):
        name = "sdk/l/" + path.relative_to(l).as_posix()
        if len(name.encode()) >= 100:
            sys.exit(f"overlay path too long for ustar: {name}")

    tar_path = stage / "sdk.tar"
    shutil.copyfile(sdk_tar, tar_path)
    with tarfile.open(tar_path, "a", format=tarfile.USTAR_FORMAT) as tar:
        for path in sorted(l.rglob("*")):
            arc = "sdk/l/" + path.relative_to(l).as_posix()
            info = tar.gettarinfo(str(path), arcname=arc)
            if path.is_dir():
                tar.addfile(info)
            else:
                with open(path, "rb") as f:
                    tar.addfile(info, f)

    size = tar_path.stat().st_size
    boot = Path(args.boot)
    with open(boot, "wb") as out:
        out.write(struct.pack("<8sIIII", b"O9BOOT01", 1, 1, 24 + 48 + size, 0))
        out.write(struct.pack("<32sIIII", b"sdk.tar", 24 + 48, size, 0o555, 0))
        with open(tar_path, "rb") as f:
            shutil.copyfileobj(f, out)

    bindir = stage / "bin"
    bindir.mkdir()
    run([str(HERE / "aarch64-clang"), str(HERE / "selfbuild.c"),
         "-o", str(bindir / "selfbuild")])
    etc = stage / "etc"
    etc.mkdir()
    shutil.copy2(HERE / "selfbuild-compile.cfg", etc / "compile.cfg")
    linkcfg = (HERE / "selfbuild-link.cfg").read_text()
    linkcfg = linkcfg.replace("/initramfs/llvm-as.o", f"/initramfs/{tool}.o")
    (etc / "link.cfg").write_text(linkcfg)

    runner = stage / "runner.elf"
    run([sys.executable, str(HERE / "mkrun.py"),
         str(ROOT / "build" / "arm64-apex" / "hello.elf"),
         "--in-tree", "/initramfs/bin/selfbuild",
         "--untar", "/boot/sdk.tar:/initramfs",
         "--env", f"SB_TOOL={tool}",
         "--tree", f"{bindir}:/initramfs/bin",
         "--tree", f"{etc}:/initramfs/etc",
         "--suffix", "", "--output", str(runner)])

    dump = Path(args.dump)
    run([sys.executable, str(HERE / "test_apex_boot.py"),
         "--elf", str(runner), "--archive", str(boot),
         "--mem", str(args.mem), "--smp", str(args.smp), "--marker", "SELFBUILD-DONE",
         "--allow-missing", "--deadline", str(args.deadline),
         "--dump", str(dump)])

    text = dump.read_text(errors="replace")
    for line in text.splitlines():
        if line.startswith("SB ") or line.startswith("SELFBUILD"):
            print(line)
    if "SB MATCH" not in text:
        sys.exit(f"{tool} self-build did not produce matching output")
    print("self-build PASS")
    if not args.keep:
        shutil.rmtree(stage, ignore_errors=True)


if __name__ == "__main__":
    main()
