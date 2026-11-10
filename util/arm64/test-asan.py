#!/usr/bin/env python3
"""Exercise the Harvey ARM64 AddressSanitizer runtime.

Builds a small set of instrumented programs and runs each in QEMU:

  heap    store past a malloc'd object        -> heap-buffer-overflow
  uaf     store through a freed pointer       -> heap-use-after-free
  global  store past a global array           -> global-buffer-overflow
  ok      valid heap use                      -> ASAN-OK, no report
  cpp     std::vector out-of-bounds store     -> heap-buffer-overflow

There is deliberately no stack/wild-pointer case: the kernel turns an
unhandled user fault into pexit("user fault"), so the runtime does not
install a SIGSEGV handler.  Instrumented redzone and UAF checks do not
need it.

    python3 util/arm64/test-asan.py
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
SDK_RUNTIME = ROOT / "build" / "arm64-sdk" / "lib" / "clang" / "22"

CASES = {
    "heap": (False, "heap-buffer-overflow", r'''
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    char *p = malloc(4);
    p[4] = 1;
    printf("ASAN-AFTER\n");
    return 0;
}
'''),
    "uaf": (False, "heap-use-after-free", r'''
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    char *p = malloc(8);
    free(p);
    p[0] = 1;
    printf("ASAN-AFTER\n");
    return 0;
}
'''),
    "global": (False, "global-buffer-overflow", r'''
#include <stdio.h>
int g[2];
int main(void) {
    g[2] = 1;
    printf("ASAN-AFTER\n");
    return 0;
}
'''),
    "ok": (False, "ASAN-OK", r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    char *p = malloc(8);
    p[0] = 1;
    p[7] = 2;
    memcpy(p, "abc", 3);
    free(p);
    printf("ASAN-OK\n");
    return 0;
}
'''),
    "cpp": (True, "heap-buffer-overflow", r'''
#include <vector>
int main() {
    std::vector<int> v(4);
    v[4] = 1;
    return 0;
}
'''),
}


def run_case(name, cxx, marker, source, stage, mem, smp, deadline):
    case = stage / name
    case.mkdir(parents=True, exist_ok=True)
    src = case / ("case.cpp" if cxx else "case.c")
    src.write_text(source)
    binary = case / "prog"
    driver = HERE / ("aarch64-clang++" if cxx else "aarch64-clang")
    subprocess.run([
        str(driver), "-fsanitize=address",
        f"-resource-dir={SDK_RUNTIME}", str(src), "-o", str(binary),
    ], check=True)
    subprocess.run(["llvm-strip", "--strip-all", str(binary)], check=True)

    inner = case / "test.tar"
    with tarfile.open(inner, "w", format=tarfile.USTAR_FORMAT) as tar:
        tar.add(str(binary), arcname="prog")
    data = inner.read_bytes()
    boot = case / "case.boot"
    with boot.open("wb") as f:
        f.write(struct.pack("<8sIIII", b"O9BOOT01", 1, 1, 24 + 48 + len(data), 0))
        f.write(struct.pack("<32sIIII", b"test.tar", 24 + 48, len(data), 0o555, 0))
        f.write(data)

    runner = case / "runner.elf"
    subprocess.run([
        sys.executable, str(HERE / "mkrun.py"), str(binary),
        "--in-tree", "/initramfs/prog",
        "--untar", "/boot/test.tar:/initramfs",
        "--suffix", "", "--output", str(runner),
    ], check=True)

    dump = case / "log"
    subprocess.run([
        sys.executable, str(HERE / "test_apex_boot.py"),
        "--elf", str(runner), "--archive", str(boot),
        "--mem", str(mem), "--smp", str(smp),
        "--marker", marker, "--allow-missing",
        "--deadline", str(deadline), "--dump", str(dump),
    ], check=False)
    text = dump.read_text(errors="replace")
    ok = marker in text
    if name == "ok":
        ok = ok and "AddressSanitizer" not in text
    else:
        ok = ok and "AddressSanitizer:" in text
    print(f"{name:8s} {'PASS' if ok else 'FAIL'}  {marker}")
    if not ok:
        for line in text.splitlines():
            if "AddressSanitizer" in line or "ASAN" in line:
                print("   ", line)
    return ok


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stage", default="/tmp/opencode/asan-cases")
    ap.add_argument("--mem", type=int, default=4096)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--deadline", type=int, default=300)
    args = ap.parse_args()

    stage = Path(args.stage)
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True)
    results = {}
    for name, (cxx, marker, source) in CASES.items():
        results[name] = run_case(name, cxx, marker, source, stage,
                                 args.mem, args.smp, args.deadline)
    if not all(results.values()):
        sys.exit("ASan cases failed: " +
                 ", ".join(n for n, ok in results.items() if not ok))
    print("ASan matrix PASS")


if __name__ == "__main__":
    main()
