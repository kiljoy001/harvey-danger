#!/usr/bin/env python3
"""
External Test Harness for Prism Microkernel & Objective-9C PID 1.
Validates boot behavior, namespace bindings, actor execution, and timing externally.
"""
import os
import sys
import time
import subprocess
import re
import queue
import threading

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
VM_BIN = os.path.join(REPO_ROOT, "util/9vm/9vm")
KERNEL_BIN = os.path.join(REPO_ROOT, "build/harvey.32bit")

REQUIRED_ASSERTIONS = [
    ("kernel_banner", "Harvey"),
    ("virtio9p_probe", "#9/0: virtio9p tag \"host\""),
    ("virtio9p_net_probe", "#9/1: virtio9p tag \"net\""),
    ("pid1_banner", "Prism Microkernel (Harvey amd64)"),
    ("pid1_supervisor", "PID 1 Objective-9C Actor Supervisor Online"),
    ("ns_bind_dev", "o9-boot: bound #c -> /dev"),
    ("ns_bind_host", "o9-boot: bound / -> /n/host"),
    ("bio_read", "o9-bio: Brdline read"),
    ("hostfs_rw", "o9-hostfs-rw: PASS 100000-byte write/read"),
    ("local_9ring", "o9-local9ring: PASS two concurrent 9P mounts and reads"),
    ("actor_worker", "o9-worker[1]: executed job 101"),
    ("actor_results", "o9-worker: job results r1=1000 r2=1200"),
    ("actor_heartbeat", "o9-supervisor: heartbeat tick 1"),
    ("async_task_spawn", "o9-async: worker task 7 running concurrently"),
    ("async_task_await", "o9-async: awaited result = 7042"),
    ("steady_state", "o9-supervisor: steady-state loop active"),
]

def run_test(use_ring):
    if not os.path.exists(VM_BIN):
        print(f"Error: {VM_BIN} not found", file=sys.stderr)
        return 1
    if not os.path.exists(KERNEL_BIN):
        print(f"Error: {KERNEL_BIN} not found", file=sys.stderr)
        return 1

    cmd = [VM_BIN] + ([] if use_ring else ["-no9ring"]) + [
        "-c", "bootargs=local!#9/0 nobootprompt=local O9_HOSTFS_RW_TEST=1 O9_RING_IPC_TEST=1",
        KERNEL_BIN,
    ]
    assertions = REQUIRED_ASSERTIONS + [
        ("init_mount", "init: mounting #~/host to /..." if use_ring
         else "init: mounting #9/0 to /...")
    ]
    print(f"[test] Launching microkernel under test: {' '.join(cmd)}")

    start_time = time.perf_counter()
    proc = subprocess.Popen(
        cmd,
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1
    )

    lines = queue.Queue()
    def read_output():
        for output_line in proc.stdout:
            lines.put(output_line)
        lines.put(None)
    threading.Thread(target=read_output, daemon=True).start()

    seen = {k: False for k, _ in assertions}
    first_kernel_ts = None
    pid1_ts = None
    steady_state_ts = None
    all_output = []

    deadline = time.perf_counter() + 5.0

    while True:
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            proc.terminate()
            break
        try:
            line = lines.get(timeout=remaining)
        except queue.Empty:
            proc.terminate()
            break
        if line is None:
            break

        now = time.perf_counter() - start_time
        all_output.append(line.rstrip())

        for key, pattern in assertions:
            if not seen[key] and pattern in line:
                seen[key] = True
                if key == "kernel_banner" and first_kernel_ts is None:
                    first_kernel_ts = now
                elif key == "pid1_banner" and pid1_ts is None:
                    pid1_ts = now
                elif key == "steady_state" and steady_state_ts is None:
                    steady_state_ts = now

        if seen["steady_state"]:
            # Reached steady state, clean shutdown
            proc.terminate()
            break

    try:
        proc.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        proc.kill()

    total_elapsed = time.perf_counter() - start_time
    stderr = proc.stderr.read()

    # Report results
    print("\n--- Test Assertions ---")
    all_passed = True
    for key, pattern in assertions:
        status = "PASS" if seen[key] else "FAIL"
        if not seen[key]:
            all_passed = False
        print(f"  [{status}] {key:20s}: expected '{pattern}'")
    if any("panic:" in line for line in all_output) or "panic:" in stderr:
        all_passed = False
        print("  [FAIL] kernel panic")
    if use_ring:
        match = re.search(r"Shared host 9ring completed (\d+) request", stderr)
        if not match or int(match.group(1)) == 0:
            all_passed = False
            print("  [FAIL] no host ring completions recorded")
    elif "Shared host 9ring" in stderr:
        all_passed = False
        print("  [FAIL] host ring started during VirtIO fallback test")

    print("\n--- Empirical Performance Timings ---")
    if first_kernel_ts:
        print(f"  Kernel Entry:       {first_kernel_ts*1000:6.1f} ms")
    if pid1_ts:
        print(f"  PID 1 Init:         {pid1_ts*1000:6.1f} ms")
    if steady_state_ts:
        print(f"  Steady-State Ready: {steady_state_ts*1000:6.1f} ms")
    print(f"  Total Test Elapsed: {total_elapsed:6.2f} s")

    if all_passed:
        print("\n[test] RESULT: ALL INVARIANTS VERIFIED EXTERNALLY (Exit 0)")
        return 0
    else:
        print("\n[test] RESULT: FAILED - Missing required assertions (Exit 1)", file=sys.stderr)
        return 1

if __name__ == "__main__":
    if len(os.sched_getaffinity(0)) < 2:
        print("[test] Ring case skipped: fewer than two CPUs available")
        sys.exit(run_test(False))
    sys.exit(run_test(True) | run_test(False))
