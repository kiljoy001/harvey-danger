#!/usr/bin/env python3
"""
test_net.py - Test suite for Microkernel Networking (VirtIO-Net + #I IP Stack).
Verifies ethernet driver, IP stack configuration, and TCP/UDP endpoints under KVM.
"""

import os
import subprocess
import sys
import time

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
PRISM_BIN = os.path.join(REPO_ROOT, "util/9vm/prism.py")
TEST_O9 = os.path.join(REPO_ROOT, "boot/test_net.o9")

REQUIRED_OUTPUTS = [
    "#9/1: virtio9p tag \"net\"",
    "[NetActor] Initializing network namespace...",
    "o9-net: mounted #9/1 -> /net",
    "[NetActor] Verifying VirtIO ethernet adapter (#l0 / /net/ether0)...",
    "addr: 525400123456",
    "[NetActor] VirtIO-Net hardware online and verified.",
    "[NetActor] Configuring IP interface with 10.0.2.15...",
    "o9-net: allocated ipifc conv 0",
    "o9-net: bound ether /net/ether0 to ipifc",
    "o9-net: configured IP 10.0.2.15 mask 255.255.255.0 gw 10.0.2.0",
    "[NetActor] IP interface 10.0.2.15 bound and configured.",
    "[NetActor] Verifying protocol endpoints (TCP, UDP, and IL)...",
    "o9-net: proto /net/tcp/clone allocated conv 0",
    "o9-net: proto /net/udp/clone allocated conv 0",
    "o9-net: proto /net/il/clone allocated conv 0",
    "[NetActor] TCP, UDP, and IL stacks online and operational.",
    "[NetActor] Testing Objective-9C native dial keywords...",
    "o9-dial: dialed udp -> udp!127.0.0.1!9000",
    "o9-dial: dialed il -> il!10.0.2.2!9999",
    "[NetActor] Native dial keywords verified.",
    "Network Self-Test Complete",
    "[init] PID 1 exited cleanly",
]

def main():
    print("[test_net] Starting microkernel network self-test...")
    cmd = [PRISM_BIN, "run", TEST_O9, "-v"]

    t0 = time.perf_counter()
    res = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True, timeout=15)
    elapsed = time.perf_counter() - t0

    print(f"[test_net] Finished in {elapsed:.3f} s (Exit: {res.returncode})")

    combined = res.stdout + res.stderr
    all_passed = True
    print("\n--- Network Assertions ---")
    for req in REQUIRED_OUTPUTS:
        passed = req in combined
        status = "PASS" if passed else "FAIL"
        print(f"  [{status}] '{req}'")
        if not passed:
            all_passed = False

    if res.returncode != 0:
        print(f"[test_net] Non-zero exit code: {res.returncode}")
        all_passed = False

    if not all_passed:
        print("\nFull Output:\n" + combined)
        print("\n[test_net] FAILED")
        sys.exit(1)

    print(f"\n[test_net] SUCCESS: All network invariants verified (time={elapsed:.3f}s)")
    sys.exit(0)

if __name__ == "__main__":
    main()
