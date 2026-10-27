#!/usr/bin/env python3
"""
prism.py - Serverless Plan 9 microkernel runner and build tool.
Runs Objective-9C workloads on KVM, optionally with an opaque GEFS image.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
O9_REPO = os.path.abspath(os.environ.get(
    "O9_REPO", os.path.join(REPO_ROOT, "..", "objective-9c")))
O9C_BIN = os.path.join(O9_REPO, "o9c/o9c")
VM_BIN = os.path.join(REPO_ROOT, "util/9vm/9vm")
KERNEL_BIN = os.path.join(REPO_ROOT, "build/harvey.32bit")

CC = "clang"
LD = "ld.lld"

CFLAGS = [
    "-c",
    "-O2",
    "-static",
    "-mno-red-zone",
    "-mno-implicit-float",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-mcmodel=small",
    "-Wall",
    "-fcommon",
    "-Wno-parentheses",
    "-Wno-missing-braces",
    "-Wno-unknown-pragmas",
    "-Wno-unused-variable",
    "-Wno-unused-function",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
    f"-I{O9_REPO}",
    f"-I{os.path.join(O9_REPO, 'libtab')}",
]

LDFLAGS = [
    "-e_main",
    "-static",
    "-z", "noseparate-code",
    "-z", "max-page-size=0x200000",
]

LIBS = [
    os.path.join(REPO_ROOT, "build/libo9.a"),
    os.path.join(REPO_ROOT, "build/libdraw.a"),
    os.path.join(REPO_ROOT, "build/libmemdraw.a"),
    os.path.join(REPO_ROOT, "build/libthread.a"),
    os.path.join(REPO_ROOT, "build/lib9p.a"),
    os.path.join(REPO_ROOT, "build/libauth.a"),
    os.path.join(REPO_ROOT, "build/libauthsrv.a"),
    os.path.join(REPO_ROOT, "build/libsec.a"),
    os.path.join(REPO_ROOT, "build/libndb.a"),
    os.path.join(REPO_ROOT, "build/libbio.a"),
    os.path.join(REPO_ROOT, "build/libflate.a"),
    os.path.join(REPO_ROOT, "build/libc.a"),
]

def compile_o9(source_path, output_binary, keep_temps=False, extra_sources=None):
    """Transpile and link an Objective-9C file into a Plan 9 AMD64 binary."""
    if not os.path.exists(source_path):
        raise FileNotFoundError(f"Source file not found: {source_path}")

    base_name = os.path.splitext(os.path.basename(source_path))[0]
    build_dir = os.path.join(REPO_ROOT, "build")
    os.makedirs(build_dir, exist_ok=True)

    temporary = tempfile.mkdtemp(prefix=f"{base_name}-", dir=build_dir)
    c_file = os.path.join(temporary, f"{base_name}.o9.c")
    obj_file = os.path.join(temporary, f"{base_name}.o9.o")

    if extra_sources is None:
        extra_sources = []
    try:
        with open(c_file, "w") as f:
            res = subprocess.run([O9C_BIN, source_path], stdout=f,
                                 stderr=subprocess.PIPE, text=True)
            if res.returncode != 0:
                raise RuntimeError(f"o9c transpilation failed:\n{res.stderr}")

        src_dir = os.path.dirname(os.path.abspath(source_path))
        compile_cmd = [CC] + CFLAGS + [f"-I{src_dir}", c_file, "-o", obj_file]
        res = subprocess.run(compile_cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"Compilation failed:\n{res.stderr}")

        extra_objects = []
        for index, source in enumerate(extra_sources):
            source = os.path.abspath(source)
            if not os.path.isfile(source):
                raise FileNotFoundError(f"C source file not found: {source}")
            extra_object = os.path.join(
                temporary, f"extra-{index}-{os.path.basename(source)}.o")
            compile_cmd = ([CC] + CFLAGS +
                           [f"-I{os.path.dirname(source)}", source,
                            "-o", extra_object])
            res = subprocess.run(compile_cmd, capture_output=True, text=True)
            if res.returncode != 0:
                raise RuntimeError(
                    f"C compilation failed for {source}:\n{res.stderr}")
            extra_objects.append(extra_object)

        os.makedirs(os.path.dirname(os.path.abspath(output_binary)), exist_ok=True)
        # libflate is only built for guests that use it (such as o9irc).
        # Resolve this at link time because those builds import prism before
        # creating the archive.
        libs = [lib for lib in LIBS if not lib.endswith("/libflate.a") or
                os.path.exists(lib)]
        link_cmd = ([LD] + LDFLAGS + [obj_file] + extra_objects + libs +
                    ["-o", output_binary])
        res = subprocess.run(link_cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"Linking failed:\n{res.stderr}")
    finally:
        if not keep_temps:
            shutil.rmtree(temporary)

    return output_binary

def run_workload(source_path, ram_mb=512, timeout_sec=10.0, quiet=False,
                 verbose=False, gefs_image=None, extra_sources=None):
    """Execute an Objective-9C workload under the ephemeral microkernel."""
    build_dir = os.path.join(REPO_ROOT, "build")
    os.makedirs(build_dir, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix="prism-boot-", dir=build_dir,
                                     delete=False) as guest_file:
        guest_binary = guest_file.name

    try:
        t_start_build = time.perf_counter()
        compile_o9(source_path, guest_binary, extra_sources=extra_sources)
        t_build = time.perf_counter() - t_start_build

        if verbose:
            print(f"[prism] Built workload in {t_build*1000:.1f} ms")

        vm_cmd = [VM_BIN, "-m", str(ram_mb), "-r", REPO_ROOT,
                  "-boot", guest_binary, KERNEL_BIN]
        if gefs_image is not None:
            vm_cmd[-1:-1] = ["-gefs-image", os.path.abspath(gefs_image)]

        t_start_vm = time.perf_counter()
        proc = subprocess.Popen(
            vm_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1
        )

        userland_started = False
        userland_output = []
        all_output = []

        start_time = time.perf_counter()
        while True:
            line = proc.stdout.readline()
            if not line and proc.poll() is not None:
                break
            if line:
                all_output.append(line)
                if not userland_started:
                    # Detect handover to userland
                    if "init: mounting #9/0 to /..." in line:
                        userland_started = True
                else:
                    if not line.startswith("[init] PID 1 exited"):
                        userland_output.append(line)
                        if quiet:
                            sys.stdout.write(line)
                            sys.stdout.flush()

                if not quiet:
                    sys.stdout.write(line)
                    sys.stdout.flush()

            if (time.perf_counter() - start_time) > timeout_sec:
                proc.terminate()
                raise TimeoutError(f"Microkernel workload timed out after {timeout_sec}s")

        proc.wait()
        t_vm = time.perf_counter() - t_start_vm

        if verbose:
            print(f"\n[prism] VM exit code: {proc.returncode} (execution time: {t_vm*1000:.1f} ms)")

        return proc.returncode, "".join(userland_output if quiet else all_output)

    finally:
        os.unlink(guest_binary)

def main():
    parser = argparse.ArgumentParser(description="Prism Serverless Plan 9 CLI")
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    # run command
    run_parser = subparsers.add_parser("run", help="Run an Objective-9C file serverlessly under KVM")
    run_parser.add_argument("file", help="Path to .o9 script")
    run_parser.add_argument("-m", "--ram", type=int, default=512, help="Guest RAM in MB (default 512)")
    run_parser.add_argument("-t", "--timeout", type=float, default=10.0, help="Timeout in seconds (default 10)")
    run_parser.add_argument("-q", "--quiet", action="store_true", help="Quiet mode: only show userland stdout")
    run_parser.add_argument("-v", "--verbose", action="store_true", help="Verbose mode: show timings and VM debug info")
    run_parser.add_argument("--gefs-image", help="Attach a GEFS image as #9/2")
    run_parser.add_argument("--c-source", action="append", default=[],
                            help="Additional C source to compile and link (repeatable)")

    # build command
    build_parser = subparsers.add_parser("build", help="Compile an Objective-9C file into a Plan 9 binary")
    build_parser.add_argument("file", help="Path to .o9 script")
    build_parser.add_argument("-o", "--output", required=True, help="Destination binary path")
    build_parser.add_argument("--keep-temps", action="store_true", help="Keep intermediate C/object files")
    build_parser.add_argument("--c-source", action="append", default=[],
                              help="Additional C source to compile and link (repeatable)")

    # view command
    view_parser = subparsers.add_parser("view", help="Launch interactive graphical viewer for o9aspera")

    args = parser.parse_args()

    if args.subcommand == "run":
        try:
            rc, _ = run_workload(
                args.file,
                ram_mb=args.ram,
                timeout_sec=args.timeout,
                quiet=args.quiet,
                verbose=args.verbose,
                gefs_image=args.gefs_image,
                extra_sources=args.c_source,
            )
            sys.exit(rc)
        except Exception as e:
            print(f"[prism] Error: {e}", file=sys.stderr)
            sys.exit(1)
    elif args.subcommand == "build":
        try:
            compile_o9(args.file, args.output, keep_temps=args.keep_temps,
                       extra_sources=args.c_source)
            print(f"[prism] Created {args.output} ({os.path.getsize(args.output)} bytes)")
        except Exception as e:
            print(f"[prism] Error: {e}", file=sys.stderr)
            sys.exit(1)
    elif args.subcommand == "view":
        aspera_bin = os.path.join(REPO_ROOT, "build/o9aspera_interactive")
        src_host = os.path.join(REPO_ROOT, "sys/src/cmd/o9aspera/aspera_host.c")
        src_engine = os.path.join(REPO_ROOT, "sys/src/cmd/o9aspera/aspera_engine.c")
        build_needed = (
            not os.path.exists(aspera_bin) or
            os.path.getmtime(src_host) > os.path.getmtime(aspera_bin) or
            os.path.getmtime(src_engine) > os.path.getmtime(aspera_bin)
        )
        if build_needed:
            print("[prism] Compiling Aspera interactive window system...")
            os.makedirs(os.path.join(REPO_ROOT, "build"), exist_ok=True)
            subprocess.run([
                "gcc", "-O2", "-Wall", "-Wextra",
                src_host, src_engine,
                "-o", aspera_bin, "-lSDL2", "-lm"
            ], check=True)
        if "DISPLAY" not in os.environ and "WAYLAND_DISPLAY" not in os.environ:
            os.environ["DISPLAY"] = ":1"
        print("[prism] Launching Aspera Window System...")
        os.execv(aspera_bin, [aspera_bin])

if __name__ == "__main__":
    main()
