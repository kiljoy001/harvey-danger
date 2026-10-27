#!/usr/bin/env python3
"""Build the first filesystem-backed ARM64 user processes for Harvey."""

from pathlib import Path
import argparse
import os
import shutil
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM64_LD") or shutil.which("ld.lld") or "ld.lld"
NM = shutil.which("llvm-nm-23") or shutil.which("llvm-nm") or "llvm-nm"
STUB_BASE = 0x4000000000
INIT_BASE = 0x200000
BIG_PAGE = 0x200000
O9DRVMAN_BASE = 0x600000
UARTFS_BASE = 0x1600000
GPIOFS_BASE = 0x1A00000
HUBFS_BASE = 0x1E00000
DRVMAN_BASE = 0x2200000
I2CFS_BASE = 0x2600000
RC_BASE = 0x2A00000
ECHO_BASE = 0x2E00000
CAT_BASE = 0x3200000
O9DISPLAY_BASE = 0x3A00000
DISPLAYTEST_BASE = 0x4200000
O9ASPERA_SCREEN_BASE = 0x4E00000
O9XHCI_BASE = 0x5600000
TESTXHCI_BASE = 0x5E00000
TESTXHCI1_BASE = 0x6600000
TESTAUDIO_BASE = 0x6E00000
USBPOWER_BASE = 0x7600000

RC_SOURCES = [
    "code.c", "exec.c", "getflags.c", "glob.c", "havefork.c", "here.c",
    "io.c", "lex.c", "pcmd.c", "pfnc.c", "plan9.c", "simple.c", "subr.c",
    "trap.c", "tree.c", "var.c",
]

# rc only needs this closure of libString for backquote handling and
# variable growth; pulling in the whole library would also pull in libbio.
RC_STRING_SOURCES = [
    "s_alloc.c", "s_nappend.c", "s_reset.c", "s_unique.c", "s_terminate.c",
    "s_putc.c", "s_grow.c", "s_copy.c",
]


def symbol_value(elf: Path, name: str) -> int:
    output = subprocess.run([NM, "-P", str(elf)], check=True, text=True,
                            capture_output=True).stdout
    for line in output.splitlines():
        fields = line.split()
        if fields and fields[0] == name:
            return int(fields[2], 16)
    raise RuntimeError(f"missing symbol {name} in {elf}")


def validate_elf(path: Path, base: int) -> tuple[bytes, int]:
    image = path.read_bytes()
    if len(image) < 64 or image[:7] != b"\x7fELF\x02\x01\x01":
        raise RuntimeError(f"{path} is not a little-endian ELF64 image")
    machine, = struct.unpack_from("<H", image, 18)
    entry, phoff = struct.unpack_from("<QQ", image, 24)
    phentsize, phnum = struct.unpack_from("<HH", image, 54)
    if machine != 183 or phentsize != 56 or phnum == 0:
        raise RuntimeError(f"{path} has an invalid AArch64 program header")
    executable = False
    writable = False
    for index in range(phnum):
        off = phoff + index * phentsize
        ptype, flags, fileoff, va, _, filesz, memsz, align = struct.unpack_from(
            "<IIQQQQQQ", image, off)
        if ptype != 1:
            continue
        if filesz > memsz or fileoff + filesz > len(image):
            raise RuntimeError(f"{path} has an invalid load segment")
        if flags & 1:
            executable = executable or base <= entry < base + BIG_PAGE
            if not base <= va or va + memsz > base + BIG_PAGE:
                raise RuntimeError(f"{path} executable segment exceeds one big page")
        if flags & 2:
            writable = True
            if not base + BIG_PAGE <= va or va + memsz > base + 2 * BIG_PAGE:
                raise RuntimeError(f"{path} writable segment exceeds one big page")
        if align > BIG_PAGE:
            raise RuntimeError(f"{path} requests unsupported alignment {align:#x}")
    if not executable or not writable:
        raise RuntimeError(f"{path} needs separate executable and writable segments")
    return image, entry


