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
#include <sys/wait.h>

/* Plan 9 namespace calls (see sys/include/libc.h). */
extern int bind(char *, char *, int);
extern int mkdir(const char *, unsigned);
/* APEX wait4; libc's waitpid (all that sys/wait.h exposes here) takes no
 * arguments and reaps any child, so it cannot report a specific status. */
extern int wait4(int, int *, int, void *);
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

static char *args2[] = {
%s
};

/* When --in-tree names a program already in the tree, no image is written. */
static const int have_image = %d;
static const char exec_path[] = %s;

/* Optional tar tree (see --untar) to unpack before running. */
static const char untar_name[] = %s;
static const char untar_dest[] = %s;

static const char *envs[] = {
%s
};

static const char *servers[] = {
%s
};

static const char *waits[] = {
%s
};

static const char bootfs[] = %s;

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

static unsigned long
otoul(const char *field, int len)
{
	unsigned long v = 0;
	int i;

	for(i = 0; i < len && field[i] >= '0' && field[i] <= '7'; i++)
		v = v * 8 + (field[i] - '0');
	return v;
}

/*
 * Unpack a ustar archive into dest.  Only the fields the SDK needs are
 * interpreted: name, prefix, size and typeflag.  Malformed headers are
 * reported rather than guessed at.
 */
