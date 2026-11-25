#!/usr/bin/env python3
"""Check #P's compound-device model: nesting, windows and groups.

Boots an ARM64 Harvey kernel under QEMU with the virt DTB rewritten so
that it exercises the three ways a compound device can be described:

  * parent@9010000 (the PL031 RTC) holds child@9030000 (the PL061 GPIO
    registers, presented as a pl031 so it is classified) inside it: a
    claim on the parent must cover the child, and the child must not be
    claimable while the parent is held.
  * parent@9010000 carries a second reg window (0x9000000): #P's
    windows file must list the mapped region and both windows.
  * both nodes carry harvey,group = "g0": claiming #P/group/g0 must hold
    both members at once, and neither member may be claimed separately
    while the group is held.

Supply the kernel image with --kernel.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

import build_userland
from test_virt import boot_until

PNPTEST = r'''#include <u.h>
#include <libc.h>

static int nfail;

static void
fail(char *fmt, ...)
{
	char buf[512];
	va_list arg;
	va_start(arg, fmt);
	vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	print("PNP-FEATURES FAIL %s\n", buf);
	nfail++;
}

/* One-line file contents, or "" when it cannot be read. */
static char*
readf(char *path)
{
	static char buf[512];
	int fd, n;

	buf[0] = 0;
	if((fd = open(path, OREAD)) < 0)
		return buf;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		return buf;
	buf[n] = 0;
	if(n > 0 && buf[n-1] == '\n')
		buf[n-1] = 0;
	return buf;
}

static int
claimf(char *path, char *who)
{
	int fd;

	if((fd = open(path, OWRITE)) < 0)
		return -1;
	if(write(fd, who, strlen(who)) != strlen(who)){
		close(fd);
		return -1;
	}
	return fd;
}

static void
expect(char *what, char *got, char *want)
{
	if(strcmp(got, want) != 0)
		fail("%s: got '%s' want '%s'", what, got, want);
}

/* First field of line n (0-based, '\n' separated) of buf, or ~0. */
static u64
field(char *buf, int line)
{
	char *p, *e;
	int i;

	p = buf;
	for(i = 0; i < line; i++){
		if((p = strchr(p, '\n')) == nil)
			return ~0ULL;
		p++;
	}
	while(*p == ' ')
		p++;
	e = p;
	while(*e != 0 && *e != ' ' && *e != '\n')
		e++;
	if(*e != 0)
		*e = 0;
	return strtoull(p, nil, 0);
}

void
main(void)
{
	char path[128], buf[512];
	char *p, *c;
	int fd, pfd, gfd;
	u64 v;

	p = "parent@9010000";
	c = "child@9030000";

	/* The hierarchy is visible. */
	snprint(path, sizeof path, "#P/%s/parent", c);
	expect("child parent", readf(path), p);
	snprint(path, sizeof path, "#P/%s/parent", p);
	expect("parent parent", readf(path), "");

	/* A claim on the parent covers the child. */
	snprint(path, sizeof path, "#P/%s/claim", p);
	if((pfd = claimf(path, "feat")) < 0)
		fail("claim parent");
	snprint(path, sizeof path, "#P/%s/owner", c);
	expect("child owner while parent held", readf(path), "feat");
	snprint(path, sizeof path, "#P/%s/regs", c);
	if((fd = open(path, OREAD)) < 0)
		fail("child regs refused while parent held");
	else{
		/* PL061 ID register */
		if(pread(fd, buf, 4, 0xfe0) != 4)
			fail("child regs read");
		else if((buf[0] & 0xff) != 0x61)
			fail("child regs id %02x", buf[0] & 0xff);
		close(fd);
	}

	/* The child cannot be claimed while the parent is held. */
	snprint(path, sizeof path, "#P/%s/claim", c);
	if((fd = claimf(path, "feat")) >= 0){
		fail("child claim granted under a parent claim");
		close(fd);
	}
	close(pfd);
	snprint(path, sizeof path, "#P/%s/owner", c);
	expect("child owner after release", readf(path), "");

	/* The mapped region, then both windows. */
	snprint(path, sizeof path, "#P/%s/windows", p);
	strncpy(buf, readf(path), sizeof buf - 1);
	if((v = field(buf, 0)) != 0x9000000ULL)
		fail("map base %#llux", v);
	snprint(path, sizeof path, "#P/%s/windows", p);
	strncpy(buf, readf(path), sizeof buf - 1);
	if((v = field(buf, 1)) != 0x9010000ULL)
		fail("window 0 %#llux", v);
	snprint(path, sizeof path, "#P/%s/windows", p);
	strncpy(buf, readf(path), sizeof buf - 1);
	if((v = field(buf, 2)) != 0x9000000ULL)
		fail("window 1 %#llux", v);

	/* The group holds both members at once. */
	snprint(path, sizeof path, "#P/group/g0/members");
	strncpy(buf, readf(path), sizeof buf - 1);
	if(strstr(buf, p) == nil || strstr(buf, c) == nil)
		fail("group members '%s'", buf);
	snprint(path, sizeof path, "#P/group/g0/owner");
	expect("group owner before", readf(path), "");
	snprint(path, sizeof path, "#P/group/g0/claim");
	if((gfd = claimf(path, "grp")) < 0)
		fail("claim group");
	snprint(path, sizeof path, "#P/group/g0/owner");
	expect("group owner", readf(path), "grp");
	snprint(path, sizeof path, "#P/%s/owner", p);
	expect("parent owner under group", readf(path), "grp");
	snprint(path, sizeof path, "#P/%s/owner", c);
	expect("child owner under group", readf(path), "grp");
	snprint(path, sizeof path, "#P/%s/claim", p);
	if((fd = claimf(path, "grp")) >= 0){
		fail("member claim granted under a group claim");
		close(fd);
	}
	close(gfd);
	snprint(path, sizeof path, "#P/%s/owner", p);
	expect("parent owner after group release", readf(path), "");
	snprint(path, sizeof path, "#P/group/g0/owner");
	expect("group owner after release", readf(path), "");

	if(nfail == 0)
		print("PNP-FEATURES PASS\n");
	else
		print("PNP-FEATURES FAILED %d\n", nfail);
	exits(nil);
}
'''


def build_dtb(out: Path) -> Path:
    """QEMU virt's DTB, rewritten for the compound-device cases."""
    base = out / "virt-base.dtb"
    subprocess.run(["qemu-system-aarch64", "-machine",
                    f"virt,dumpdtb={base}", "-cpu", "cortex-a55",
                    "-m", "768M", "-nographic"],
                   check=True, capture_output=True)
    dts = out / "features.dts"
    subprocess.run(["dtc", "-q", "-I", "dtb", "-O", "dts", "-o", dts, base],
                   check=True)
    text = dts.read_text()
    old = """	pl031@9010000 {
		clock-names = "apb_pclk";
		clocks = <0x8000>;
		interrupts = <0x00 0x02 0x04>;
		reg = <0x00 0x9010000 0x00 0x1000>;
		compatible = "arm,pl031\\0arm,primecell";
	};"""
    if old not in text:
        raise SystemExit("virt DTB layout changed: pl031@9010000 not found")
    new = """	parent@9010000 {
		clock-names = "apb_pclk";
		clocks = <0x8000>;
		interrupts = <0x00 0x02 0x04>;
		reg = <0x00 0x9010000 0x00 0x1000 0x00 0x9000000 0x00 0x1000>;
		compatible = "arm,pl031\\0arm,primecell";
		harvey,group = "g0";

		child@9030000 {
			compatible = "arm,pl031";
			reg = <0x00 0x9030000 0x00 0x1000>;
			interrupts = <0x00 0x07 0x04>;
			harvey,group = "g0";
		};
	};"""
    dts.write_text(text.replace(old, new, 1))
    dtb = out / "features.dtb"
    subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", dtb, dts],
                   check=True)
    return dtb


