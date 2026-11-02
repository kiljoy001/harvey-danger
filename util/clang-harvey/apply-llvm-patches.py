#!/usr/bin/env python3
"""Apply Harvey portability edits to the LLVM checkout.

These are small, mechanical changes: LLVM and libc++ take the address of, or
qualify, the POSIX stat()/fstat() names, which APEX defines as function-like
macros forwarding to __stat_ap()/__fstat().  Defining real stat()/fstat()
symbols instead would capture Plan 9's three-argument stat() and recurse.

Run once after cloning LLVM, and again after updating the checkout:

    util/clang-harvey/apply-llvm-patches.py [/path/to/llvm-project]

The script is idempotent: an edit whose replacement text is already present is
left alone.
"""
import os
import sys
from pathlib import Path

DEFAULT = Path(os.environ.get("LLVM_PROJECT", Path.home() / "Repo" / "llvm-project"))

EDITS = [
    # lib/Support: the bare names are used as function values / qualified calls.
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
    # Harvey has no /proc/self/exe and no dyld; the driver is always started
    # with a path, so resolve argv[0].  Without this InstalledDir is empty and
    # the driver cannot find its own cc1 or its resource directory.
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
    (
        "llvm/lib/Support/Unix/Jobserver.inc",
        "  if (::fstat(FD, &StatBuf) != 0)",
        "  if (__harvey_fstat(FD, &StatBuf) != 0)",
    ),
    (
        "llvm/lib/Support/Unix/Path.inc",
        "  int StatRet = ::fstat(FD, &Status);",
        "  int StatRet = __harvey_fstat(FD, &Status);",
    ),
    # libc++'s filesystem needs fs_stat/fs_fstat because `using ::stat;` would
    # name the macro and `detail::stat(...)` would expand inside the namespace.
    (
        "libcxx/src/filesystem/posix_compat.h",
        "using ::fstat;",
        "inline int fs_fstat(int __fd, struct stat *__st) { return ::__fstat(__fd, __st); }",
    ),
    (
        "libcxx/src/filesystem/posix_compat.h",
        "using ::stat;",
        "inline int fs_stat(const char *__p, struct stat *__st) { return ::__stat_ap(__p, __st); }",
    ),
    (
        "libcxx/src/filesystem/file_descriptor.h",
        "  if (detail::stat(p.c_str(), &path_stat) == -1)",
        "  if (detail::fs_stat(p.c_str(), &path_stat) == -1)",
    ),
    (
        "libcxx/src/filesystem/file_descriptor.h",
        "  if (detail::fstat(fd, &m_stat) == -1)",
        "  if (detail::fs_fstat(fd, &m_stat) == -1)",
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
