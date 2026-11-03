#!/usr/bin/env python3
"""Turn a host LLVM ninja target into an hbuild manifest for the guest.

The cross build's own command graph is the source of truth for flags and
file lists.  This script rewrites the tool and every path from the host to
the guest's /n/host tree, drops the shell-isms (`cmake -E rm ... &&`), and
writes the commands as three stages:

    all compiles  ->  all archives  ->  the link

which is the shape hbuild understands.  Outputs go to build/llvm-guest, so
the host build tree is left alone.

    util/clang-harvey/make-guest-build.py [build-dir] [target] [source-dir]
"""
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GUEST = "/n/host/build/llvm-guest"
GB = "/n/host/build/llvm-build"
GS = "/n/host/build/llvm-src"

DEFAULT_BUILD = Path("/tmp/opencode/llvm-thr")
DEFAULT_SRC = Path("/home/scott/Repo/llvm-project")

# Warnings are not useful on the target's headers, and -O1 halves the guest
# compile time; this tool only has to work, not to be fast.
DROP_PREFIX = ("-W", "-pedantic", "-fdiagnostics-color", "-fcolor-diagnostics")
DROP_WITH_VALUE = ("-MT", "-MF")
# The host wrapper drops these; APEX's libc/libc++ already provide them.
DROP_LIBS = ("-lpthread", "-lm", "-ldl", "-lrt", "-lc", "-lz", "-lstdc++",
             "-lgcc", "-lgcc_s", "--as-needed", "--no-as-needed",
             "-Wl,--as-needed", "-Wl,--no-as-needed")


def convert(part, build: Path, src: Path) -> list[str] | None:
    args = shlex.split(part)
    if not args:
        return None
    if args[0].endswith("cmake") and len(args) > 1 and args[1] == "-E":
        return None

    tool = args[0]
    if tool.endswith("harvey-clang++"):
        args[0] = "/n/host/build/clang-harvey/clang++"
    elif tool.endswith("harvey-clang"):
        args[0] = "/n/host/build/clang-harvey/clang"
    elif tool.endswith("harvey-ar"):
        args[0] = "/n/host/build/clang-harvey/llvm-ar"
    else:
        # Generated headers were produced by the *host* tablegen; they are
        # already in the build tree, and the host tool cannot run in the
        # guest.  Skip anything that is not our compiler or archiver.
        return None

    def to_guest(a: str) -> str:
        return a.replace(str(build), GB).replace(str(src), GS)

    def out_path(a: str) -> str:
        a = to_guest(a)
        if a.startswith(GB + "/"):
            return GUEST + a[len(GB):]
        if a.startswith("/"):
            return a
        return GUEST + "/" + a

    out = [args[0]]
    i = 1
    while i < len(args):
        a = args[i]
        if a == "-MD":
            i += 1
            continue
        if a in DROP_WITH_VALUE:
            i += 2
            continue
        if a.startswith(DROP_PREFIX):
            i += 1
            continue
        if a in DROP_LIBS:
            i += 1
            continue
        if a in ("-O3", "-O2"):
            out.append("-O1")
            i += 1
            continue
        if a == "-o":
            out.extend(["-o", out_path(args[i + 1])])
            i += 2
            continue
        a = to_guest(a)
        if a.startswith("/"):
            out.append(a)
            i += 1
            continue
        if not a.startswith("-") and (a.endswith(".o") or a.endswith(".a")):
            out.append(out_path(a))
            i += 1
            continue
        out.append(a)
        i += 1
    return out


def host_path(p: str) -> Path:
    if p.startswith("/n/host/"):
        return REPO / p[len("/n/host/"):]
    return Path(p)


