#!/usr/bin/env python3
"""
test_serverless.py - Test suite for Serverless Plan 9 execution under KVM.
Verifies ephemeral execution of arbitrary Objective-9C code on bare metal.
"""

import os
import subprocess
import sys
import time

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
PRISM_BIN = os.path.join(REPO_ROOT, "util/9vm/prism.py")
TEST_O9 = os.path.join(REPO_ROOT, "boot/test_serverless.o9")

REQUIRED_OUTPUTS = [
    "[Serverless Plan 9] Hello from ephemeral KVM microkernel!",
    "[Serverless Plan 9] Computing fibonacci(10)...",
    "[Serverless Plan 9] Result: fib(10) = 55",
    "[Serverless Plan 9] Workload complete. Exiting.",
    "[init] PID 1 exited cleanly",
]

def main():
    print("[test_serverless] Starting serverless execution test...")
    cmd = [PRISM_BIN, "run", TEST_O9, "-v"]

    t0 = time.perf_counter()
    res = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True, timeout=10)
    elapsed = time.perf_counter() - t0

    print(f"[test_serverless] Finished in {elapsed:.3f} s (Exit: {res.returncode})")

    combined = res.stdout + res.stderr
    all_passed = True
    print("\n--- Assertions ---")
    for req in REQUIRED_OUTPUTS:
        passed = req in combined
        status = "PASS" if passed else "FAIL"
        print(f"  [{status}] '{req}'")
        if not passed:
            all_passed = False

    if res.returncode != 0:
        print(f"[test_serverless] Non-zero exit code: {res.returncode}")
        all_passed = False

    if not all_passed:
        print("\nFull Output:\n" + combined)
        print("\n[test_serverless] FAILED")
        sys.exit(1)

    print(f"\n[test_serverless] SUCCESS: All invariants verified (time={elapsed:.3f}s)")
    sys.exit(0)

if __name__ == "__main__":
    main()
