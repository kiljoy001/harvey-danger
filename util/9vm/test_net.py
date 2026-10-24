#!/usr/bin/env python3
"""Guest test for the user-space /net service.

The guest runs /build/netd, mounts its 9P tree at /net, and drives IL
announce/listen/accept and dial through the guest kernel's ordinary /net
interface.  No host socket backend is involved.
"""

import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
PRISM = os.path.join(ROOT, "util/9vm/prism.py")
BUILD_NETD = os.path.join(ROOT, "util/9vm/build_netd.py")
TEST_O9 = os.path.join(ROOT, "boot/test_netd.o9")
NETD = os.path.join(ROOT, "build/netd")

REQUIRED = [
    "o9-netd: mounted guest /net",
    "o9-netd: client dial OK",
    "o9-netd: server read/write OK",
    "o9-netd: client read/write OK",
    "o9-netd: done",
    "o9-netd: result 1",
]


def main():
    if not os.path.exists(NETD):
        subprocess.run([sys.executable, BUILD_NETD], check=True)
    start = time.perf_counter()
    res = subprocess.run(
        [sys.executable, PRISM, "run", TEST_O9, "-v", "-t", "30"],
        cwd=ROOT, capture_output=True, text=True, timeout=45)
    elapsed = time.perf_counter() - start
    combined = res.stdout + res.stderr

    failed = False
    for want in REQUIRED:
        ok = want in combined
        print(f"  [{'PASS' if ok else 'FAIL'}] {want}")
        failed |= not ok
    if res.returncode != 0:
        print(f"  [FAIL] VM exit code {res.returncode}")
        failed = True
    if failed:
        print("\n--- guest output ---\n" + combined)
        raise SystemExit(1)
    print(f"guest user-space /net OK ({elapsed:.2f}s)")


if __name__ == "__main__":
    main()
