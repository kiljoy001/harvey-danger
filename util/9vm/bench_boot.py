#!/usr/bin/env python3
"""Measure Harvey microVM boot milestones and idle CPU on 9vm.

Boots an image created by install_gefs.py, timestamps the kernel,
host-ring, GEFS-root and rc-shell milestones from the serial log, then
samples the 9vm process CPU over an idle window.  The idle figure
includes the host ring thread, which polls for requests.

    ./install_gefs.py -o /tmp/9vm/gefs.img -s 192
    ./bench_boot.py --image /tmp/9vm/gefs.img -n 3
"""

import argparse
import os
import selectors
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VM = ROOT / "util/9vm/9vm"
KERNEL = ROOT / "build/harvey.32bit"
CMDLINE = "bootargs=local!#9/0 nobootprompt=local gefsroot=1"

MILESTONES = (
    ("kernel", "Booting"),
    ("hostring", "host transport"),
    ("gefs", "ASPERA-GEFS-ROOT PASS"),
    ("rc", "started /bin/rc"),
)


def cpu_ticks(pid: int) -> int:
    with open(f"/proc/{pid}/stat") as f:
        data = f.read()
    fields = data[data.rindex(")") + 2:].split()
    return int(fields[11]) + int(fields[12])  # utime + stime


def boot(args) -> tuple[dict[str, float], float, str]:
    cmd = [str(VM), "-r", str(args.stage), "-c", CMDLINE,
           "-disk", str(args.image)]
    if args.no9ring:
        cmd.append("-no9ring")
    cmd.append(str(KERNEL))
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    start = time.monotonic()
    deadline = start + args.timeout
    times: dict[str, float] = {}
    text = ""
    try:
        while time.monotonic() < deadline and "rc" not in times:
            if not sel.select(0.05):
                continue
            chunk = os.read(proc.stdout.fileno(), 65536)
            if not chunk:
                break
            text += chunk.decode(errors="replace").replace("\r", "")
            now = time.monotonic() - start
            for name, marker in MILESTONES:
                if name not in times and marker in text:
                    times[name] = now
        if "rc" not in times:
            raise RuntimeError("rc prompt did not appear:\n" + text[-2000:])

        time.sleep(0.5)
        hz = os.sysconf("SC_CLK_TCK")
        c0 = cpu_ticks(proc.pid)
        w0 = time.monotonic()
        time.sleep(args.idle)
        c1 = cpu_ticks(proc.pid)
        w1 = time.monotonic()
        cpu = (c1 - c0) / hz / (w1 - w0) * 100.0
        return times, cpu, text
    finally:
        sel.close()
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        proc.stdout.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", type=Path, default=Path("/tmp/9vm/gefs.img"))
    ap.add_argument("--stage", type=Path, default=Path("/tmp/9vm/stage"))
    ap.add_argument("--idle", type=float, default=2.0,
                    help="idle CPU sample window in seconds (default 2)")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--no9ring", action="store_true",
                    help="disable the shared ring (VirtIO baseline)")
    ap.add_argument("-n", "--repetitions", type=int, default=3)
    args = ap.parse_args()
    if args.repetitions < 1:
        ap.error("repetitions must be positive")
    if not VM.is_file() or not KERNEL.is_file():
        print("build 9vm and the kernel first", file=sys.stderr)
        return 1
    if not args.image.is_file():
        print(f"missing image {args.image}; run install_gefs.py first",
              file=sys.stderr)
        return 1

    runs = []
    for rep in range(args.repetitions):
        times, cpu, _ = boot(args)
        runs.append((times, cpu))
        print(f"run={rep+1} " + " ".join(
            (f"{name}={times[name]:.2f}s" if name in times else f"{name}=n/a")
            for name, _ in MILESTONES) + f" idle_cpu={cpu:.1f}%", flush=True)

    print("\nmedian of runs:")
    for name, _ in MILESTONES:
        values = [r[0][name] for r in runs if name in r[0]]
        if values:
            print(f"  {name:9s} {statistics.median(values):7.2f}s")
    print(f"  idle_cpu  {statistics.median(r[1] for r in runs):7.1f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
