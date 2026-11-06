#!/usr/bin/env python3
"""Self-build llvm-tblgen with the guest's own clang on Harvey/ARM64.

The script stages the LLVM headers, the TableGen emitter sources
(``llvm/utils/TableGen``), the prebuilt ARM64 TableGen/Support archives and
a manifest of the tool's translation units into the SDK archive.  A guest
driver then

  1. compiles every TableGen source with the SDK clang++,
  2. links the objects with the SDK ld.lld against those archives,
  3. runs the resulting llvm-tblgen on ``ValueTypes.td``, and
  4. compares the ``-gen-vt`` fragment byte for byte with a host build of the
     same revision.

The base SDK tar must come from ``util/arm64/install-sdk.py --tar`` and must
not already contain the ``sdk/l/`` overlay.

    python3 util/arm64/test-selfbuild-tblgen.py \
        --sdk-tar /tmp/opencode/sdk-full.tar \
        --llvm /home/scott/Repo/llvm-project \
        --build /tmp/opencode/llvm-arm64-full \
        --host-llvm-tblgen /tmp/opencode/llvm-host/bin/llvm-tblgen

The ARM64 build tree must already have ``lib/libLLVMTableGen.a`` (for example
``ninja lib/libLLVMTableGen.a`` in that tree).
"""
from pathlib import Path
import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

# Archives the tool links against; the emitters themselves are compiled here.
LIBS = ["CodeGenTypes", "TableGen", "Support", "Demangle", "TargetParser",
        "BinaryFormat"]
# Headers needed to compile llvm/utils/TableGen sources.
INCLUDE_DIRS = ["include", "include-gen", "tg", "tg/Basic", "tg/Common"]