def main():
    build = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BUILD
    target = sys.argv[2] if len(sys.argv) > 2 else "llvm-tblgen"
    src = Path(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_SRC

    text = subprocess.run(["ninja", "-C", str(build), "-t", "commands", target],
                          text=True, capture_output=True, check=True).stdout

    stages = [[], [], []]
    asm_objs = []
    skipped = 0
    for line in text.splitlines():
        for part in line.split(" && "):
            part = part.strip()
            if part.startswith(":"):
                part = part[1:].strip()
            argv = convert(part, build, src)
            if not argv:
                continue
            # The guest's clang++.cfg force-includes compat.h, which cannot
            # be assembled.  The cross build already produced these objects
            # for the same target, so reuse them instead of recompiling.
            if "-c" in argv and any(a.endswith((".S", ".s"))
                                    for a in argv if not a.startswith("-")):
                if "-o" in argv:
                    asm_objs.append(argv[argv.index("-o") + 1])
                continue
            if "-c" in argv:
                # Rebuild only what is missing or older than its source.
                src_arg = out_arg = None
                for i, a in enumerate(argv):
                    if a == "-o" and i + 1 < len(argv):
                        out_arg = argv[i + 1]
                    elif not a.startswith("-") and a.endswith(
                            (".cpp", ".cc", ".c", ".S", ".s")):
                        src_arg = a
                if src_arg and out_arg and host_path(out_arg).is_file() \
                        and host_path(src_arg).is_file() \
                        and host_path(out_arg).stat().st_mtime >= \
                            host_path(src_arg).stat().st_mtime:
                    skipped += 1
                    continue
                stage = 0
            elif argv[0].endswith("llvm-ar"):
                stage = 1
            else:
                stage = 2
            stages[stage].append(argv)

    for argv in stages[0]:
        for i, a in enumerate(argv[:-1]):
            if a == "-o":
                host_path(argv[i + 1]).parent.mkdir(parents=True, exist_ok=True)
    # A fourth stage: use the tablegen we just built to generate a header,
    # so the manifest proves the tool works and not only that it links.
    def tg(a: str) -> str:
        return a.replace(str(build), GB).replace(str(src), GS)

    gen_stage = []
    for line in text.splitlines():
        if "-gen-vt" not in line:
            continue
        for part in line.split(" && "):
            if "llvm-tblgen" not in part:
                continue
            argv = shlex.split(part)
            if not argv or not argv[0].endswith("llvm-tblgen"):
                continue
            cmd = [GUEST + "/bin/llvm-tblgen"]
            i = 1
            while i < len(argv):
                a = argv[i]
                if a == "--write-if-changed":
                    i += 1
                    continue
                if a == "-d":
                    i += 2
                    continue
                if a == "-o":
                    cmd.extend(["-o", GUEST + "/gen/" + argv[i + 1]])
                    i += 2
                    continue
                cmd.append(tg(a))
                i += 1
            gen_stage.append(cmd)
        break

    for out in asm_objs:
        if not out.startswith(GUEST + "/"):
            continue
        rel = out[len(GUEST) + 1:]
        s = build / rel
        d = REPO / "build" / "llvm-guest" / rel
        d.parent.mkdir(parents=True, exist_ok=True)
        if s.is_file():
            shutil.copyfile(s, d)
        else:
            print(f"warning: no cross-built assembly object {s}")
    (REPO / "build" / "llvm-guest" / "lib").mkdir(parents=True, exist_ok=True)
    (REPO / "build" / "llvm-guest" / "bin").mkdir(parents=True, exist_ok=True)
    (REPO / "build" / "llvm-guest" / "gen").mkdir(parents=True, exist_ok=True)
    (REPO / "build" / "llvm-guest" / "tmp").mkdir(parents=True, exist_ok=True)

    if gen_stage:
        stages.append(gen_stage)

    out = REPO / "build" / "llvm-guest" / f"{target}.build"
    with out.open("w") as f:
        for n, stage in enumerate(stages):
            if n:
                f.write("---\n")
            for argv in stage:
                f.write(" ".join(argv) + "\n")

    print(f"{out}: {len(stages[0])} compiles, {len(stages[1])} archives, "
          f"{len(stages[2])} links, {len(gen_stage)} generate "
          f"({len(asm_objs)} prebuilt assembly objects, {skipped} up to date)")
    for a in stages[2]:
        for i, x in enumerate(a[:-1]):
            if x == "-o":
                print(f"  output: {a[i + 1]}")


if __name__ == "__main__":
    main()
