#!/usr/bin/env python3
"""Boot an existing LKL probe under an ARM64 Harvey kernel in QEMU.

Supply --kernel explicitly: this test uses an external archive and never
replaces the kernel's generated userland headers. --archive-only prepares
the same archive for a board; --debug enables lklboot's host wait trace.
"""
import argparse
from pathlib import Path
import struct

import build_userland
from test_virt import boot_until


LAUNCHER = r'''#include <u.h>
#include <libc.h>

void
main(void)
{
	char cons[] = "#c/cons", dev[] = "/dev", consdev[] = "#c";
	char log[] = "#c/lkllog", path[] = "/boot/lklboot", debug[] = "-d";
	char *av[] = {path, HOST_ARG DEBUG_ARG nil};
	char buf[4096];
	int fd, n, pid, i;

	open(cons, OREAD);
	open(cons, OWRITE);
	open(cons, OWRITE);
	if(bind(consdev, dev, MREPL) < 0){
		print("LKL-TEST bind failed: %r\n");
		exits("bind");
	}
	pid = rfork(RFPROC|RFFDG);
	if(pid == 0){
		exec(path, av);
		print("LKL-TEST exec failed: %r\n");
		exits("exec");
	}
	if(pid < 0){
		print("LKL-TEST rfork failed: %r\n");
		exits("rfork");
	}
	/* Leave boot undisturbed before reading diagnostics: UART output can
	 * hide host/guest scheduling races. Never wait for a stalled child. */
	sleep(SETTLE_MS);
	fd = open(log, OREAD);
	if(fd < 0){
		print("LKL-TEST log open failed: %r\n");
		exits("log");
	}
	for(i = 0; i < 300; i++){
		while((n = read(fd, buf, sizeof buf)) > 0)
			write(1, buf, n);
		sleep(100);
	}
	print("LKL-TEST observation finished\n");
	for(;;)
		sleep(1000);
}
'''


def make_archive(out: Path, elf: Path, libc: Path, debug: bool,
                 settle_ms: int = 5000, hosttest: bool = False) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    source = LAUNCHER.replace("DEBUG_ARG", "debug," if debug else "")
    source = source.replace("HOST_ARG", '"-s",' if hosttest else "")
    source = source.replace("SETTLE_MS", str(settle_ms))
    launcher, _ = build_userland.build_program(out, "init", source,
                                              0x200000, libc)
    files = [("init", launcher), ("lklboot", elf.read_bytes())]
    offset = 24 + 48 * len(files)
    table, body = bytearray(), bytearray()
    for name, data in files:
        table += struct.pack("<32sIIII", name.encode(), offset,
                             len(data), 0o555, 0)
        body += data
        offset += len(data)
    archive = out / "lkl-test.img"
    archive.write_bytes(struct.pack("<8sIIII", b"O9BOOT01", 1, len(files),
                                    offset, 0) + table + body)
    return archive


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kernel", type=Path)
    ap.add_argument("--elf", type=Path, required=True)
    ap.add_argument("--libc", type=Path,
                    default=build_userland.ROOT / "build/arm64-libc/libc.a")
    ap.add_argument("--out", type=Path,
                    default=build_userland.ROOT / "build/arm64-lkl-test")
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--hosttest", action="store_true",
                    help="run the host-layer selftest (-s) instead of LKL")
    ap.add_argument("--archive-only", action="store_true")
    ap.add_argument("--cpu", default="cortex-a55")
    ap.add_argument("--deadline", type=float, default=40)
    ap.add_argument("--settle-ms", type=int, default=5000,
                    help="leave guest quiet before reading its log (default: 5000)")
    args = ap.parse_args()
    if not args.archive_only and args.kernel is None:
        ap.error("--kernel is required unless --archive-only is used")
    if not 0 <= args.settle_ms <= 300000:
        ap.error("--settle-ms must be between 0 and 300000")
    archive = make_archive(args.out, args.elf, args.libc, args.debug,
                           args.settle_ms, args.hosttest)
    if args.archive_only:
        print(archive)
        return
    command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", args.cpu,
               "-m", "768M", "-nographic", "-monitor", "none", "-kernel",
               str(args.kernel), "-device",
               f"loader,file={archive},addr=0x50000000"]
    transcript = boot_until(command, "lklboot: halted", idle=args.deadline,
                            total=args.deadline, limit=4 * 1024 * 1024)
    log = args.out / "boot.log"
    log.write_text(transcript)
    markers = ("Linux version", "lklboot: kernel up", "lklboot: halted")
    missing = [marker for marker in markers if marker not in transcript]
    if missing:
        raise SystemExit(f"LKL boot failed: missing {missing}; see {log}")
    if "WARNING: CPU:" in transcript or "lkl: bug" in transcript or "lkl: panic" in transcript:
        raise SystemExit(f"LKL boot reported a kernel warning or failure; see {log}")
    print(f"LKL boot and halt PASS: {log}")


if __name__ == "__main__":
    main()