def make_archive(out: Path, libc: Path) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    prog, _ = build_userland.build_program(out, "pnpfeat", PNPTEST,
                                          0x200000, libc)
    import struct
    files = [("pnpfeat", prog)]
    offset = 24 + 48 * len(files)
    total = offset + sum(len(data) for _, data in files)
    table, body = bytearray(), bytearray()
    for name, data in files:
        table += struct.pack("<32sIIII", name.encode(), offset,
                             len(data), 0o555, 0)
        body += data
        offset += len(data)
    archive = out / "pnp-features.img"
    archive.write_bytes(struct.pack("<8sIIII", b"O9BOOT01", 1, len(files),
                                    total, 0) + table + body)
    return archive


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kernel", type=Path, required=True)
    ap.add_argument("--libc", type=Path,
                    default=build_userland.ROOT / "build/arm64-libc/libc.a")
    ap.add_argument("--out", type=Path,
                    default=build_userland.ROOT / "build/arm64-pnp-features")
    args = ap.parse_args()
    for tool in ("qemu-system-aarch64", "dtc"):
        if shutil.which(tool) is None:
            raise SystemExit(f"{tool} is required")
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    dtb = build_dtb(out)
    archive = make_archive(out, args.libc)
    command = ["qemu-system-aarch64", "-machine", "virt", "-cpu",
               "cortex-a55", "-m", "768M", "-nographic", "-monitor",
               "none", "-dtb", str(dtb), "-kernel", str(args.kernel),
               "-device", f"loader,file={archive},addr=0x49000000"]
    send = [(50.0, "/boot/pnpfeat\n")]
    transcript = boot_until(command, "PNP-FEATURES PASS", idle=90,
                            total=180, limit=4 * 1024 * 1024, send=send)
    log = out / "boot.log"
    log.write_text(transcript)
    if "PNP-FEATURES PASS" not in transcript:
        fails = [l for l in transcript.splitlines() if "PNP-FEATURES" in l]
        raise SystemExit("#P compound test failed:\n" + "\n".join(fails) +
                         f"\nsee {log}")
    print(f"#P compound devices PASS: {log}")


if __name__ == "__main__":
    sys.exit(main())
