#!/usr/bin/env python3
"""Differential compile test: host reference clang vs the guest clang.

Both compilers are built from the same checkout and target the same triple,
so compiling the same source with the same flags must produce the same
object file.  Any difference points at a codegen, ABI or platform-layer bug
in the guest compiler.

The reference is /tmp/opencode/llvm-host/bin/clang++, a *host* build of this
checkout.  The system compiler is a different revision and is not a valid
oracle.

The flags are not guessed: they are taken from the real ninja graph of the
Harvey-target build, with the output path and dependency file removed, and
the path roots normalised with -ffile-prefix-map so that only the compiler
differs between the two sides.  -frandom-seed is pinned because its default
is derived from the output file name.

    util/clang-harvey/test-diff.py
    util/clang-harvey/test-diff.py /tmp/opencode/llvm-host/bin/clang++

The first TU is compiled twice on each side as a determinism check.
"""
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
GUEST = REPO / "build" / "llvm-guest"
DIFF = REPO / "build" / "diff"
VM = REPO / "util" / "9vm" / "9vm"
KERNEL = REPO / "build" / "harvey.32bit"
LLVM_BUILD = Path("/tmp/opencode/llvm-thr")
SRC = Path("/home/scott/Repo/llvm-project")
HOST_CLANG = Path("/tmp/opencode/llvm-host/bin/clang++")
GUEST_CLANG = "/n/host/build/clang-harvey/clang++"

CORPUS = [
    "llvm/lib/Support/StringRef.cpp",
    "llvm/lib/Support/SmallVector.cpp",
    "llvm/lib/Support/Twine.cpp",
    "llvm/lib/Support/APInt.cpp",
    "llvm/lib/Support/APFloat.cpp",
    "llvm/lib/Support/CommandLine.cpp",
    "llvm/lib/Support/MemoryBuffer.cpp",
    "llvm/lib/Support/StringMap.cpp",
    "llvm/lib/Support/Error.cpp",
    "llvm/lib/IR/Attributes.cpp",
    "llvm/lib/IR/Module.cpp",
    "llvm/lib/IR/Instructions.cpp",
]


def obj_for(rel: str) -> str:
    """The ninja object target for a source file."""
    parts = rel.split("/")
    lib = parts[2]
    if lib == "Support":
        return f"lib/Support/CMakeFiles/LLVMSupport.dir/{parts[-1]}.o"
    if lib == "IR":
        return f"lib/IR/CMakeFiles/LLVMCore.dir/{parts[-1]}.o"
    raise SystemExit(f"no object mapping for {rel}")


def ninja_tokens(rel: str) -> list[str]:
    out = subprocess.run(["ninja", "-t", "commands", obj_for(rel)],
                         cwd=LLVM_BUILD, capture_output=True, text=True,
                         check=True).stdout
    cmd = out.strip().splitlines()[-1]
    toks = shlex.split(cmd)
    # Drop the compiler (replaced per side) and the output/dep side effects.
    toks = toks[1:]
    kept, i = [], 0
    while i < len(toks):
        t = toks[i]
        if t in ("-o", "-MF", "-MT"):
            i += 2
            continue
        if t == "-MD":
            i += 1
            continue
        kept.append(t)
        i += 1
    return kept


def cfg_flags() -> list[str]:
    """The compile-time flags the guest driver config adds."""
    skip = ("-fuse-ld=lld", "-nostdlib", "-static", "-no-pie")
    out = []
    for line in (HERE / "sdk" / "clang++.cfg").read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("$"):
            continue
        if line in skip or line.startswith("-Wl,"):
            continue
        out.append(line)
    return out


def to_guest(path: str) -> str:
    for root, guest in ((str(LLVM_BUILD), "/n/host/build/llvm-build"),
                        (str(SRC), "/n/host/build/llvm-src"),
                        (str(REPO), "/n/host")):
        if path == root:
            return guest
        if path.startswith(root + "/"):
            return guest + path[len(root):]
    return path


def to_host(path: str) -> str:
    if path == "/n/host":
        return str(REPO)
    if path.startswith("/n/host/"):
        return str(REPO) + path[len("/n/host"):]
    return path


