#!/usr/bin/env python3
"""Check #P's claims and DMA arena under an ARM64 Harvey kernel in QEMU.

Claims: a device's window, interrupts and regs file are refused without
its claim, which is held through an open file and released by closing it;
exclusive and shared claims exclude each other; regs reads the PL031's ID
registers.  DMA: a chunk is allocated and mapped through its ctl and read
and written through its data file; another process cannot reach it without
its ctl; it is zeroed when allocated and goes back to the arena only once
its files are closed and its mapping gone; the arena as a whole cannot be
attached.  Supply the kernel image with --kernel.
"""
import argparse
from pathlib import Path
import struct

import build_userland
from test_virt import boot_until


PNPTEST = r'''#include <u.h>
#include <libc.h>

static int nfail;

static void
fail(char *fmt, ...)
{
	char buf[256];
	va_list arg;

	va_start(arg, fmt);
	vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	print("PNP-TEST FAIL %s\n", buf);
	nfail++;
}

static u64
avail(void)
{
	char buf[64];
	int fd, n;

	if((fd = open("#P/dma/avail", OREAD)) < 0)
		return 0;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		return 0;
	buf[n] = 0;
	return strtoull(buf, nil, 0);
}

static int
cmd(int ctl, char *s)
{
	return write(ctl, s, strlen(s)) == strlen(s) ? 0 : -1;
}

/* "<n> pa <pa> len <len> <type> va <va>" */
static int
chunkstat(int ctl, int *n, u64 *pa, u64 *len, uchar **va)
{
	char buf[128], *f[8];
	int m;

	m = pread(ctl, buf, sizeof buf - 1, 0);
	if(m <= 0)
		return -1;
	buf[m] = 0;
	if(tokenize(buf, f, nelem(f)) != 8)
		return -1;
	*n = atoi(f[0]);
	*pa = strtoull(f[2], nil, 0);
	*len = strtoull(f[4], nil, 0);
	*va = (uchar*)(uintptr)strtoull(f[7], nil, 0);
	return 0;
}

/* A chunk of size bytes, allocated and mapped. */
static int
chunk(u64 size, u64 align, char *type, int *n, u64 *pa, u64 *len, uchar **va)
{
	char buf[64];
	int ctl;

	if((ctl = open("#P/dma/clone", ORDWR)) < 0){
		fail("open clone: %r");
		return -1;
	}
	snprint(buf, sizeof buf, "alloc %llud %llud %s\n", size, align, type);
	if(cmd(ctl, buf) < 0 || cmd(ctl, "map\n") < 0 ||
	   chunkstat(ctl, n, pa, len, va) < 0){
		fail("chunk %llud %s: %r", size, type);
		close(ctl);
		return -1;
	}
	return ctl;
}

/* Whether a child given a clean file table (not holding the chunk's
 * ctl), or a copy of ours (holding it), can open the chunk's files. */
static int
childopens(int n, int flag)
{
	char path[64];
	Waitmsg *w;
	int pid, ok;

	switch(pid = rfork(RFPROC|flag)){
	case -1:
		fail("rfork: %r");
		return -1;
	case 0:
		snprint(path, sizeof path, "#P/dma/%d/ctl", n);
		ok = open(path, ORDWR) >= 0;
		snprint(path, sizeof path, "#P/dma/%d/data", n);
		ok += open(path, ORDWR) >= 0;
		exits(ok == 2 ? nil : ok == 0 ? "denied" : "partial");
	}
	while((w = wait()) != nil){
		if(w->pid == pid){
			ok = w->msg[0] == 0 ? 1 : strstr(w->msg, "denied") != nil ? 0 : -1;
			if(ok < 0)
				print("PNP-TEST child %d: '%s'\n", flag, w->msg);
			free(w);
			return ok;
		}
		free(w);
	}
	return -1;
}

static char*
readstr(char *path, char *buf, int n)
{
	int fd, m;

	buf[0] = 0;
	if((fd = open(path, OREAD)) < 0)
		return nil;
	m = read(fd, buf, n - 1);
	close(fd);
	if(m < 0)
		return nil;
	buf[m] = 0;
	if(m > 0 && buf[m-1] == '\n')
		buf[m-1] = 0;
	return buf;
}

static void
listpnp(void)
{
	Dir *d;
	char path[128], cls[32], own[64], reg[32];
	int fd, i, j, n;

	if((fd = open("#P", OREAD)) < 0)
		return;
	n = dirreadall(fd, &d);
	close(fd);
	/* One name, one device: the kernel drops a second node with the
	 * same name, so a listing must never repeat one. */
	for(i = 0; i < n; i++)
		for(j = i + 1; j < n; j++)
			if(strcmp(d[i].name, d[j].name) == 0)
				fail("#P: duplicate node name %s", d[i].name);
	for(i = 0; i < n; i++){
		snprint(path, sizeof path, "#P/%s/class", d[i].name);
		if(readstr(path, cls, sizeof cls) == nil)
			continue;
		snprint(path, sizeof path, "#P/%s/owner", d[i].name);
		readstr(path, own, sizeof own);
		snprint(path, sizeof path, "#P/%s/reg", d[i].name);
		readstr(path, reg, sizeof reg);
		print("PNP-TEST node %s class %s reg %s owner '%s'\n", d[i].name, cls, reg, own);
	}
	free(d);
}

/* A claim file holding node, or -1. */
static int
claim(char *node, char *who)
{
	char path[128];
	int fd;

	snprint(path, sizeof path, "#P/%s/claim", node);
	if((fd = open(path, OWRITE)) < 0)
		return -1;
	if(write(fd, who, strlen(who)) != strlen(who)){
		close(fd);
		return -1;
	}
	return fd;
}

static int
canopen(char *node, char *file, int mode)
{
	char path[128];
	int fd;

	snprint(path, sizeof path, "#P/%s/%s", node, file);
	if((fd = open(path, mode)) < 0)
		return 0;
	close(fd);
	return 1;
}

/* Whether a child given a clean file table, or a copy of ours, may open
 * node's regs. */
static int
childregs(char *node, int flag)
{
	Waitmsg *w;
	int pid, ok;

	switch(pid = rfork(RFPROC|flag)){
	case -1:
		return -1;
	case 0:
		exits(canopen(node, "regs", OREAD) ? nil : "denied");
	}
	ok = -1;
	while((w = wait()) != nil){
		if(w->pid == pid)
			ok = w->msg[0] == 0 ? 1 : strstr(w->msg, "denied") != nil ? 0 : -1;
		free(w);
		if(ok >= 0)
			break;
	}
	return ok;
}

static u32
reg32(int fd, vlong off, int *err)
{
	u32 v;

	if(pread(fd, &v, 4, off) != 4){
		*err = 1;
		return 0;
	}
	return v;
}

static void
claimtests(void)
{
	Dir *d;
	char rtc[64], path[128], buf[64], uart[64];
	int fd, c1, c2, c3, regs, i, n, err;
	u64 base;
	uchar *va;

	rtc[0] = uart[0] = 0;
	if((fd = open("#P", OREAD)) >= 0){
		n = dirreadall(fd, &d);
		close(fd);
		for(i = 0; i < n; i++){
			snprint(path, sizeof path, "#P/%s/class", d[i].name);
			if(readstr(path, buf, sizeof buf) == nil)
				continue;
			if(strcmp(buf, "rtc") == 0)
				snprint(rtc, sizeof rtc, "%s", d[i].name);
			if(strcmp(buf, "uart") == 0)
				snprint(uart, sizeof uart, "%s", d[i].name);
		}
		free(d);
	}
	if(rtc[0] == 0){
		fail("no rtc in #P");
		return;
	}
	snprint(path, sizeof path, "#P/%s/reg", rtc);
	base = strtoull(readstr(path, buf, sizeof buf), nil, 0);

	/* nothing without the claim, and no release file */
	if(canopen(rtc, "regs", OREAD))
		fail("regs opened without the claim");
	if(canopen(rtc, "wait", OREAD))
		fail("wait opened without the claim");
	if(segattach(0, rtc, 0, 0x200000) != (void*)-1)
		fail("window mapped without the claim");
	if(canopen(rtc, "release", OWRITE))
		fail("release file still there");

	/* claimed: the owner shows, regs reads the PL031's IDs */
	if((c1 = claim(rtc, "pnptest")) < 0){
		fail("claim %s: %r", rtc);
		return;
	}
	snprint(path, sizeof path, "#P/%s/owner", rtc);
	if(strcmp(readstr(path, buf, sizeof buf), "pnptest") != 0)
		fail("owner '%s', want pnptest", buf);
	snprint(path, sizeof path, "#P/%s/regs", rtc);
	if((regs = open(path, ORDWR)) < 0)
		fail("regs while claimed: %r");
	else{
		err = 0;
		if(reg32(regs, 0xff0, &err) != 0x0d || reg32(regs, 0xfe0, &err) != 0x31 || err)
			fail("PL031 ids: %#ux %#ux", reg32(regs, 0xff0, &err), reg32(regs, 0xfe0, &err));
		if(pread(regs, buf, 4, 2) >= 0)
			fail("misaligned register read succeeded");
		if(pread(regs, buf, 2, 0) >= 0)
			fail("2-byte register read succeeded");
		if(pread(regs, buf, 4, 0x1000) >= 0)
			fail("register read past the window succeeded");
		close(regs);
	}
	va = segattach(0, rtc, 0, 0x200000);
	if(va == (void*)-1)
		fail("window not mapped while claimed: %r");
	else{
		if(*(u32*)(va + (base & 0x1fffff) + 0xff0) != 0x0d)
			fail("mapped PL031 id wrong");
		segdetach(va);
	}
	if(canopen(rtc, "wait", OREAD) == 0)
		fail("wait refused while claimed: %r");

	/* exclusive: no second claim, shared or not */
	if((c2 = claim(rtc, "other")) >= 0){
		fail("second exclusive claim succeeded");
		close(c2);
	}
	if((c2 = claim(rtc, "other shared")) >= 0){
		fail("shared claim joined an exclusive one");
		close(c2);
	}
	if(childregs(rtc, RFCFDG) != 0)
		fail("a process without the claim opened regs");
	if(childregs(rtc, RFFDG) != 1)
		fail("a process holding a copy of the claim could not open regs");

	/* closing releases */
	close(c1);
	snprint(path, sizeof path, "#P/%s/owner", rtc);
	if(readstr(path, buf, sizeof buf)[0] != 0)
		fail("owner '%s' after release", buf);
	if(canopen(rtc, "regs", OREAD))
		fail("regs opened after release");

	/* shared claims coexist and keep an exclusive one out */
	c1 = claim(rtc, "a shared");
	c2 = claim(rtc, "b shared");
	if(c1 < 0 || c2 < 0)
		fail("shared claims: %r");
	if((c3 = claim(rtc, "c")) >= 0){
		fail("exclusive claim joined shared ones");
		close(c3);
	}
	close(c1);
	if(readstr(path, buf, sizeof buf)[0] == 0)
		fail("released with a shared holder left");
	close(c2);
	if(readstr(path, buf, sizeof buf)[0] != 0)
		fail("still claimed after the last shared holder");

	/* the console uart is uartfs's */
	if(uart[0] != 0){
		snprint(path, sizeof path, "#P/%s/owner", uart);
		if(readstr(path, buf, sizeof buf)[0] == 0)
			fail("uart not claimed by its driver");
		if((c1 = claim(uart, "pnptest")) >= 0){
			fail("claimed the driver's uart");
			close(c1);
		}
		if(canopen(uart, "regs", OREAD))
			fail("opened the driver's uart regs");
	}
	print("PNP-TEST claims done\n");
}

void
main(void)
{
	u64 a0, a1, pa, len, pa2, len2;
	uchar *va, *va2, buf[64];
	int ctl, ctl2, data, n, n2, i;

	listpnp();
	claimtests();
	a0 = avail();
	print("PNP-TEST avail %llud\n", a0);
	if(a0 == 0)
		fail("no arena");

	/* the arena itself has no segment, and the old files are gone */
	if(segattach(0, "dma", 0, 4096) != (void*)-1)
		fail("segattach dma succeeded");
	if(open("#P/dma/alloc", ORDWR) >= 0 || open("#P/dma/free", OWRITE) >= 0)
		fail("old #P/dma files still there");

	/* a chunk: map before alloc and a second alloc are refused */
	if((ctl = open("#P/dma/clone", ORDWR)) < 0)
		fail("clone: %r");
	if(cmd(ctl, "map\n") == 0)
		fail("map before alloc succeeded");
	if(cmd(ctl, "alloc 65536 4096 cached\n") < 0)
		fail("alloc: %r");
	if(cmd(ctl, "alloc 65536 4096 cached\n") == 0)
		fail("second alloc succeeded");
	if(cmd(ctl, "map\n") < 0 || chunkstat(ctl, &n, &pa, &len, &va) < 0)
		fail("map: %r");
	print("PNP-TEST chunk %d pa %#llux len %#llux va %p\n", n, pa, len, va);
	if(len != 65536)
		fail("len %llud", len);
	a1 = avail();
	if(a1 != a0 - 65536)
		fail("avail %llud after alloc, want %llud", a1, a0 - 65536);

	/* zeroed, and the mapping and the data file agree */
	for(i = 0; i < 65536; i++)
		if(va[i] != 0){
			fail("not zeroed at %d", i);
			break;
		}
	memset(va, 0xa5, 65536);
	if(cmd(ctl, "clean 0 65536\n") < 0)
		fail("clean: %r");
	if(cmd(ctl, "clean 65536 1\n") == 0)
		fail("clean past the chunk succeeded");
	snprint((char*)buf, sizeof buf, "#P/dma/%d/data", n);
	if((data = open((char*)buf, ORDWR)) < 0)
		fail("open data: %r");
	memset(buf, 0, sizeof buf);
	if(pread(data, buf, 64, 65536 - 64) != 64 || buf[0] != 0xa5 || buf[63] != 0xa5)
		fail("data read does not show the mapping's write");
	memset(buf, 0x3c, sizeof buf);
	if(pwrite(data, buf, 64, 128) != 64 || va[128] != 0x3c || va[191] != 0x3c)
		fail("mapping does not show the data write");
	if(pwrite(data, buf, 1, 65536) >= 0)
		fail("write past the chunk succeeded");
	close(data);

	/* only a holder of the ctl reaches the chunk */
	if(childopens(n, RFCFDG) != 0)
		fail("a process without the ctl opened the chunk");
	if(childopens(n, RFFDG) != 1)
		fail("a process holding the ctl could not open the chunk");

	/* closing the ctl keeps a mapped chunk; detaching frees it */
	close(ctl);
	if(avail() != a1)
		fail("chunk freed while still mapped");
	va[0] = 1;
	if(segdetach(va) < 0)
		fail("segdetach: %r");
	if(avail() != a0)
		fail("avail %llud after free, want %llud", avail(), a0);

	/* a new chunk over the same memory starts zeroed */
	ctl2 = chunk(65536, 4096, "cached", &n2, &pa2, &len2, &va2);
	if(ctl2 >= 0){
		if(pa2 != pa)
			print("PNP-TEST note: reallocated at %#llux, not %#llux\n", pa2, pa);
		for(i = 0; i < 65536; i++)
			if(va2[i] != 0){
				fail("reallocated chunk not zeroed at %d", i);
				break;
			}
		close(ctl2);
		segdetach(va2);
	}

	/* uncached: the mapping and the data file agree without maintenance */
	ctl2 = chunk(4096, 4096, "uncached", &n2, &pa2, &len2, &va2);
	if(ctl2 >= 0){
		snprint((char*)buf, sizeof buf, "#P/dma/%d/data", n2);
		data = open((char*)buf, ORDWR);
		va2[100] = 0x77;
		if(data < 0 || pread(data, buf, 1, 100) != 1 || buf[0] != 0x77)
			fail("uncached: data read does not show the mapping's write");
		buf[0] = 0x55;
		if(data < 0 || pwrite(data, buf, 1, 200) != 1 || va2[200] != 0x55)
			fail("uncached: mapping does not show the data write");
		close(data);
		close(ctl2);
		segdetach(va2);
	}

	/* a 2 MiB-aligned chunk (mapped with 2 MiB pages) is whole */
	ctl2 = chunk(4*1024*1024, 2*1024*1024, "cached", &n2, &pa2, &len2, &va2);
	if(ctl2 >= 0){
		if(pa2 & (2*1024*1024 - 1))
			fail("big chunk pa %#llux not 2 MiB aligned", pa2);
		va2[0] = 1;
		va2[len2 - 1] = 2;
		if(va2[0] != 1 || va2[len2 - 1] != 2)
			fail("big chunk not whole");
		close(ctl2);
		segdetach(va2);
	}
	if(avail() != a0)
		fail("avail %llud at the end, want %llud", avail(), a0);

	if(nfail == 0)
		print("PNP-TEST PASS\n");
	else
		print("PNP-TEST FAILED %d\n", nfail);
	exits(nil);
}
'''


