#!/usr/bin/env python3
"""Cross-build a minimal native rc command set for the Aspera GEFS image."""

import json
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
CFLAGS = [
    "-c", "-O2", "-static", "-mno-red-zone", "-mno-implicit-float",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-mcmodel=small", "-fcommon", "-Wno-unknown-pragmas",
    # 9front sources predate C99 and use Plan 9 libc idioms (implicit int,
    # function-pointer signatures without const, variadic USED).
    "-Wno-implicit-int", "-Wno-implicit-function-declaration",
    "-Wno-incompatible-function-pointer-types", "-Wno-parentheses",
    f"-I{ROOT / 'amd64/include'}", f"-I{ROOT / 'sys/include'}",
]
LDFLAGS = ["-e_main", "-static", "-z", "noseparate-code",
           "-z", "max-page-size=0x200000"]


def compile_file(source: Path, output: Path, extra=()) -> None:
    subprocess.run(["clang", *CFLAGS, *extra, str(source), "-o", str(output)],
                   check=True)


def build_rc() -> None:
    source = ROOT / "sys/src/cmd/rc"
    work = BUILD / "aspera-rc-build"
    work.mkdir(parents=True, exist_ok=True)
    for path in source.iterdir():
        if path.suffix in (".c", ".h", ".y"):
            shutil.copy2(path, work / path.name)
    subprocess.run(["bison", "-y", "-d", "-Wno-yacc", "syn.y"],
                   cwd=work, check=True)
    shutil.copy2(work / "y.tab.h", work / "x.tab.h")
    names = json.loads((source / "rc.json").read_text())[0]["SourceFiles"]
    objects = []
    for name in names:
        obj = work / f"{name}.o"
        compile_file(work / name, obj, (f"-I{work}",))
        objects.append(obj)

    string_source = ROOT / "sys/src/libString"
    string_work = BUILD / "aspera-libString"
    string_work.mkdir(parents=True, exist_ok=True)
    string_names = json.loads((string_source / "libString.json").read_text())[0]["SourceFiles"]
    string_objects = []
    for name in string_names:
        obj = string_work / f"{name}.o"
        compile_file(string_source / name, obj)
        string_objects.append(obj)
    string_lib = BUILD / "aspera-libString.a"
    subprocess.run(["llvm-ar", "rcs", str(string_lib),
                    *map(str, string_objects)], check=True)
    subprocess.run(["ld.lld", *LDFLAGS, *map(str, objects),
                    str(string_lib), str(BUILD / "libc.a"),
                    "-o", str(BUILD / "aspera-rc")], check=True)


def build_simple(name: str) -> None:
    obj = BUILD / f"aspera-{name}.o"
    compile_file(ROOT / f"sys/src/cmd/{name}.c", obj)
    libs = [str(BUILD / "libbio.a")] if name == "ls" else []
    subprocess.run(["ld.lld", *LDFLAGS, str(obj), *libs,
                    str(BUILD / "libc.a"), "-o", str(BUILD / f"aspera-{name}")],
                   check=True)


# Core userland ported from the 9front tree (sibling checkout).
# Sources are compiled unmodified; Harvey's libc lacks only the
# IOUNIT constant, supplied here.  Some commands need libbio or
# libString at link time.
NINEFRONT = Path(__file__).resolve().parents[3] / "plan9front"
CORE9_BIO = ("cal", "comm", "sort", "tail", "uniq")
CORE9_STRING = ("du", "tar")
CORE9_PLAIN = ("basename", "chmod", "cleanname", "cmp", "cp", "dd",
               "mkdir", "mv", "seq", "tee", "tr", "wc")
CORE9 = CORE9_PLAIN + CORE9_BIO + CORE9_STRING