def maparg(tok: str, fn) -> str:
    """Rewrite a whole argument or just the path in -I<dir>."""
    if tok.startswith("-I") and len(tok) > 2:
        return "-I" + fn(tok[2:])
    return fn(tok)


def main():
    host = Path(sys.argv[1]) if len(sys.argv) > 1 else HOST_CLANG
    if not host.is_file():
        sys.exit(f"missing host reference compiler {host}")

    subprocess.run([sys.executable, str(HERE / "install-sdk.py"), str(LLVM_BUILD)],
                   check=True, capture_output=True)
    for link, target in ((REPO / "build" / "llvm-src", SRC),
                         (REPO / "build" / "llvm-build", LLVM_BUILD)):
        link.unlink(missing_ok=True)
        link.symlink_to(target)

    GUEST.mkdir(parents=True, exist_ok=True)
    (GUEST / "tmp").mkdir(exist_ok=True)
    shutil.rmtree(DIFF, ignore_errors=True)
    (DIFF / "host").mkdir(parents=True)
    (DIFF / "guest").mkdir(parents=True)

    prefixes = [
        (str(LLVM_BUILD), "/build"),
        (str(SRC), "/src"),
        (str(REPO), "/host"),
    ]
    host_prefixes = [f"-ffile-prefix-map={a}/={b}/" for a, b in prefixes]
    guest_prefixes = [
        f"-ffile-prefix-map={to_guest(a)}/={b}/" for a, b in prefixes
    ]
    host_cfg = [maparg(t, to_host) for t in cfg_flags()]
    seed = ["-frandom-seed=harvey-diff"]

    jobs = []
    for n, rel in enumerate(CORPUS):
        toks = ninja_tokens(rel)
        for suffix in (".o", ".again.o") if n == 0 else (".o",):
            name = rel.replace("/", "_") + suffix
            out_h = DIFF / "host" / name
            out_g = DIFF / "guest" / name
            jobs.append((
                [str(host), "--no-default-config", *host_cfg, *seed,
                 *host_prefixes, *toks, "-o", str(out_h)],
                [GUEST_CLANG, *seed, *guest_prefixes,
                 *[maparg(t, to_guest) for t in toks],
                 "-o", to_guest(str(out_g))],
                out_h, out_g, rel,
            ))

    print(f"+ compiling {len(jobs)} objects with the host reference")
    for hcmd, _, _, _, rel in jobs:
        r = subprocess.run(hcmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr[-4000:])
            sys.exit(f"host reference failed on {rel}")

    manifest = GUEST / "diff.build"
    manifest.write_text("\n".join(" ".join(j[1]) for j in jobs) + "\n")
    print(f"+ compiling the same objects in the guest ({manifest})")
    subprocess.run([str(HERE / "harvey-clang"), str(HERE / "run-diff.c"),
                    "-o", str(GUEST / "run-diff.elf")], check=True)
    proc = subprocess.run(
        [str(VM), "-m", "3584", "-boot", str(GUEST / "run-diff.elf"),
         str(KERNEL)],
        cwd=REPO, text=True, capture_output=True, timeout=7200)
    out = proc.stdout + proc.stderr
    shown = 0
    for line in out.splitlines():
        if line.strip() and any(w in line for w in
                                ("hbuild:", "panic", "suicide", "error:")):
            if "error:" in line and shown >= 5:
                continue
            print("  " + line)
            shown += 1
    if proc.returncode != 0 or "hbuild: FAILED" in out:
        sys.exit("the guest compile failed")

    bad = 0
    for _, _, h, g, rel in jobs:
        if not h.is_file() or not g.is_file():
            print(f"  MISSING {rel}")
            bad += 1
            continue
        hb, gb = h.read_bytes(), g.read_bytes()
        if hb != gb:
            first = next((i for i, (a, b) in enumerate(zip(hb, gb))
                          if a != b), min(len(hb), len(gb)))
            print(f"  DIFF {rel}: host {len(hb)} guest {len(gb)} bytes, "
                  f"first at {first}")
            bad += 1
    if bad:
        sys.exit(f"differential test: {bad} mismatch(es)")

    print(f"differential test: {len(jobs)} objects compile identically on the "
          f"host reference and on Harvey ({len(CORPUS)} translation units, "
          f"one built twice on each side)")


if __name__ == "__main__":
    main()
