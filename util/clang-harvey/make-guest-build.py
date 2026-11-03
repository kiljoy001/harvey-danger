#!/usr/bin/env python3
"""Turn a host LLVM ninja target into an hbuild manifest for the guest.

The cross build's own command graph is the source of truth for flags and
file lists.  This script rewrites the tool and every path from the host to
the guest's /n/host tree, drops the shell-isms (`cmake -E rm ... &&`), and
writes the commands as stages that hbuild runs in order:

    [generated headers] -> compiles -> archives -> link

Outputs go to build/llvm-guest, so the host build tree is left alone.

By default the only generation stage is the `-gen-vt` check after building
llvm-tblgen.  With --overlay the generated headers are mirrored into
build/llvm-guest and *all* llvm-tblgen commands are run there with the
guest-built tool, before the compiles, so a library can be built against
guest-generated headers.

    util/clang-harvey/make-guest-build.py [build-dir] [target] [source-dir]
    util/clang-harvey/make-guest-build.py --overlay /tmp/opencode/llvm-thr LLVMCore
"""
import os
import re
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

# Files worth mirroring from the build tree into the guest overlay.
HEADER_EXTS = {".h", ".inc", ".def", ".td", ".gen", ".hpp", ".modulemap"}


def convert(part: str, build: Path, src: Path, overlay: bool) -> list[str] | None:
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
        return None

    bg = GUEST if overlay else GB

    def to_guest(a: str) -> str:
        return a.replace(str(build), bg).replace(str(src), GS)

    def out_path(a: str) -> str:
        a = to_guest(a)
        if a.startswith(bg + "/"):
            return GUEST + a[len(bg):]
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


def gen_commands(text: str, build: Path, src: Path, overlay: bool) -> list[list[str]]:
    """Rewrite the llvm-tblgen invocations for the guest tool."""
    cmds = []
    for line in text.splitlines():
        if "llvm-tblgen" not in line:
            continue
        m = re.match(r"\s*cd\s+(\S+)\s*&&", line)
        cddir = m.group(1) if m else None
        for part in line.split(" && "):
            if "llvm-tblgen" not in part:
                continue
            argv = shlex.split(part)
            if not argv or not argv[0].endswith("llvm-tblgen"):
                continue
            if not overlay and "-gen-vt" not in argv:
                continue

            def tg(a: str) -> str:
                if overlay:
                    return a.replace(str(build), GUEST).replace(str(src), GS)
                return a.replace(str(build), GB).replace(str(src), GS)

            cmd = [GUEST + "/bin/llvm-tblgen"]
            i = 1
            while i < len(argv):
                a = argv[i]
                if a == "-d":
                    i += 2
                    continue
                if a == "-o":
                    out = argv[i + 1]
                    if not out.startswith("/") and cddir:
                        rel = cddir
                        if rel.startswith(str(build)):
                            rel = rel[len(str(build)):].lstrip("/")
                        out = GUEST + "/" + rel + "/" + out
                    else:
                        out = tg(out)
                    cmd.extend(["-o", out])
                    i += 2
                    continue
                cmd.append(tg(a))
                i += 1
            cmds.append(cmd)
    return cmds


def mirror(build: Path, sub: str) -> int:
    """Copy the header-ish files under build/sub into the guest overlay."""
    src_dir = build / sub
    if not src_dir.is_dir():
        return 0
    dst_dir = REPO / "build" / "llvm-guest" / sub
    copy_all = sub == "include" or sub.startswith("include/")
    n = 0
    for root, _, files in os.walk(src_dir):
        rel = Path(root).relative_to(build)
        (REPO / "build" / "llvm-guest" / rel).mkdir(parents=True, exist_ok=True)
        for name in files:
            if not copy_all and Path(name).suffix not in HEADER_EXTS:
                continue
            s = Path(root) / name
            d = REPO / "build" / "llvm-guest" / rel / name
            if d.is_file() and d.stat().st_size == s.stat().st_size:
                continue
            shutil.copyfile(s, d)
            n += 1
    return n


def host_path(p: str) -> Path:
    if p.startswith("/n/host/"):
        return REPO / p[len("/n/host/"):]
    return Path(p)


def main():
    argv = [a for a in sys.argv[1:] if a != "--overlay"]
    overlay = len(argv) != len(sys.argv) - 1
    build = Path(argv[0]) if len(argv) > 0 else DEFAULT_BUILD
    target = argv[1] if len(argv) > 1 else "llvm-tblgen"
    src = Path(argv[2]) if len(argv) > 2 else DEFAULT_SRC

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
            argv2 = convert(part, build, src, overlay)
            if not argv2:
                continue
            # The guest's clang++.cfg force-includes compat.h, which cannot
            # be assembled.  The cross build already produced these objects
            # for the same target, so reuse them instead of recompiling.
            if "-c" in argv2 and any(a.endswith((".S", ".s"))
                                     for a in argv2 if not a.startswith("-")):
                if "-o" in argv2:
                    asm_objs.append(argv2[argv2.index("-o") + 1])
                continue
            if "-c" in argv2:
                # Rebuild only what is missing or older than its source.
                src_arg = out_arg = None
                for i, a in enumerate(argv2):
                    if a == "-o" and i + 1 < len(argv2):
                        out_arg = argv2[i + 1]
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
            elif argv2[0].endswith("llvm-ar"):
                stage = 1
            else:
                stage = 2
            stages[stage].append(argv2)

    for argv2 in stages[0]:
        for i, a in enumerate(argv2[:-1]):
            if a == "-o":
                host_path(argv2[i + 1]).parent.mkdir(parents=True, exist_ok=True)

    gen = gen_commands(text, build, src, overlay)

    # In overlay mode the build-tree headers (generated and configured) are
    # mirrored so the compiles can include them from the guest tree.
    mirrored = 0
    if overlay:
        subs = set()
        for m in re.finditer(r"(?:-I|cd\s+)" + re.escape(str(build)) + r"/([^\s]+)",
                             text):
            subs.add(m.group(1))
        for sub in sorted(subs):
            mirrored += mirror(build, sub)
        for d in ("include", "lib", "bin", "gen", "tmp"):
            (REPO / "build" / "llvm-guest" / d).mkdir(parents=True, exist_ok=True)

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

    for d in ("lib", "bin", "gen", "tmp"):
        (REPO / "build" / "llvm-guest" / d).mkdir(parents=True, exist_ok=True)

    all_stages = ([gen] if overlay else []) + stages
    if gen and not overlay:
        all_stages.append(gen)

    out = REPO / "build" / "llvm-guest" / f"{target}.build"
    with out.open("w") as f:
        for n, stage in enumerate(all_stages):
            if n:
                f.write("---\n")
            for argv2 in stage:
                f.write(" ".join(argv2) + "\n")

    print(f"{out}: {len(stages[0])} compiles, {len(stages[1])} archives, "
          f"{len(stages[2])} links, {len(gen)} generate "
          f"({len(asm_objs)} prebuilt assembly objects, {skipped} up to date, "
          f"{mirrored} mirrored)")
    if not overlay:
        for a in stages[2]:
            for i, x in enumerate(a[:-1]):
                if x == "-o":
                    print(f"  output: {a[i + 1]}")


if __name__ == "__main__":
    main()