# Extended 9front command set.  Grouped by the extra library each one
# needs at link time; a few need a one-line source shim for a 9front
# libc extension Harvey lacks (procsetuser, variadic USED, C89 implicit
# int).  Commands that depend on 9front-only APIs we do not have yet
# (libmach, the newer libbio Blethal/Biofn, timezone tmfmt) are left out
# rather than patched into something subtly wrong.
CORE9_MORE_PLAIN = ("bind", "unmount", "sum", "crc32", "read", "pwd")
CORE9_MORE_C89 = ("touch",)
CORE9_MORE_BIO = ("ns", "strings", "factor", "primes", "dc")
CORE9_MORE_STRING_SEC = ("lnfs",)
CORE9_MORE_SEC = ("md5sum",)
CORE9_MORE_AUTH = ("mount", "srv")
CORE9_MORE_PATCH = {"pwd": (("USED(argc, argv)", "USED(argc)"),)}

COMPAT_H = """/* 9front command compat for Harvey. No includes: Harvey's u.h has no
 * include guard, so including it here would collide with the command's. */
static int procsetuser(const char *s) { (void)s; return 0; }
"""


def build_core9_more() -> None:
    cmd = NINEFRONT / "sys/src/cmd"
    if not cmd.is_dir():
        return
    compat = BUILD / "harvey_cmd_compat.h"
    compat.write_text(COMPAT_H)

    def one(name: str, libs=(), extra=()) -> None:
        src = cmd / f"{name}.c"
        if not src.is_file():
            print(f"skip: {name} (no source)")
            return
        work = src
        if name in CORE9_MORE_PATCH:
            work = BUILD / f"{name}.patched.c"
            text = src.read_text()
            for old, new in CORE9_MORE_PATCH[name]:
                text = text.replace(old, new)
            work.write_text(text)
        obj = BUILD / f"aspera-{name}.o"
        flags = ["-DIOUNIT=32768"]
        if name in CORE9_MORE_C89:
            flags.append("-Wno-implicit-int")
        if name in CORE9_MORE_AUTH:
            flags.extend(("-include", str(compat)))
        compile_file(work, obj, tuple(flags + list(extra)))
        subprocess.run(["ld.lld", *LDFLAGS, str(obj),
                        *[str(BUILD / lib) for lib in libs],
                        str(BUILD / "libc.a"),
                        "-o", str(BUILD / f"aspera-{name}")], check=True)

    for name in CORE9_MORE_PLAIN:
        one(name)
    for name in CORE9_MORE_C89:
        one(name)
    for name in CORE9_MORE_BIO:
        one(name, libs=("libbio.a",))
    for name in CORE9_MORE_STRING_SEC:
        one(name, libs=("libbio.a", "aspera-libString.a", "libsec.a"))
    for name in CORE9_MORE_SEC:
        one(name, libs=("libsec.a",))
    for name in CORE9_MORE_AUTH:
        one(name, libs=("libauth.a", "libauthsrv.a", "libsec.a", "libndb.a"))
    print("Built extended 9front tools")


def build_core9() -> None:
    cmd = NINEFRONT / "sys/src/cmd"
    if not cmd.is_dir():
        print(f"9front tree not found at {NINEFRONT}; skipping core tools")
        return
    for name in CORE9:
        obj = BUILD / f"aspera-{name}.o"
        compile_file(cmd / f"{name}.c", obj, ("-DIOUNIT=32768",))
        libs = []
        if name in CORE9_BIO:
            libs.append(str(BUILD / "libbio.a"))
        if name in CORE9_STRING:
            libs.append(str(BUILD / "aspera-libString.a"))
        subprocess.run(["ld.lld", *LDFLAGS, str(obj), *libs,
                        str(BUILD / "libc.a"),
                        "-o", str(BUILD / f"aspera-{name}")], check=True)
    build_core9_more()
    print(f"Built {len(CORE9)} core tools from 9front sources")


# Second tier: the everyday text and inspection tools.  libregexp is
# built from Harvey's sources because the archive is not staged with
# the kernel; everything else uses the extended libbio.
CORE9_MORE2_PLAIN = ("chgrp", "cmp", "mtime", "time")
CORE9_MORE2_BIO = ("p", "fmt", "look", "ascii", "unicode", "col", "pr",
                   "ps", "xd")
CORE9_MORE2_BIO_RE = ("split",)
CORE9_MORE2_PATCH = {
    # clang does not treat a tagged union without a declarator as an
    # anonymous member (kencc does).  Drop the tag.
    "sed": (("\tunion PCTL {", "\tunion {"),),
}