def run(cmd: list) -> None:
    print("+", " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True)


def tblgen_sources(build: Path) -> list:
    """Return llvm/utils/TableGen sources in bin/llvm-tblgen's object order."""
    ninja = (build / "build.ninja").read_text()
    if "build bin/llvm-tblgen:" not in ninja:
        sys.exit(f"{build} has no llvm-tblgen rule")
    line = ninja.split("build bin/llvm-tblgen:", 1)[1].split("\n", 1)[0]
    objs = [t for t in line.split("|", 1)[0].split()[1:] if t.endswith(".o")]
    commands = {e["output"]: e for e in
                json.loads((build / "compile_commands.json").read_text())}
    relative = "llvm/utils/TableGen/"
    sources = []
    for obj in objs:
        entry = commands.get(obj)
        if entry is None:
            sys.exit(f"no compile command for {obj}")
        if relative not in entry["file"]:
            sys.exit(f"unexpected llvm-tblgen source {entry['file']}")
        sources.append(entry["file"].split(relative, 1)[1])
    return sources


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sdk-tar", default="/tmp/opencode/sdk-full.tar",
                    help="clean SDK ustar from install-sdk.py --tar")
    ap.add_argument("--llvm", default="/home/scott/Repo/llvm-project")
    ap.add_argument("--build", default="/tmp/opencode/llvm-arm64-full")
    ap.add_argument("--host-llvm-tblgen",
                    default="/tmp/opencode/llvm-host/bin/llvm-tblgen")
    ap.add_argument("--stage", default="/tmp/opencode/tblgen-stage")
    ap.add_argument("--boot", default="/tmp/opencode/sdk-tblgen.boot")
    ap.add_argument("--dump", default="/tmp/opencode/tblgen.log")
    ap.add_argument("--mem", type=int, default=0,
                    help="guest RAM in MiB (0: 3072, or 6144 with --jobs > 1)")
    ap.add_argument("--smp", type=int, default=1,
                    help="QEMU vCPUs (the guest kernel supports SMP)")
    ap.add_argument("--jobs", type=int, default=1,
                    help="parallel guest clang++ compiles (TGEN_JOBS)")
    ap.add_argument("--deadline", type=int, default=5400)
    ap.add_argument("--limit", type=int, default=0,
                    help="compile only the first N sources (smoke test)")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--stage-only", action="store_true",
                    help="build the SDK overlay/runner and stop before QEMU")
    args = ap.parse_args()

    sdk_tar = Path(args.sdk_tar)
    llvm = Path(args.llvm)
    build = Path(args.build)
    host_tblgen = Path(args.host_llvm_tblgen)
    if not sdk_tar.is_file():
        sys.exit(f"missing {sdk_tar}; run install-sdk.py --tar first")
    if not host_tblgen.is_file():
        sys.exit(f"missing host reference {host_tblgen}")
    for name in LIBS:
        if not (build / "lib" / f"libLLVM{name}.a").is_file():
            sys.exit(f"missing libLLVM{name}.a; "
                     f"run ninja lib/libLLVMTableGen.a in {build}")
    with tarfile.open(sdk_tar) as tar:
        if any(n.startswith("sdk/l/") for n in tar.getnames()):
            sys.exit(f"{sdk_tar} already has the sdk/l overlay; "
                     "regenerate it with install-sdk.py --tar")

    sources = tblgen_sources(build)
    print(f"{len(sources)} llvm-tblgen translation units")

    stage = Path(args.stage)
    shutil.rmtree(stage, ignore_errors=True)
    l = stage / "l"
    (l / "lib").mkdir(parents=True)
    (l / "tg").mkdir()
    shutil.copytree(llvm / "llvm" / "include", l / "include")
    shutil.copytree(build / "include", l / "include-gen")
    shutil.copytree(llvm / "llvm" / "utils" / "TableGen", l / "tg",
                    dirs_exist_ok=True)
    for name in LIBS:
        shutil.copy2(build / "lib" / f"libLLVM{name}.a", l / "lib")
    (l / "tg.list").write_text("".join(
        f"/initramfs/sdk/l/tg/{src}\n" for src in sources))
    run([str(host_tblgen), "-gen-vt", "-I", str(l / "include"),
         str(l / "include" / "llvm" / "CodeGen" / "ValueTypes.td"),
         "-o", str(l / "host-vt.inc")])

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
    run([str(HERE / "aarch64-clang"), str(HERE / "selfbuild-tblgen.c"),
         "-o", str(bindir / "tblgen")])
    etc = stage / "etc"
    etc.mkdir()
    shutil.copy2(HERE / "tblgen-compile.cfg", etc / "tg-compile.cfg")
    shutil.copy2(HERE / "tblgen-link.cfg", etc / "tg-link.cfg")

    runner = stage / "runner.elf"
    mkrun = [sys.executable, str(HERE / "mkrun.py"),
             str(ROOT / "build" / "arm64-apex" / "hello.elf"),
             "--in-tree", "/initramfs/bin/tblgen",
             "--untar", "/boot/sdk.tar:/initramfs",
             "--env", "TMPDIR=/initramfs/sdk/tmp"]
    if args.mem == 0:
        args.mem = 6144 if args.jobs > 1 else 3072
    elif args.jobs > 1 and args.mem < 6144:
        print(f"note: --jobs {args.jobs} at {args.mem} MiB; "
              "the guest may run out of physical memory")
    if args.limit:
        mkrun += ["--env", f"TGEN_LIMIT={args.limit}"]
    if args.jobs > 1:
        mkrun += ["--env", f"TGEN_JOBS={args.jobs}"]
    mkrun += ["--tree", f"{bindir}:/initramfs/bin",
              "--tree", f"{etc}:/initramfs/etc",
              "--suffix", "", "--output", str(runner)]
    run(mkrun)
    if args.stage_only:
        print(f"stage ready: {stage}")
        return

    marker = "TGEN-LIMIT-DONE" if args.limit else "TGEN-DONE"
    dump = Path(args.dump)
    run([sys.executable, str(HERE / "test_apex_boot.py"),
         "--elf", str(runner), "--archive", str(boot),
         "--mem", str(args.mem), "--smp", str(args.smp), "--marker", marker,
         "--allow-missing", "--deadline", str(args.deadline),
         "--dump", str(dump)])

    text = dump.read_text(errors="replace")
    for line in text.splitlines():
        if line.startswith("TGEN"):
            print(line)
    if args.limit:
        ok = "TGEN-LIMIT-DONE" in text and f"compiled {args.limit} files" in text
    else:
        ok = "TGEN VT-MATCH" in text and "TGEN-DONE" in text
    if not ok:
        sys.exit("tblgen self-build did not produce matching output")
    print("tblgen self-build PASS")
    if not args.keep:
        shutil.rmtree(stage, ignore_errors=True)


if __name__ == "__main__":
    main()