def build_program(out: Path, name: str, source_text: str, base: int,
                  libc: Path) -> tuple[bytes, int]:
    source = out / f"{name}.c"
    script = out / f"{name}.ld"
    obj = out / f"{name}.o"
    elf = out / name
    source.write_text(source_text)
    script.write_text(f'''ENTRY(_main)
SECTIONS {{
  . = 0x{base:x};
  .text : {{ *(.text .text.*) *(.rodata .rodata.*) }}
  . = ALIGN(0x{BIG_PAGE:x});
  .data : {{ *(.data .data.*) }}
  .bss : {{ *(.bss .bss.* COMMON) }}
  PROVIDE(end = .);
}}
''')
    subprocess.run([
        CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
        "-mstrict-align", "-O2",
        "-I", str(ROOT / "aarch64/include"),
        "-I", str(ROOT / "sys/include"), "-c", str(source), "-o", str(obj),
    ], check=True)
    subprocess.run([
        LD, "-z", "max-page-size=4096", "-T", str(script),
        "-e", "_main", "-u", "_main", str(obj), str(libc), "-o", str(elf),
    ], check=True)
    image, entry = validate_elf(elf, base)
    if symbol_value(elf, "_main") != entry:
        raise RuntimeError(f"{name} entry does not name _main")
    return image, entry


