#!/usr/bin/env python3
"""Check #P's DMA arena under an ARM64 Harvey kernel in QEMU.

A chunk is allocated and mapped through its ctl and read and written
through its data file; another process cannot reach it without its ctl;
it is zeroed when allocated and goes back to the arena only once its
files are closed and its mapping gone; the arena as a whole cannot be
attached.  Supply the kernel image with --kernel.
"""
import argparse
from pathlib import Path
import struct

import build_userland
from test_virt import boot_until


PDMATEST = r'''#include <u.h>
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
	print("PDMA-TEST FAIL %s\n", buf);
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
				print("PDMA-TEST child %d: '%s'\n", flag, w->msg);
			free(w);
			return ok;
		}
		free(w);
	}
	return -1;
}

void
main(void)
{
	u64 a0, a1, pa, len, pa2, len2;
	uchar *va, *va2, buf[64];
	int ctl, ctl2, data, n, n2, i;

	a0 = avail();
	print("PDMA-TEST avail %llud\n", a0);
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
	print("PDMA-TEST chunk %d pa %#llux len %#llux va %p\n", n, pa, len, va);
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
			print("PDMA-TEST note: reallocated at %#llux, not %#llux\n", pa2, pa);
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
		print("PDMA-TEST PASS\n");
	else
		print("PDMA-TEST FAILED %d\n", nfail);
	exits(nil);
}
'''


def make_archive(out: Path, libc: Path) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    prog, _ = build_userland.build_program(out, "pdmatest", PDMATEST,
                                          0x200000, libc)
    files = [("pdmatest", prog)]
    offset = 24 + 48 * len(files)
    total = offset + sum(len(data) for _, data in files)
    table, body = bytearray(), bytearray()
    for name, data in files:
        table += struct.pack("<32sIIII", name.encode(), offset,
                             len(data), 0o555, 0)
        body += data
        offset += len(data)
    archive = out / "pdma-test.img"
    archive.write_bytes(struct.pack("<8sIIII", b"O9BOOT01", 1, len(files),
                                    total, 0) + table + body)
    return archive


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kernel", type=Path, required=True)
    ap.add_argument("--libc", type=Path,
                    default=build_userland.ROOT / "build/arm64-libc/libc.a")
    ap.add_argument("--out", type=Path,
                    default=build_userland.ROOT / "build/arm64-pdma-test")
    args = ap.parse_args()
    archive = make_archive(args.out, args.libc)
    command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a55",
               "-m", "768M", "-nographic", "-monitor", "none", "-kernel",
               str(args.kernel), "-device",
               f"loader,file={archive},addr=0x49000000"]
    # a boot file started from the shell once it is up (after init's
    # services, an LKL guest among them, have started)
    send = [(50.0, "/boot/pdmatest\n")]
    transcript = boot_until(command, "PDMA-TEST PASS", idle=90, total=180,
                            limit=4 * 1024 * 1024, send=send)
    log = args.out / "boot.log"
    log.write_text(transcript)
    if "PDMA-TEST PASS" not in transcript:
        fails = [l for l in transcript.splitlines() if "PDMA-TEST" in l]
        raise SystemExit("PDMA test failed:\n" + "\n".join(fails) +
                         f"\nsee {log}")
    print(f"#P DMA arena PASS: {log}")


if __name__ == "__main__":
    main()