def make_archive(out: Path, libc: Path) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    prog, _ = build_userland.build_program(out, "pnptest", PNPTEST,
                                          0x200000, libc)
    files = [("pnptest", prog)]
    offset = 24 + 48 * len(files)
    total = offset + sum(len(data) for _, data in files)
    table, body = bytearray(), bytearray()
    for name, data in files:
        table += struct.pack("<32sIIII", name.encode(), offset,
                             len(data), 0o555, 0)
        body += data
        offset += len(data)
    archive = out / "pnp-test.img"
    archive.write_bytes(struct.pack("<8sIIII", b"O9BOOT01", 1, len(files),
                                    total, 0) + table + body)
    return archive


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kernel", type=Path, required=True)
    ap.add_argument("--libc", type=Path,
                    default=build_userland.ROOT / "build/arm64-libc/libc.a")
    ap.add_argument("--out", type=Path,
                    default=build_userland.ROOT / "build/arm64-pnp-test")
    args = ap.parse_args()
    archive = make_archive(args.out, args.libc)
    command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a55",
               "-m", "768M", "-nographic", "-monitor", "none", "-kernel",
               str(args.kernel), "-device",
               f"loader,file={archive},addr=0x49000000"]
    # a boot file started from the shell once it is up (after init's
    # services, an LKL guest among them, have started)
    send = [(50.0, "/boot/pnptest\n")]
    transcript = boot_until(command, "PNP-TEST PASS", idle=90, total=180,
                            limit=4 * 1024 * 1024, send=send)
    log = args.out / "boot.log"
    log.write_text(transcript)
    if "PNP-TEST PASS" not in transcript:
        fails = [l for l in transcript.splitlines() if "PNP-TEST" in l]
        raise SystemExit("#P test failed:\n" + "\n".join(fails) +
                         f"\nsee {log}")
    print(f"#P claims and DMA arena PASS: {log}")


if __name__ == "__main__":
    main()
