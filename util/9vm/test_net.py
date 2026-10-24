#!/usr/bin/env python3
"""Guest test for the in-kernel Plan 9 network stack."""
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
PRISM = os.path.join(ROOT, "util/9vm/prism.py")
TEST_O9 = os.path.join(ROOT, "boot/test_kernel_net.o9")
REQUIRED = [
    "o9-knet: loopback 127.0.0.1/8 up",
    "o9-knet: server got <ping>",
    "o9-knet: client got <pong>",
    "o9-knet: IL loopback PASS",
    "o9-knet: result 1",
]

def main():
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
    print(f"in-kernel #I IL loopback OK ({elapsed:.2f}s)")

if __name__ == "__main__":
    main()
