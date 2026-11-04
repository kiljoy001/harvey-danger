#!/usr/bin/env python3
"""Wrap an ARM64 program in a launcher that runs it as the kernel's init.

The ARM64 kernel starts its first user process with no open descriptors,
no arguments and a read-only boot filesystem.  This generates a small APEX
program that embeds the target ELF (and optionally a tree of data files),
opens the console, unions a ramfs over /, writes everything out and execs
the target with the arguments given here:

    util/arm64/mkrun.py build/arm64-apex/hello.elf -- hello arg1
    util/arm64/mkrun.py /tmp/opencode/llvm-arm64/bin/llvm-tblgen \
        --tree /home/scott/Repo/llvm-project/llvm/include:/td \
        -- -gen-vt -I /td /td/llvm/Target/ValueTypes.td

Output: build/arm64-run/runner.elf (use with test_apex_boot.py --elf).
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "arm64-run"


RUNNER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* Plan 9 namespace calls (see sys/include/libc.h). */
extern int bind(char *, char *, int);
extern int mkdir(const char *, unsigned);
enum { MREPL = 0, MCREATE = 4 };

static const unsigned char image[] = {
%s
};

static const unsigned char blob[] = {
%s
};

struct datafile {
	const char *path;
	unsigned offset;
	unsigned length;
};

static const struct datafile files[] = {
%s
};

static char *args[] = {
%s
};

static int
mkdirs(char *path)
{
	char *p;

	for(p = path + 1; *p != 0; p++){
		if(*p != '/')
			continue;
		*p = 0;
		if(mkdir(path, 0777) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return 0;
}

static int
writefile(const char *path, const unsigned char *data, unsigned long len)
{
	char tmp[256];
	int fd;
	unsigned long off = 0;

	if(strlen(path) >= sizeof tmp)
		return -1;
	strcpy(tmp, path);
	if(mkdirs(tmp) < 0)
		return -1;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0777);
	if(fd < 0)
		return -1;
	while(off < len){
		long n = write(fd, data + off, len - off);

		if(n <= 0){
			close(fd);
			return -1;
		}
		off += n;
	}
	close(fd);
	return 0;
}

int
main(void)
{
	int i, fd;

	/* The kernel gives the first process no descriptors. */
	open("#c/cons", 0);
	open("#c/cons", 1);
	open("#c/cons", 1);

	/*
	 * The boot filesystem is read-only.  Replacing the root with a ramfs
	 * gives us somewhere to write; the console is already open.
	 */
	if(bind("#@", "/", MREPL | MCREATE) < 0)
		fprintf(stderr, "runner: bind: %%s\n", strerror(errno));

	fd = open("/runner-image", O_WRONLY | O_CREAT | O_TRUNC, 0777);
	if(fd < 0){
		fprintf(stderr, "runner: create: %%s\n", strerror(errno));
		return 1;
	}
	{
		unsigned long off = 0;

		while(off < sizeof image){
			long n = write(fd, image + off, sizeof image - off);

			if(n <= 0){
				fprintf(stderr, "runner: write: %%s\n",
				    strerror(errno));
				return 1;
			}
			off += n;
		}
	}
	close(fd);

	for(i = 0; i < (int)(sizeof files / sizeof files[0]); i++)
		if(writefile(files[i].path, blob + files[i].offset,
		    files[i].length) < 0){
			fprintf(stderr, "runner: %%s: %%s\n", files[i].path,
			    strerror(errno));
			return 1;
		}
	fprintf(stderr, "runner: wrote %%d files, exec %%s\n", i, args[0]);

	execv("/runner-image", args);
	fprintf(stderr, "runner: exec: %%s\n", strerror(errno));
	return 1;
}
"""


def emit(data: bytes) -> str:
    lines = []
    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        lines.append("\t" + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(lines)


def c_string(s: str) -> str:
    out = []
    for ch in s:
        if ch in ('"', "\\"):
            out.append("\\" + ch)
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.append(f"\\{ord(ch):03o}")
    return '"' + "".join(out) + '"'


def collect_tree(host: Path, guest: str, suffixes: tuple[str, ...]):
    """Collect files from host under guest, preserving relative paths."""
    files = []
    for path in sorted(host.rglob("*")):
        if not path.is_file():
            continue
        if suffixes and path.suffix not in suffixes:
            continue
        rel = path.relative_to(host)
        files.append((f"{guest}/{rel}", path.read_bytes()))
    return files


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("elf")
    ap.add_argument("--tree", action="append", default=[],
                    metavar="HOSTDIR:GUESTDIR",
                    help="embed files from HOSTDIR under GUESTDIR")
    ap.add_argument("--suffix", default=".td",
                    help="only embed files with this suffix (default .td)")
    ap.add_argument("--output", default=str(OUT / "runner.elf"))
    opts, rest = ap.parse_known_args()
    if rest and rest[0] == "--":
        rest = rest[1:]
    opts.args = rest

    image = Path(opts.elf).read_bytes()
    data = []
    for spec in opts.tree:
        host, guest = spec.split(":", 1)
        suffixes = tuple(s for s in opts.suffix.split(",") if s)
        data.extend(collect_tree(Path(host), guest, suffixes))

    blob = bytearray()
    table = []
    for path, contents in data:
        table.append((path, len(blob), len(contents)))
        blob.extend(contents)

    argv = [Path(opts.elf).name] + opts.args
    src = OUT / "runner.c"
    OUT.mkdir(parents=True, exist_ok=True)
    src.write_text(RUNNER % (
        emit(image),
        emit(bytes(blob)),
        ",\n".join(f'\t{c_string(p)}, {o}, {n}' for p, o, n in table) or
        "\t{0, 0, 0}",
        ",\n".join("\t" + c_string(a) for a in argv) + ",\n\t0"))

    # Link with the Plan 9 libc ahead of the APEX archive: APEX's BSD socket
    # layer also defines bind(); the namespace bind() is the one a runner
    # needs.  crt0 and the startup still come from libap.a.
    cc = Path(os.environ.get("ARM64_CC") or
              (shutil.which("clang-23") or "clang"))
    ld = os.environ.get("ARM64_LD", "ld.lld")
    obj = OUT / "runner.o"
    subprocess.run([str(cc), "--target=aarch64-unknown-linux-elf",
                    "-nostdlibinc", "-mstrict-align", "-mno-outline-atomics",
                    "-O2", "-fno-stack-protector",
                    "-I", str(ROOT / "util" / "arm64" / "include"),
                    "-I", str(ROOT / "apex" / "include"),
                    "-I", str(ROOT / "apex" / "aarch64" / "include"),
                    "-D_SUSV2_SOURCE", "-D_POSIX_SOURCE",
                    "-D_LIMITS_EXTENSION", "-D_GNU_SOURCE", "-DHARVEY",
                    "-c", str(src), "-o", str(obj)], check=True)
    subprocess.run([ld, "-z", "max-page-size=4096", "--eh-frame-hdr",
                    "-T", str(ROOT / "util" / "arm64" / "apex.ld"),
                    "-e", "_start", "-u", "_start", str(obj),
                    str(ROOT / "build" / "arm64-libc" / "libc.a"),
                    str(ROOT / "build" / "arm64-apex" / "libap.a"),
                    str(ROOT / "build" / "arm64-apex" / "libbuiltins.a"),
                    "-o", opts.output], check=True)
    out = Path(opts.output)
    print(f"built {out} ({out.stat().st_size} bytes), "
          f"{len(data)} data files, runs {argv}")


if __name__ == "__main__":
    main()
