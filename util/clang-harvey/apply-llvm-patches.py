#!/usr/bin/env python3
"""Apply Harvey portability edits to the LLVM checkout.

These are two small, mechanical changes: LLVM takes the address of the bare
POSIX stat()/fstat() names in two places, but APEX defines those as
function-like macros that forward to __stat_ap()/__fstat().  The edits use
inlinable wrappers declared by util/libcxx/compat.h instead.

Run once after cloning LLVM, and again after updating the checkout:

    util/clang-harvey/apply-llvm-patches.py [/path/to/llvm-project]
"""
import os
import sys
from pathlib import Path

DEFAULT = Path(os.environ.get("LLVM_PROJECT", Path.home() / "Repo" / "llvm-project"))

EDITS = [
    (
        "llvm/lib/Support/Unix/Process.inc",
        "    if (RetryAfterSignal(-1, ::fstat, StandardFD, &st) < 0) {",
        "    if (RetryAfterSignal(-1, __harvey_fstat, StandardFD, &st) < 0) {",
    ),
    (
        "llvm/lib/Support/Unix/Path.inc",
        "  int StatRet = (Follow ? ::stat : ::lstat)(P.begin(), &Status);",
        "  int StatRet = (Follow ? __harvey_stat : ::lstat)(P.begin(), &Status);",
    ),
    (
        "llvm/lib/Support/Unix/Path.inc",
        "std::string getMainExecutable(const char *argv0, void *MainAddr) {\n"
        "#if defined(__APPLE__)",
        "std::string getMainExecutable(const char *argv0, void *MainAddr) {\n"
        "#if defined(HARVEY)\n"
        "  // Harvey has no /proc/self/exe and no dyld.  The driver is always\n"
        "  // started with a path, so resolve argv[0]; without this the driver\n"
        "  // reports an empty InstalledDir and cannot find its own cc1.\n"
        "  if (argv0 && *argv0) {\n"
        "    char Resolved[PATH_MAX];\n"
        "    if (::realpath(argv0, Resolved))\n"
        "      return Resolved;\n"
        "    return argv0;\n"
        "  }\n"
        "#endif\n"
        "#if defined(__APPLE__)",
    ),
]


def main():
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else DEFAULT
    if not (root / "llvm" / "lib" / "Support").is_dir():
        sys.exit(f"not an llvm-project checkout: {root}")
    for rel, old, new in EDITS:
        path = root / rel
        text = path.read_text()
        if new in text:
            print(f"already applied: {rel}")
            continue
        if old not in text:
            sys.exit(f"pattern not found in {rel}; update the patch script")
        path.write_text(text.replace(old, new))
        print(f"patched: {rel}")


if __name__ == "__main__":
    main()