def emit_array(lines: list[str], name: str, image: bytes) -> None:
    lines.append(f"static const u8 {name}[{len(image)}] = {{")
    for offset in range(0, len(image), 16):
        chunk = image[offset:offset + 16]
        lines.append("\t" + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    lines.extend(("};", ""))


def build_elf(out: Path, libc: Path, name: str, source: Path,
              base: int, extra: list[Path], ldflags: list[str] = ()) -> tuple[bytes, int]:
    obj = out / f"{name}.o"
    script = out / f"{name}.ld"
    elf = out / name
    script.write_text(f'''ENTRY(_main)
PHDRS {{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}}
SECTIONS {{
  . = 0x{base:x};
  .text : {{ *(.text .text.*) *(.rodata .rodata.*) }} :text
  . = ALIGN(0x{BIG_PAGE:x});
  .data : {{ *(.data .data.*) *(.sdata .sdata.*) *(.got .got.*) }} :data
  .bss : {{ *(.bss .bss.*) *(.sbss .sbss.*) COMMON }} :data
  PROVIDE(end = .);
  /DISCARD/ : {{ *(.comment) *(.note*) }}
}}
''')
    subprocess.run([
        CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
        "-mstrict-align", "-fsigned-char", "-fcommon", "-O2",
        "-I", str(ROOT / "aarch64/include"),
        "-I", str(ROOT / "sys/include"), "-c", str(source), "-o", str(obj),
    ], check=True)
    libs = [*extra, libc]
    subprocess.run([
        LD, "-static", "-z", "max-page-size=4096", "-T", str(script),
        "--allow-multiple-definition", *ldflags,
        "-e", "_main", "-u", "_main", str(obj),
        "--start-group", *map(str, libs), "--end-group",
        "-o", str(elf),
    ], check=True)
    image, entry = validate_elf(elf, base)
    if symbol_value(elf, "_main") != entry:
        raise RuntimeError(f"{name} entry does not name _main")
    return image, entry


def build_fs(out: Path, libc: Path, name: str, source: Path,
             base: int) -> tuple[bytes, int]:
    o9 = ROOT / "build/arm64-o9"
    health = out / "driverhealth.o"
    if not health.exists():
        subprocess.run([
            CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
            "-fno-stack-protector", "-mstrict-align", "-fcommon", "-O2",
            "-D__HARVEY__",
            "-I", str(ROOT / "aarch64/include"),
            "-I", str(ROOT / "sys/include"),
            "-I", str(ROOT / "sys/src/cmd/o9drvman"),
            "-c", str(ROOT / "sys/src/cmd/o9drvman/health.c"),
            "-o", str(health),
        ], check=True)
    extra = [health, o9 / "lib9p.a", o9 / "libthread.a", o9 / "libauth.a",
             o9 / "libauthsrv.a", o9 / "libsec.a", o9 / "libndb.a",
             o9 / "libbio.a"]
    return build_elf(out, libc, name, source, base, extra,
                     ldflags=["--defsym=threadmain=main"])


def build_simple(out: Path, libc: Path, name: str, source: Path,
                 base: int) -> tuple[bytes, int]:
    return build_elf(out, libc, name, source, base, [])


def build_uartfs(out: Path, libc: Path) -> tuple[bytes, int]:
    return build_fs(out, libc, "uartfs",
                    ROOT / "sys/src/cmd/uartfs/uartfs.c", UARTFS_BASE)


def build_gpiofs(out: Path, libc: Path) -> tuple[bytes, int]:
    return build_fs(out, libc, "gpiofs",
                    ROOT / "sys/src/cmd/gpiofs/gpiofs.c", GPIOFS_BASE)


def build_hubfs(out: Path, libc: Path) -> tuple[bytes, int]:
    return build_fs(out, libc, "hubfs",
                    ROOT / "sys/src/cmd/hubfs/hubfs.c", HUBFS_BASE)


def build_drvman(out: Path, libc: Path) -> tuple[bytes, int]:
    return build_simple(out, libc, "drvman",
                        ROOT / "sys/src/cmd/drvman/drvman.c", DRVMAN_BASE)


def build_i2cfs(out: Path, libc: Path) -> tuple[bytes, int]:
    return build_fs(out, libc, "i2cfs",
                    ROOT / "sys/src/cmd/i2cfs/i2cfs.c", I2CFS_BASE)


def build_usbpower(out: Path, libc: Path) -> tuple[bytes, int]:
    return build_fs(out, libc, "usbpower",
                    ROOT / "sys/src/cmd/usbpower/usbpower.c", USBPOWER_BASE)


def build_rc(out: Path, libc: Path) -> tuple[bytes, int]:
    rcsrc = out / "rcsrc"
    rcsrc.mkdir()
    parser = rcsrc / "syn.y"
    shutil.copyfile(ROOT / "sys/src/cmd/rc/syn.y", parser)
    subprocess.run(["yacc", "-d", "-o", "y.tab.c", "syn.y"], cwd=rcsrc,
                   check=True, text=True, capture_output=True)
    shutil.copyfile(rcsrc / "y.tab.h", rcsrc / "x.tab.h")

    flags = [
        CC, "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
        "-mstrict-align", "-fsigned-char", "-fcommon", "-O2", "-D__HARVEY__",
        "-I", str(rcsrc), "-I", str(ROOT / "sys/src/cmd/rc"),
        "-I", str(ROOT / "aarch64/include"), "-I", str(ROOT / "sys/include"),
    ]
    objects = []
    sources = [ROOT / "sys/src/cmd/rc" / name for name in RC_SOURCES]
    sources.append(rcsrc / "y.tab.c")
    for source in sources:
        obj = out / f"rc-{source.stem}.o"
        subprocess.run([*flags, "-c", str(source), "-o", str(obj)], check=True)
        objects.append(obj)
    for name in RC_STRING_SOURCES:
        obj = out / f"rc-{Path(name).stem}.o"
        subprocess.run([*flags, "-I", str(ROOT / "sys/src/libString"),
                        "-c", str(ROOT / "sys/src/libString" / name),
                        "-o", str(obj)], check=True)
        objects.append(obj)

    script = out / "rc.ld"
    elf = out / "rc"
    script.write_text(f'''ENTRY(_main)
PHDRS {{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}}
SECTIONS {{
  . = 0x{RC_BASE:x};
  .text : {{ *(.text .text.*) *(.rodata .rodata.*) }} :text
  . = ALIGN(0x{BIG_PAGE:x});
  .data : {{ *(.data .data.*) *(.sdata .sdata.*) *(.got .got.*) }} :data
  .bss : {{ *(.bss .bss.*) *(.sbss .sbss.*) COMMON }} :data
  PROVIDE(end = .);
  /DISCARD/ : {{ *(.comment) *(.note*) }}
}}
''')
    subprocess.run([
        LD, "-static", "-z", "max-page-size=4096", "-T", str(script),
        "-e", "_main", "-u", "_main", *map(str, objects),
        "--start-group", str(libc), "--end-group", "-o", str(elf),
    ], check=True)
    image, entry = validate_elf(elf, RC_BASE)
    if symbol_value(elf, "_main") != entry:
        raise RuntimeError("rc entry does not name _main")
    return image, entry


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    subprocess.run([sys.executable,
                    str(ROOT / "util/arm64/build_o9_services.py")],
                   check=True, stdout=subprocess.DEVNULL)
    out = args.output.parent / "userland"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    libc = ROOT / "build/arm64-libc/libc.a"
    stub, stub_entry = build_program(out, "execstub", r'''#include <u.h>
#include <libc.h>

void
main(int argc, char **argv)
{
	char *initargv[] = { "/boot/init", nil };
	char message[] = "HARVEY-ARM64 FILE-EXEC FAIL\n";
	int fd;

	USED(argc);
	USED(argv);
	exec(initargv[0], initargv);
	fd = open("#c/cons", OWRITE);
	if(fd >= 0)
		write(fd, message, sizeof(message)-1);
	_exits("exec");
}
''', STUB_BASE, libc)
    init, init_entry = build_program(out, "init", r'''#include <u.h>
#include <libc.h>

static void
say(char *message)
{
	write(1, message, strlen(message));
}

static int
waitfor(char *path, int tries)
{
	int fd;

	while(tries-- > 0){
		fd = open(path, OREAD);
		if(fd >= 0){
			close(fd);
			return 0;
		}
		sleep(100);
	}
	return -1;
}

static int
hubtest(void)
{
	char wbuf[] = "hubfs-ok\n";
	char rbuf1[32], rbuf2[32];
	int mfd, wfd, rfd1, rfd2, n1, n2;

	mfd = open("/srv/hubfs", OREAD);
	if(mfd < 0)
		return -1;
	create("/mnt/hub", OREAD, DMDIR|0755);
	if(mount(mfd, -1, "/mnt/hub", MREPL | MCREATE, "") < 0){
		close(mfd);
		return -1;
	}
	close(mfd);
	mfd = create("/mnt/hub/console", OWRITE, 0666);
	if(mfd < 0)
		return -1;
	close(mfd);
	wfd = open("/mnt/hub/console", OWRITE);
	if(wfd < 0)
		return -1;
	if(write(wfd, wbuf, sizeof(wbuf)-1) != sizeof(wbuf)-1){
		close(wfd);
		return -1;
	}
	close(wfd);
	/* two independent readers must each replay the stream (broadcast) */
	rfd1 = open("/mnt/hub/console", OREAD);
	rfd2 = open("/mnt/hub/console", OREAD);
	if(rfd1 < 0 || rfd2 < 0)
		return -1;
	n1 = read(rfd1, rbuf1, sizeof(rbuf1)-1);
	n2 = read(rfd2, rbuf2, sizeof(rbuf2)-1);
	close(rfd1);
	close(rfd2);
	if(n1 != sizeof(wbuf)-1 || memcmp(rbuf1, wbuf, n1) != 0 ||
	   n2 != sizeof(wbuf)-1 || memcmp(rbuf2, wbuf, n2) != 0)
		return -1;
	return 0;
}

void
main(int argc, char **argv)
{
	long m;
	int fd, pid;

	USED(argc);
	USED(argv);
	open("#c/cons", OREAD);
	open("#c/cons", OWRITE);
	open("#c/cons", OWRITE);
	say("HARVEY-ARM64 FILE-EXEC INIT PASS\n");
	if(bind("#c", "/dev", MAFTER) < 0){
		say("HARVEY-ARM64 DRIVER-NAMESPACE FAIL\n");
		_exits("namespace");
	}
	say("HARVEY-ARM64 DRIVER-DEV PASS\n");
	if(bind("#V", "/dev", MAFTER) < 0)
		say("HARVEY-ARM64 DISPLAY-BIND FAIL\n");
	else
		say("HARVEY-ARM64 DISPLAY-BIND PASS\n");
	if(bind("#e", "/env", MCREATE|MAFTER) < 0 ||
	   bind("#s", "/srv", MREPL|MCREATE) < 0 ||
	   bind("#@", "/mnt", MREPL|MCREATE) < 0){
		say("HARVEY-ARM64 DRIVER-NAMESPACE FAIL\n");
		_exits("namespace");
	}
	say("HARVEY-ARM64 DRIVER-NAMESPACE PASS\n");
	fd = create("/mnt/o9drvman-status", OREAD, DMDIR|0755);
	if(fd >= 0)
		close(fd);
	{
		char *drvmansrv[] = { "/boot/drvman", "drvman", nil };
		char status[16];
		pid = rfork(RFPROC|RFNOWAIT|RFFDG|RFNAMEG|RFENVG|RFREND|RFNOTEG);
		if(pid == 0){
			exec(drvmansrv[0], drvmansrv);
			say("HARVEY-ARM64 DRVMAN EXEC FAIL\n");
			_exits("exec");
		}
		if(pid < 0){
			say("HARVEY-ARM64 DRIVER-MANAGER FAIL\n");
			_exits("manager");
		}
		status[0] = 0;
		for(fd = 0; fd < 100; fd++){
			int sfd = open("/mnt/o9drvman-status/complete", OREAD);
			if(sfd >= 0){
				m = read(sfd, status, sizeof status - 1);
				close(sfd);
				if(m > 0){
					status[m] = 0;
					break;
				}
			}
			sleep(50);
		}
		if(strncmp(status, "pass", 4) != 0){
			say("HARVEY-ARM64 DRIVER-MANAGER FAIL\n");
			_exits("manager");
		}
	}
	say("HARVEY-ARM64 DRIVER-MANAGER PASS\n");
	{
		char *hubsrv[] = { "/boot/hubfs", "hubfs", nil };
		pid = rfork(RFPROC|RFNOWAIT|RFFDG|RFNAMEG|RFENVG|RFREND|RFNOTEG);
		if(pid == 0){
			exec(hubsrv[0], hubsrv);
			_exits("exec");
		}
		if(pid < 0 || waitfor("/srv/hubfs", 50) < 0)
			say("HARVEY-ARM64 HUBFS FAIL\n");
		else if(hubtest() == 0)
			say("HARVEY-ARM64 HUBFS PASS\n");
		else
			say("HARVEY-ARM64 HUBFS FAIL\n");
	}
	{
		char *dispargv[] = { "/boot/o9display", nil };
		pid = rfork(RFPROC|RFNOWAIT|RFFDG|RFENVG|RFREND|RFNOTEG);
		if(pid == 0){
			exec(dispargv[0], dispargv);
			say("HARVEY-ARM64 DISPLAY-SERVICE EXEC FAIL\n");
			_exits("exec");
		}
		if(pid < 0)
			say("HARVEY-ARM64 DISPLAY-SERVICE FAIL\n");
		else
			say("HARVEY-ARM64 DISPLAY-SERVICE LAUNCHED\n");
	}
	{
		char *rcargv[] = { "/boot/rc", "-i", "-m", "/boot/rcmain", nil };
		say("HARVEY-ARM64 RC START\n");
		for(;;){
			pid = rfork(RFPROC|RFFDG|RFNOTEG);
			if(pid == 0){
				exec(rcargv[0], rcargv);
				say("HARVEY-ARM64 RC EXEC FAIL\n");
				_exits("rc");
			}
			if(pid < 0){
				say("HARVEY-ARM64 RC LAUNCH FAIL\n");
				break;
			}
			if(waitpid() < 0)
				sleep(1000);
		}
	}
	for(;;)
		sleep(1000);
}
''', INIT_BASE, libc)
    o9drvman, _ = validate_elf(ROOT / "build/arm64-o9/o9drvman", O9DRVMAN_BASE)
    o9display, _ = validate_elf(ROOT / "build/arm64-o9/o9display",
                                O9DISPLAY_BASE)
    asperascreen, _ = validate_elf(ROOT / "build/arm64-o9/o9aspera-screen",
                                   O9ASPERA_SCREEN_BASE)
    o9xhci, _ = validate_elf(ROOT / "build/arm64-o9/o9xhci", O9XHCI_BASE)
    testxhci, _ = validate_elf(ROOT / "build/arm64-o9/test-xhci", TESTXHCI_BASE)
    testxhci1, _ = validate_elf(ROOT / "build/arm64-o9/test-xhci1", TESTXHCI1_BASE)
    testaudio, _ = validate_elf(ROOT / "build/arm64-o9/test-audio", TESTAUDIO_BASE)
    uartfs, _ = build_uartfs(out, libc)
    gpiofs, _ = build_gpiofs(out, libc)
    hubfs, _ = build_hubfs(out, libc)
    drvman, _ = build_drvman(out, libc)
    i2cfs, _ = build_i2cfs(out, libc)
    usbpower, _ = build_usbpower(out, libc)
    rc, rc_entry = build_rc(out, libc)
    echo, _ = build_program(out, "echo", r'''#include <u.h>
#include <libc.h>

void
main(int argc, char **argv)
{
	int i;

	for(i = 1; i < argc; i++){
		if(i > 1)
			write(1, " ", 1);
		write(1, argv[i], strlen(argv[i]));
	}
	write(1, "\n", 1);
	exits(nil);
}
''', ECHO_BASE, libc)
    cat, _ = build_program(out, "cat", r'''#include <u.h>
#include <libc.h>

void
main(int argc, char **argv)
{
	char buf[4096];
	int fd, n, i;

	if(argc < 2){
		fprint(2, "usage: cat file...\n");
		exits("usage");
	}
	for(i = 1; i < argc; i++){
		fd = open(argv[i], OREAD);
		if(fd < 0){
			fprint(2, "cat: %s: %r\n", argv[i]);
			exits("open");
		}
		while((n = read(fd, buf, sizeof buf)) > 0)
			if(write(1, buf, n) != n){
				fprint(2, "cat: write: %r\n");
				exits("write");
			}
		close(fd);
	}
	exits(nil);
}
''', CAT_BASE, libc)
    displaytest, _ = build_program(out, "displaytest", r'''#include <u.h>
#include <libc.h>

static int
wr(char *path, char *text)
{
	int fd, n, len;

	len = strlen(text);
	fd = open(path, OWRITE);
	if(fd < 0)
		return -1;
	n = write(fd, text, len);
	close(fd);
	return n == len ? 0 : -1;
}

void
main(int argc, char **argv)
{
	int fd;
	char *mnt = "/mnt/display";

	USED(argv);
	if(argc > 1 && strcmp(argv[1], "peek") == 0){
		long off, at;
		u32 pixel;
		fd = open("/dev/display/data", OREAD);
		if(fd < 0){
			print("DISPLAYTEST PEEK OPEN FAIL\n");
			exits("open");
		}
		for(off = 0; off < 8294400; off += 2073600){
			at = off;
			if(seek(fd, at, 0) >= 0 && read(fd, &pixel, 4) == 4)
				print("DISPLAYTEST PIXEL %ld %ux\n", at, pixel);
		}
		close(fd);
		exits(nil);
	}
	if(argc > 1 && strcmp(argv[1], "scan") == 0){
		int x, y, bad;
		long at;
		u32 pixel;
		fd = open("/dev/display/data", OREAD);
		if(fd < 0){
			print("DISPLAYTEST SCAN OPEN FAIL\n");
			exits("open");
		}
		bad = 0;
		for(y = 0; y < 1080; y += 60){
			for(x = 0; x < 1920; x += 60){
				at = (long)y * 7680 + (long)x * 4;
				if(seek(fd, at, 0) >= 0 && read(fd, &pixel, 4) == 4 &&
				   pixel != 0xff00ff00){
					if(bad < 40)
						print("DISPLAYTEST BLACK %d %d %ux\n",
						      x, y, pixel);
					bad++;
				}
			}
		}
		close(fd);
		print("DISPLAYTEST SCAN BAD %d\n", bad);
		exits(nil);
	}
	create(mnt, OREAD, DMDIR|0755);
	fd = open("/srv/DisplayViews", OREAD);
	if(fd < 0 || mount(fd, -1, mnt, MREPL|MCREATE, "") < 0){
		if(fd >= 0)
			close(fd);
		print("DISPLAYTEST MOUNT FAIL\n");
		exits("mount");
	}
	close(fd);
	if(wr("/mnt/display/view/ctl", "buffer 480 270\n") < 0){
		print("DISPLAYTEST BUFFER FAIL\n");
		exits("buffer");
	}
	print("DISPLAYTEST BUFFER PASS\n");
	if(wr("/mnt/display/view/buffers/1/ctl", "clear 65280\n") < 0){
		print("DISPLAYTEST CLEAR FAIL\n");
		exits("clear");
	}
	print("DISPLAYTEST CLEAR PASS\n");
	{
		char path[64], cmd[64];
		int id, row, col;
		id = 1;
		for(row = 0; row < 4; row++){
			for(col = 0; col < 4; col++){
				if(wr("/mnt/display/view/ctl", "surface 480 270\n") < 0){
					print("DISPLAYTEST SURFACE FAIL\n");
					exits("surface");
				}
				snprint(path, sizeof path, "/mnt/display/view/surfaces/%d/ctl", id);
				snprint(cmd, sizeof cmd, "place %d %d\n", col * 480, row * 270);
				if(wr(path, cmd) < 0 || wr(path, "attach 1\n") < 0 ||
				   wr(path, "commit\n") < 0){
					print("DISPLAYTEST TILE FAIL\n");
					exits("tile");
				}
				{
					int dfd;
					u32 pixel;
					long at;
					dfd = open("/dev/display/data", OREAD);
					at = (row * 270 + 135) * 7680 + (col * 480 + 240) * 4;
					if(dfd >= 0 && seek(dfd, at, 0) >= 0 &&
					   read(dfd, &pixel, 4) == 4)
						print("DISPLAYTEST TILE %d %d %d %ux\n",
						      col, row, id, pixel);
					if(dfd >= 0)
						close(dfd);
				}
				id++;
			}
		}
	}
	print("DISPLAYTEST TILES PASS\n");
	fd = open("/dev/display/data", OREAD);
	if(fd >= 0){
		long off, at;
		u32 pixel;
		for(off = 0; off < 8294400; off += 2073600){
			at = off;
			if(seek(fd, at, 0) >= 0 && read(fd, &pixel, 4) == 4)
				print("DISPLAYTEST PIXEL %ld %ux\n", at, pixel);
		}
		close(fd);
	}
	print("DISPLAYTEST PASS\n");
	exits(nil);
}
''', DISPLAYTEST_BASE, libc)
    rcmain = (ROOT / "rc/lib/rcmain").read_text()
    rcmain = rcmain.replace("path=(. /bin)", "path=(/boot . /bin)")
    lines = [
        "/* generated by util/arm64/build_userland.py */",
        f"#define ARM64EXECSTUBBASE 0x{STUB_BASE:x}ULL",
        f"#define ARM64EXECSTUBENTRY 0x{stub_entry:x}ULL",
        f"#define ARM64INITENTRY 0x{init_entry:x}ULL",
        f"#define ARM64RCENTRY 0x{rc_entry:x}ULL",
    ]
    emit_array(lines, "arm64execstubelf", stub)
    emit_array(lines, "arm64initelf", init)
    emit_array(lines, "arm64o9drvmanelf", o9drvman)
    emit_array(lines, "arm64o9displayelf", o9display)
    emit_array(lines, "arm64o9asperascreenelf", asperascreen)
    emit_array(lines, "arm64o9xhcielf", o9xhci)
    emit_array(lines, "arm64testxhcielf", testxhci)
    emit_array(lines, "arm64testxhci1elf", testxhci1)
    emit_array(lines, "arm64testaudioelf", testaudio)
    emit_array(lines, "arm64uartfself", uartfs)
    emit_array(lines, "arm64gpiofself", gpiofs)
    emit_array(lines, "arm64hubfself", hubfs)
    emit_array(lines, "arm64drvmanelf", drvman)
    emit_array(lines, "arm64i2cfself", i2cfs)
    emit_array(lines, "arm64usbpowerelf", usbpower)
    emit_array(lines, "arm64rcelf", rc)
    emit_array(lines, "arm64echoelf", echo)
    emit_array(lines, "arm64catelf", cat)
    emit_array(lines, "arm64displaytestelf", displaytest)
    emit_array(lines, "arm64rcmain", rcmain.encode())
    args.output.write_text("\n".join(lines))


if __name__ == "__main__":
    main()