static int
untar(const char *name, const char *dest)
{
	char hdr[512], path[512], buf[16384];
	unsigned long size, left;
	int fd, out, n, nfiles = 0;

	fd = open(name, O_RDONLY);
	if(fd < 0){
		fprintf(stderr, "runner: %%s: %%s\n", name, strerror(errno));
		return -1;
	}
	for(;;){
		for(n = 0; n < 512; ){
			int m = read(fd, hdr + n, 512 - n);

			if(m <= 0)
				goto done;
			n += m;
		}
		if(hdr[0] == 0)
			break;		/* end of archive */
		if(strncmp(hdr+257, "ustar", 5) != 0){
			fprintf(stderr, "runner: %%s: bad tar header\n", name);
			close(fd);
			return -1;
		}
		size = otoul(hdr + 124, 12);
		if(hdr[345] != 0){
			if(snprintf(path, sizeof path, "%%s/%%s/%%s", dest,
			    hdr + 345, hdr) >= (int)sizeof path){
				fprintf(stderr, "runner: %%s: name too long\n", name);
				close(fd);
				return -1;
			}
		}else if(snprintf(path, sizeof path, "%%s/%%s", dest, hdr) >=
		    (int)sizeof path){
			fprintf(stderr, "runner: %%s: name too long\n", name);
			close(fd);
			return -1;
		}
		nfiles++;
		if((nfiles & 63) == 0)
			fprintf(stderr, "runner: untar %%d %%s\n", nfiles, path);
		if(hdr[156] == '5'){
			if(mkdirs(path) < 0){
				fprintf(stderr, "runner: mkdirs %%s: %%s\n", path,
				    strerror(errno));
				close(fd);
				return -1;
			}
			if(mkdir(path, 0777) < 0 && errno != EEXIST){
				fprintf(stderr, "runner: %%s: %%s\n", path,
				    strerror(errno));
				close(fd);
				return -1;
			}
		}else if(hdr[156] == '0' || hdr[156] == 0){
			if(mkdirs(path) < 0){
				fprintf(stderr, "runner: mkdirs %%s: %%s\n", path,
				    strerror(errno));
				close(fd);
				return -1;
			}
			out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
			if(out < 0){
				fprintf(stderr, "runner: create %%s: %%s\n", path,
				    strerror(errno));
				close(fd);
				return -1;
			}
			left = size;
			while(left > 0){
				unsigned long want = left < sizeof buf ?
				    left : sizeof buf;
				int m = read(fd, buf, want);

				if(m <= 0){
					fprintf(stderr, "runner: %%s: short\n",
					    name);
					close(fd);
					close(out);
					return -1;
				}
				errno = 0;
				{
					int w = write(out, buf, m);

					if(w != m){
						fprintf(stderr,
						    "runner: write %%s: got %%d want %%d/%%d errno %%d %%s\n",
						    path, w, m, left, errno,
						    strerror(errno));
						close(fd);
						close(out);
						return -1;
					}
				}
				left -= m;
			}
			close(out);
			left = size %% 512;
			if(left != 0)
				read(fd, buf, 512 - left);
			continue;
		}
		/* Skip the body of anything we do not create (links, etc). */
		left = size + ((512 - size %% 512) %% 512);
		while(left > 0){
			int m = read(fd, buf, left < sizeof buf ? left : sizeof buf);

			if(m <= 0)
				goto done;
			left -= m;
		}
	}
done:
	close(fd);
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
	 * gives us somewhere to write; the console is already open.  When a
	 * tar tree is supplied we keep / visible (the archive is a boot file)
	 * and mount the ramfs over /initramfs instead.
	 */
	if(untar_name[0] != 0){
		/*
		 * The tar tree wants /dev/null, /dev/cons and friends.
		 * Plan 9's getenv() reads /env, so the environment device has
		 * to be there too; APEX's setenv() and execve() write #e.
		 */
		bind("#c", "/dev", MREPL);
		bind("#e", "/env", MREPL);
		if(bind("#@", "/initramfs", MREPL | MCREATE) < 0)
			fprintf(stderr, "runner: bind: %%s\n", strerror(errno));
		if(strcmp(untar_name, "-") != 0 &&
		    untar(untar_name, untar_dest) < 0)
			return 1;
	}else{
		if(bind("#@", "/", MREPL | MCREATE) < 0)
			fprintf(stderr, "runner: bind: %%s\n", strerror(errno));
		mkdir("/env", 0777);
		bind("#e", "/env", MREPL | MCREATE);
		mkdir("/dev", 0777);
		bind("#c", "/dev", MREPL | MCREATE);
	}

	if(bootfs[0] != 0){
		mkdir(bootfs, 0777);
		if(bind("#@", bootfs, MREPL) < 0)
			fprintf(stderr, "runner: bind %%s: %%s\n", bootfs,
			    strerror(errno));
	}

	if(servers[0] != 0 || waits[0] != 0){
		mkdir("/srv", 0777);
		bind("#s", "/srv", MREPL | MCREATE);
		mkdir("/mnt", 0777);
	}

	if(have_image){
		fd = open(untar_name[0] != 0 ? "/initramfs/runner-image" :
		    "/runner-image", O_WRONLY | O_CREAT | O_TRUNC, 0777);
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
	}

	for(i = 0; i < (int)(sizeof files / sizeof files[0]); i++)
		if(files[i].path[0] != 0 &&
		    writefile(files[i].path, blob + files[i].offset,
		    files[i].length) < 0){
			fprintf(stderr, "runner: %%s: %%s\n", files[i].path,
			    strerror(errno));
			return 1;
		}
	for(i = 0; envs[i] != 0; i++){
		char name[128];
		const char *eq = strchr(envs[i], '=');
		size_t n;

		if(eq == 0 || (n = eq - envs[i]) >= sizeof name)
			continue;
		memcpy(name, envs[i], n);
		name[n] = 0;
		setenv(name, eq + 1, 1);
	}

	for(i = 0; servers[i] != 0; i++){
		pid_t pid = fork();

		if(pid == 0){
			char *av[2];

			av[0] = (char *)servers[i];
			av[1] = 0;
			execv((char *)servers[i], av);
			fprintf(stderr, "runner: server %%s: %%s\n", servers[i],
			    strerror(errno));
			_exit(127);
		}
		if(pid < 0)
			fprintf(stderr, "runner: server fork: %%s\n",
			    strerror(errno));
	}
	for(i = 0; waits[i] != 0; i++){
		int k, ok = 0, fd;

		for(k = 0; k < 1000; k++){
			fd = open((char *)waits[i], O_RDONLY);
			if(fd >= 0){
				close(fd);
				ok = 1;
				break;
			}
			sleep(10);
		}
		if(!ok)
			fprintf(stderr, "runner: wait %%s: timeout\n", waits[i]);
	}

	fprintf(stderr, "runner: wrote %%d files, exec %%s\n", i, args[0]);

	if(args2[0] != 0){
		int st, tfd;
		pid_t pid, got;

		pid = fork();
		if(pid == 0){
			execv(exec_path[0] != 0 ? exec_path :
			    (untar_name[0] != 0 ? "/initramfs/runner-image" :
			    "/runner-image"), args);
			fprintf(stderr, "runner: exec %%s: %%s\n", args[0],
			    strerror(errno));
			_exit(127);
		}
		if(pid < 0){
			fprintf(stderr, "runner: fork: %%s\n", strerror(errno));
			return 1;
		}
		/* APEX wait4, not the Plan 9 libc waitpid: this runner links
		 * libc.a first (for bind/mkdir), and libc's waitpid takes no
		 * arguments and reaps any child, so it cannot report status. */
		st = 0;
		got = wait4(pid, &st, 0, 0);
		if(got < 0){
			fprintf(stderr, "runner: wait4: %%s\n", strerror(errno));
			return 1;
		}
		fprintf(stderr, "runner: wait4 got %%d for %%d\n", got, pid);
		if(WIFSIGNALED(st))
			fprintf(stderr, "runner: %%s killed by signal %%d (status %%#x)\n",
			    args[0], WTERMSIG(st), st);
		else
			fprintf(stderr, "runner: %%s exited %%d (status %%#x)\n",
			    args[0], WEXITSTATUS(st), st);
		tfd = open(args2[0], O_RDONLY);
		fprintf(stderr, "runner: next %%s open %%d\n", args2[0], tfd);
		if(tfd >= 0)
			close(tfd);
		if(!WIFEXITED(st) || WEXITSTATUS(st) != 0)
			return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
		execv(args2[0], args2);
		fprintf(stderr, "runner: exec %%s: %%s\n", args2[0],
		    strerror(errno));
		return 1;
	}

	execv(exec_path[0] != 0 ? exec_path :
	    (untar_name[0] != 0 ? "/initramfs/runner-image" :
	    "/runner-image"), args);
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


def argv_init(items: list[str]) -> str:
    if not items:
        return "\t0"
    return ",\n".join("\t" + c_string(a) for a in items) + ",\n\t0"


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
    ap.add_argument("--env", action="append", default=[],
                    metavar="NAME=VALUE",
                    help="variable to put in the guest environment")
    ap.add_argument("--in-tree", default="",
                    metavar="PATH",
                    help="run PATH from the unpacked tree instead of "
                         "writing the embedded image (argv[0] becomes PATH)")
    ap.add_argument("--untar", default="",
                    metavar="BOOTFILE:DEST",
                    help="unpack the ustar archive at BOOTFILE (a boot "
                         "file, e.g. /boot/sdk.tar) into DEST before running")
    ap.add_argument("--bootfs", default="",
                    metavar="PATH",
                    help="bind the boot filesystem at PATH so a boot "
                         "archive file stays reachable")
    ap.add_argument("--server", action="append", default=[],
                    metavar="PATH",
                    help="program to start in the background before the "
                         "target (repeatable)")
    ap.add_argument("--wait", action="append", default=[],
                    metavar="PATH",
                    help="file to wait for after starting servers "
                         "(repeatable)")
    ap.add_argument("--then", action="append", default=[],
                    help="program to run after the first one succeeds; "
                         "repeatable, executed in order")
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
    if opts.in_tree:
        argv[0] = opts.in_tree
    src = OUT / "runner.c"
    OUT.mkdir(parents=True, exist_ok=True)
    then = opts.then[0] if opts.then else ""
    then_args = [then] if then else []
    untar_name, untar_dest = "", ""
    if opts.untar:
        untar_name, untar_dest = opts.untar.split(":", 1)
    src.write_text(RUNNER % (
        emit(image),
        emit(bytes(blob)),
        ",\n".join(f'\t{c_string(p)}, {o}, {n}' for p, o, n in table) or
        '\t{"", 0, 0}',
        argv_init(argv),
        argv_init(then_args),
        1 if not opts.in_tree else 0, c_string(opts.in_tree),
        c_string(untar_name), c_string(untar_dest),
        (",\n".join("\t" + c_string(e) for e in opts.env) + ",\n\t0")
        if opts.env else "\t0",
        (",\n".join("\t" + c_string(v) for v in opts.server) + ",\n\t0")
        if opts.server else "\t0",
        (",\n".join("\t" + c_string(v) for v in opts.wait) + ",\n\t0")
        if opts.wait else "\t0",
        c_string(opts.bootfs)))

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
