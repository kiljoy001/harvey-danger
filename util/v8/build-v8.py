#!/usr/bin/env python3
"""Cross-build V8's d8 for Harvey/ARM64.

The V8 checkout is not vendored in the Harvey tree.  Fetch a V8 16.x checkout
with depot_tools somewhere, then:

    python3 util/v8/build-v8.py --v8 ~/Repo/v8

The script installs the Harvey GN toolchain and the small abseil/clang-23
patches, generates out/harvey, and builds d8.  V8 is built in jitless/lite
mode: no Sparkplug, Maglev, Turbofan, WebAssembly, sandbox or pointer
compression.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
HARVEY = HERE.parents[1]

ARGS = """\
is_debug=false
target_cpu="arm64"
target_os="linux"
is_clang=true
custom_toolchain="//harvey:harvey_arm64"
use_custom_libcxx=false
use_custom_libcxx_for_host=true
use_sysroot=false
v8_enable_lite_mode=true
v8_enable_webassembly=false
v8_enable_i18n_support=false
v8_enable_temporal_support=false
is_component_build=false
v8_enable_sandbox=false
v8_enable_pointer_compression=false
v8_enable_pointer_compression_shared_cage=false
v8_enable_external_code_space=false
v8_use_metagen=false
clang_use_chrome_plugins=false
use_cppgc_clang_plugin=false
arm_control_flow_integrity="none"
v8_control_flow_integrity=false
is_ubsan=false
is_asan=false
is_tsan=false
is_msan=false
"""


def run(cmd, cwd=None):
    print("+", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], cwd=cwd, check=True)


def apply_patch(repo: Path, patch: Path, strip: int = 1) -> None:
    check = subprocess.run(["git", "apply", "--check", f"-p{strip}", str(patch)],
                           cwd=repo, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    if check.returncode == 0:
        run(["git", "apply", f"-p{strip}", patch], cwd=repo)
    else:
        already = subprocess.run(["git", "apply", "--reverse", "--check",
                                  f"-p{strip}", str(patch)],
                                 cwd=repo, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
        if already.returncode != 0:
            sys.exit(f"cannot apply {patch} in {repo}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--v8", type=Path,
                    default=Path(os.environ.get("V8", HARVEY.parent / "v8")))
    ap.add_argument("--out", default="out/harvey")
    ap.add_argument("--target", default="d8")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--force", action="store_true",
                    help="regenerate args.gn even if it exists")
    args = ap.parse_args()

    v8 = args.v8.resolve()
    if not (v8 / "BUILD.gn").exists():
        sys.exit(f"{v8} does not look like a V8 checkout")
    if not shutil.which("gn"):
        sys.exit("gn not found; put depot_tools on PATH")

    # Install the toolchain and patches into the checkout.
    dest = v8 / "harvey"
    dest.mkdir(exist_ok=True)
    shutil.copy2(HERE / "harvey" / "BUILD.gn", dest / "BUILD.gn")
    apply_patch(v8, HERE / "patches" / "v8-simd-harvey.patch")
    apply_patch(v8 / "third_party" / "abseil-cpp",
                HERE / "patches" / "absl-harvey.patch")

    out = v8 / args.out
    out.mkdir(parents=True, exist_ok=True)
    argsfile = out / "args.gn"
    if args.force or not argsfile.exists():
        argsfile.write_text(ARGS)

    env = os.environ.copy()
    env["HARVEY"] = str(HARVEY)
    print(f"Harvey={HARVEY}")
    subprocess.run(["gn", "gen", str(out)], cwd=v8, env=env, check=True)
    subprocess.run(["ninja", "-C", str(out), "-j", str(args.jobs),
                    args.target], cwd=v8, env=env, check=True)
    print(f"built {out / args.target}")


if __name__ == "__main__":
    main()
