#!/usr/bin/env python3
"""Compare cached 9P reads through 9vm's shared ring and VirtIO paths."""

import argparse
import os
import re
import selectors
import statistics
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VM = ROOT / "util/9vm/9vm"
KERNEL = ROOT / "build/harvey.32bit"
CMDLINE = "bootargs=local!#9/0 nobootprompt=local O9_HOSTFS_BENCH=1"
GUEST = re.compile(
    r"o9-bench: kind=(\d+) count=(\d+) bytes=(\d+) elapsed_ns=(\d+) "
    r"read_mean_ns=(\d+) bins=([\d,]+)"
)
HOST = re.compile(
    r"\[9vm-bench\] kind=(\d+) wall_ns=(\d+) cpu_ns=(\d+) "
    r"io_exits=(\d+) mmio_exits=(\d+) virtio_host_kicks=(\d+) "
    r"ring_requests=(\d+)"
)
BIN_LIMIT_US = (10, 25, 50, 100, 250, 500, 1000, float("inf"))


def percentile_bound(bins, fraction):
    target = sum(bins) * fraction
    seen = 0
    for count, limit in zip(bins, BIN_LIMIT_US):
        seen += count
        if seen >= target:
            return limit
    raise ValueError("empty latency histogram")


def sample(mode, timeout):
    cmd = [str(VM)]
    if mode == "virtio":
        cmd.append("-no9ring")
    cmd += ["-c", CMDLINE, str(KERNEL)]
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    output = bytearray()
    deadline = time.monotonic() + timeout
    try:
        while time.monotonic() < deadline:
            if not sel.select(timeout=0.25):
                if proc.poll() is not None:
                    break
                continue
            chunk = os.read(proc.stdout.fileno(), 65536)
            if not chunk:
                break
            output.extend(chunk)
            if b"o9-bench: done" in output:
                break
    finally:
        sel.close()
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        output.extend(proc.stdout.read())
        proc.stdout.close()

    log = output.decode(errors="replace")
    if "panic:" in log or "o9-bench: FAIL" in log:
        raise RuntimeError(f"{mode} failed:\n{log[-3000:]}")
    if "o9-bench: done" not in log:
        raise TimeoutError(f"{mode} benchmark did not finish:\n{log[-3000:]}")
    guest = {int(m[1]): m for m in GUEST.finditer(log)}
    host = {int(m[1]): m for m in HOST.finditer(log)}
    if set(guest) != {0, 1, 2, 3} or set(host) != {0, 1, 2, 3}:
        raise RuntimeError(f"{mode} missing metrics:\n{log[-3000:]}")
    for kind, count in ((0, 2000), (1, 500), (2, 50), (3, 500)):
        if int(guest[kind][2]) != count:
            raise RuntimeError(f"{mode} kind {kind}: wrong operation count")
        if mode == "ring" and int(host[kind][7]) < count:
            raise RuntimeError(f"{mode} kind {kind}: missing ring requests")
        if mode == "virtio" and int(host[kind][6]) < count:
            raise RuntimeError(f"{mode} kind {kind}: missing VirtIO kicks")
    return guest, host


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-n", "--repetitions", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error("repetitions must be positive")
    if len(os.sched_getaffinity(0)) < 2:
        parser.error("ring benchmark requires two available CPUs; 9vm uses VirtIO on one")
    results = {(mode, kind): [] for mode in ("ring", "virtio")
               for kind in (0, 1, 2, 3)}
    for rep in range(args.repetitions):
        for mode in (("ring", "virtio") if rep % 2 == 0
                     else ("virtio", "ring")):
            guest, host = sample(mode, args.timeout)
            for kind in (0, 1, 2, 3):
                row = {
                    "mean_us": int(guest[kind][5]) / 1000,
                    "p95_bound_us": percentile_bound(
                        [int(v) for v in guest[kind][6].split(",")], 0.95),
                    "wall_ms": int(host[kind][2]) / 1e6,
                    "ops_s": int(guest[kind][2]) * 1e9 /
                             int(host[kind][2]),
                    "cpu_ms": int(host[kind][3]) / 1e6,
                    "io_exits": int(host[kind][4]),
                    "mmio_exits": int(host[kind][5]),
                    "kicks": int(host[kind][6]),
                    "ring_req": int(host[kind][7]),
                }
                results[mode, kind].append(row)
                print(f"run={rep+1} mode={mode} kind={kind} {row}", flush=True)
    print("\nMedian of runs (kind 0: 128-byte reads x2000; "
          "kind 1: 16-KiB reads x500; "
          "kind 2: 128-byte reads x50 with 10-ms gaps; "
          "kind 3: open/close x500)")
    print("mode    kind  mean_us  p95_lt_us  ops_s  cpu_cores  mmio_exits  "
          "virtio_kicks  ring_requests")
    for mode in ("ring", "virtio"):
        for kind in (0, 1, 2, 3):
            rows = results[mode, kind]
            med = {key: statistics.median(row[key] for row in rows)
                   for key in rows[0]}
            print(f"{mode:6s}  {kind:4d}  {med['mean_us']:7.2f}  "
                  f"{med['p95_bound_us']:9.0f}  {med['ops_s']:5.0f}  "
                  f"{med['cpu_ms']/med['wall_ms']:9.2f}  "
                  f"{med['mmio_exits']:10.0f}  {med['kicks']:12.0f}  "
                  f"{med['ring_req']:13.0f}")


if __name__ == "__main__":
    main()