def build_libregexp() -> Path:
    source = ROOT / "sys/src/libregexp"
    objects = []
    for path in sorted(source.glob("*.c")):
        obj = BUILD / "libregexp" / f"{path.stem}.o"
        obj.parent.mkdir(parents=True, exist_ok=True)
        compile_file(path, obj)
        objects.append(obj)
    lib = BUILD / "libregexp.a"
    subprocess.run(["llvm-ar", "rcs", str(lib), *map(str, objects)],
                   check=True)
    return lib


def link(name: str, objects, libs) -> None:
    subprocess.run(["ld.lld", *LDFLAGS, *map(str, objects),
                    *[str(BUILD / lib) for lib in libs],
                    str(BUILD / "libc.a"),
                    "-o", str(BUILD / f"aspera-{name}")], check=True)


def build_core9_more2() -> None:
    cmd = NINEFRONT / "sys/src/cmd"
    if not cmd.is_dir():
        return
    build_libregexp()

    def one(name: str, libs=()) -> None:
        src = cmd / f"{name}.c"
        if not src.is_file():
            print(f"skip: {name} (no source)")
            return
        work = src
        if name in CORE9_MORE2_PATCH:
            work = BUILD / f"{name}.patched.c"
            text = src.read_text()
            for old, new in CORE9_MORE2_PATCH[name]:
                text = text.replace(old, new)
            work.write_text(text)
        obj = BUILD / f"aspera-{name}.o"
        compile_file(work, obj, ("-DIOUNIT=32768",))
        link(name, [obj], libs)

    for name in CORE9_MORE2_PLAIN:
        one(name)
    for name in CORE9_MORE2_BIO:
        one(name, ("libbio.a",))
    for name in CORE9_MORE2_BIO_RE:
        one(name, ("libbio.a", "libregexp.a"))
    # sed also needs libregexp; it is patched rather than plain.
    one("sed", ("libbio.a", "libregexp.a"))

    # grep is a yacc grammar plus three C files.
    grep_dir = cmd / "grep"
    work = BUILD / "aspera-grep-build"
    work.mkdir(parents=True, exist_ok=True)
    for path in grep_dir.iterdir():
        if path.suffix in (".c", ".h", ".y"):
            shutil.copy2(path, work / path.name)
    # plan9port yacc matches the Plan 9 grammar conventions; bison's
    # skeleton emits its own yylex prototype and collides with grep.y.
    yacc = Path("/usr/local/plan9/bin/yacc")
    if not yacc.is_file():
        yacc = Path(shutil.which("yacc") or "yacc")
    subprocess.run([str(yacc), "-d", "grep.y"], cwd=work, check=True)
    generated = work / "y.tab.c"
    text = generated.read_text()
    # plan9port yacc emits fprint/sprint prototypes that conflict with
    # Harvey's const-qualified declarations.
    for line in ("extern\tint\tfprint(int, char*, ...);\n",
                 "extern\tint\tsprint(char*, char*, ...);\n"):
        text = text.replace(line, "")
    generated.write_text(text)
    objects = []
    for name in ("y.tab.c", "main.c", "comp.c", "sub.c"):
        obj = work / f"{name}.o"
        compile_file(work / name, obj, (f"-I{work}", "-DIOUNIT=32768"))
        objects.append(obj)
    link("grep", objects, ("libbio.a", "libregexp.a"))

    # diff is five C files sharing diff.h.
    diff_dir = cmd / "diff"
    objects = []
    for name in ("diff", "diffdir", "diffio", "diffreg", "util"):
        obj = BUILD / f"aspera-diff-{name}.o"
        compile_file(diff_dir / f"{name}.c", obj,
                     (f"-I{diff_dir}", "-DIOUNIT=32768"))
        objects.append(obj)
    link("diff", objects, ("libbio.a", "libregexp.a"))
    print("Built second-tier 9front tools (grep, sed, diff, ...)")


def main() -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    build_rc()
    for name in ("ls", "cat", "test", "echo", "sleep", "rm"):
        build_simple(name)
    build_core9()
    build_core9_more2()
    print("Built native Aspera rc, ls, cat, test, echo, and sleep")


if __name__ == "__main__":
    main()
