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

/*
 * lklrun: start the LKL driver server as a child of the shell (it is a
 * boot file, not init), then stream /dev/lkllog to the console.
 */
void
main(void)
{
	char log[] = "/dev/lkllog", path[] = "/boot/lklboot", debug[] = "-d";
	char *av[] = {path, HOST_ARG KT_ARG DEBUG_ARG nil};
	char buf[4096];
	int fd, n, pid, i;

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


FAMILY_LAUNCHER = r'''#include <u.h>
#include <libc.h>

/*
 * lkl<family>: one LKL instance per driver family.  The family tables
 * and device tree are inside /boot/lklboot.
 */
void
main(void)
{
	char *av[] = {"/boot/lklboot", "-f", "FAMILY", nil};

	exec("/boot/lklboot", av);
	print("LKL-FAMILY exec failed: %r\n");
	exits("exec");
}
'''


def make_archive(out: Path, elf: Path, libc: Path, debug: bool,
                 settle_ms: int = 5000, hosttest: bool = False,
                 kerneltest: bool = False, noemmc: bool = False) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    source = LAUNCHER.replace("DEBUG_ARG", "debug," if debug else "")
    source = source.replace("HOST_ARG", '"-s",' if hosttest else "")
    source = source.replace("KT_ARG", '"-t",' if kerneltest else "")
    source = source.replace("SETTLE_MS", str(settle_ms))
    launcher, _ = build_userland.build_program(out, "lklrun", source,
                                              0x200000, libc)
    fams = {}
    for fam in ("emmc", "usb"):
        src = FAMILY_LAUNCHER.replace("FAMILY", fam)
        fams[fam], _ = build_userland.build_program(
            out, "lkl" + fam, src, 0x200000, libc)
    # The LKL eMMC family is a validation harness: only its archive asks
    # init to skip the native emmcfs/gefs stack.  A production archive
    # keeps native eMMC and starts lklusb (or another family) alongside it.
    files = [("lklrun", launcher), ("lklboot", elf.read_bytes()),
             ("lklemc", fams["emmc"]), ("lklusb", fams["usb"])]
    if noemmc:
        files.append(("noemmc", b"1\n"))
    offset = 24 + 48 * len(files)
    total = offset + sum(len(data) for _, data in files)
    table, body = bytearray(), bytearray()
    for name, data in files:
        table += struct.pack("<32sIIII", name.encode(), offset,
                             len(data), 0o555, 0)
        body += data
        offset += len(data)
    archive = out / "lkl-test.img"
    archive.write_bytes(struct.pack("<8sIIII", b"O9BOOT01", 1, len(files),
                                    total, 0) + table + body)
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
    ap.add_argument("--kerneltest", action="store_true",
                    help="run the kernel-side nanosleep selftest (-t) after boot")
    ap.add_argument("--noemmc", action="store_true",
                    help="let the LKL emmc family own the controller "
                         "(skip native emmcfs/gefs)")
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
                           args.settle_ms, args.hosttest, args.kerneltest,
                           args.noemmc)
    if args.archive_only:
        print(archive)
        return
    command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", args.cpu,
               "-m", "768M", "-nographic", "-monitor", "none", "-kernel",
               str(args.kernel), "-device",
               f"loader,file={archive},addr=0x49000000"]
    # LKL is a boot file started from the shell (not init): give the guest
    # time to reach rc, then type "lklrun".
    send = [(25.0, "/boot/lklrun\n")]
    transcript = boot_until(command, "lklboot: halted", idle=args.deadline,
                            total=max(args.deadline, 120),
                            limit=4 * 1024 * 1024, send=send)
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
