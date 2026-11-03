#!/usr/bin/env python3
"""Differential execution: the same programs built by both compilers must
print the same thing.

test-diff.py compares object files, which tests codegen.  This compares what
the programs *do*, which tests the runtime behind them: libc++, the
exception unwinder, stdio, filesystem, threads and libm.

Each source is compiled twice at -O2 -- once by the host reference clang
from this checkout (and run on the host), once by the guest clang inside
Harvey (and run there) -- and the two outputs are compared byte for byte.

    util/clang-harvey/test-diffrun.py
    util/clang-harvey/test-diffrun.py /tmp/opencode/llvm-host/bin/clang++

Add a program by dropping a .cpp in util/clang-harvey/difftests/.
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
GUEST = REPO / "build" / "llvm-guest"
OUT = REPO / "build" / "diffexec"
VM = REPO / "util" / "9vm" / "9vm"
KERNEL = REPO / "build" / "harvey.32bit"
HOST_CLANG = Path("/tmp/opencode/llvm-host/bin/clang++")
LLVM_BUILD = Path("/tmp/opencode/llvm-thr")
GUEST_CLANG = "/n/host/build/clang-harvey/clang++"
FLAGS = ["-std=c++17", "-O2"]

# Programs whose output is floating point.  APEX's libm is the Plan 9
# implementation (Hart and Cheney coefficients), not a correctly-rounded one
# like glibc's, so the last bits legitimately differ.  These are compared
# numerically with a tolerance instead of byte for byte.
APPROX = {"math"}
TOLERANCE = 1e-12

NUMBER = re.compile(r"[-+]?(?:0[xX][0-9a-fA-F.]+(?:[pP][-+]?\d+)?|"
                    r"\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?)")


def close_enough(a: str, b: str) -> bool:
    """True if the lines differ only in floating-point last bits."""
    na, nb = NUMBER.findall(a), NUMBER.findall(b)
    if len(na) != len(nb):
        return False
    if NUMBER.sub("#", a) != NUMBER.sub("#", b):
        return False
    for x, y in zip(na, nb):
        try:
            fx = float.fromhex(x) if "x" in x.lower() else float(x)
            fy = float.fromhex(y) if "y" in y.lower() else float(y)
        except ValueError:
            if x != y:
                return False
            continue
        if fx == fy:
            continue
        if abs(fx - fy) > TOLERANCE * max(abs(fx), abs(fy), 1e-300):
            return False
    return True


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


def main():
    host = Path(sys.argv[1]) if len(sys.argv) > 1 else HOST_CLANG
    if not host.is_file():
        sys.exit(f"missing host reference compiler {host}")

    subprocess.run([sys.executable, str(HERE / "install-sdk.py"), str(LLVM_BUILD)],
                   check=True, capture_output=True)

    progs = sorted(p.stem for p in (HERE / "difftests").glob("*.cpp"))
    if not progs:
        sys.exit("no programs in difftests/")

    for side in ("host", "guest", "tmp"):
        (OUT / side).mkdir(parents=True, exist_ok=True)

    print(f"+ building {len(progs)} programs with the host reference")
    for name in progs:
        run([str(host), *FLAGS, "-o", str(OUT / "host" / name),
             str(HERE / "difftests" / (name + ".cpp"))], capture_output=True)

    manifest = GUEST / "diffexec.build"
    manifest.write_text("\n".join(
        " ".join([GUEST_CLANG, *FLAGS,
                  "-o", f"/n/host/build/diffexec/guest/{name}",
                  f"/n/host/util/clang-harvey/difftests/{name}.cpp"])
        for name in progs) + "\n")
    print("+ building the same programs in the guest")

    shutil.rmtree(OUT / "guest", ignore_errors=True)
    (OUT / "guest").mkdir()
    (OUT / "progs.list").write_text("".join(f"guest/{p}\n" for p in progs))
    run([str(HERE / "harvey-clang"), str(HERE / "run-progs.c"),
         "-o", str(GUEST / "run-progs.elf")], capture_output=True)

    print("+ building and running the guest programs")
    proc = subprocess.run(
        [str(VM), "-m", "3584", "-boot", str(GUEST / "run-progs.elf"),
         str(KERNEL)],
        cwd=REPO, text=True, capture_output=True, timeout=7200)
    out = proc.stdout + proc.stderr
    for line in out.splitlines():
        if line.strip() and any(w in line for w in
                                ("hbuild:", "== run", "== build", "panic",
                                 "error:")):
            print("  " + line)
    if proc.returncode != 0 or "hbuild: FAILED" in out:
        sys.exit("the guest build or run failed")

    print("+ running the host programs")
    for name in progs:
        p = subprocess.run([str(OUT / "host" / name)], cwd=OUT / "tmp",
                           capture_output=True, text=True,
                           env={"TMPDIR": str(OUT / "tmp"), "PATH": "/usr/bin:/bin"})
        (OUT / "host" / (name + ".out")).write_text(p.stdout)
        (OUT / "host" / (name + ".err")).write_text(p.stderr)
        if p.returncode != 0:
            print(f"  host {name}: exit {p.returncode}\n{p.stderr[-2000:]}")

    bad = 0
    for name in progs:
        h = OUT / "host" / (name + ".out")
        g = OUT / "guest" / (name + ".out")
        if not g.is_file():
            print(f"  MISSING {name}: the guest program produced no output")
            bad += 1
            continue
        hb, gb = h.read_text(), g.read_text()
        if hb == gb:
            continue
        if name in APPROX:
            hl, gl = hb.splitlines(), gb.splitlines()
            if len(hl) == len(gl) and all(close_enough(a, b)
                                          for a, b in zip(hl, gl)):
                print(f"  approx {name}: same to {TOLERANCE:g} relative")
                continue
        if hb != gb:
            print(f"  DIFF {name}:")
            hl, gl = hb.splitlines(), gb.splitlines()
            for n, (a, b) in enumerate(zip(hl, gl)):
                if a != b:
                    print(f"    line {n + 1}:\n      host : {a}\n      guest: {b}")
            if len(hl) != len(gl):
                print(f"    host {len(hl)} lines, guest {len(gl)} lines")
            bad += 1
    if bad:
        sys.exit(f"differential run: {bad}/{len(progs)} programs differ")
    print(f"differential run: {len(progs)} programs print the same thing on "
          f"the host and on Harvey")


if __name__ == "__main__":
    main()
