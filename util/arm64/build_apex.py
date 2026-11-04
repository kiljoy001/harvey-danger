#!/usr/bin/env python3
"""Cross-build the APEX runtime layer for ARM64 and link a test program.

This is the ARM64 answer to the x86 APEX build in apex/src: the same
portable C sources, with an aarch64 crt0, an aarch64 stack switch, and the
variant-I TLS block in __libc_start_main().  It links against the ARM64
Plan 9 libc built by build_libc.py.

    python3 util/arm64/build_apex.py [--keep]

The output is build/arm64-apex/hello.elf (entry _start).  Add --keep to
keep the objects for inspection.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/arm64-apex"
APEX = ROOT / "apex"
LIBC = ROOT / "build/arm64-libc/libc.a"
CC = os.environ.get("ARM64_CC") or shutil.which("clang-23") or "clang"
LD = os.environ.get("ARM64_LD", "ld.lld")

HELLO = r"""
/* Plan 9 libc entry points, declared directly to avoid mixing the APEX and
 * Plan 9 header trees in one translation unit. */
extern long write(int, const void *, unsigned long);
extern int open(char *, int);
extern void exits(char *);

static void
put(char *s)
{
	unsigned long n;

	for(n = 0; s[n] != 0; n++)
		;
	write(1, s, n);
}

static void
putnum(long v)
{
	char b[24];
	int i;

	i = 0;
	if(v < 0){
		b[i++] = '-';
		v = -v;
	}
	{
		char t[24];
		int j = 0;
		do {
			t[j++] = '0' + v % 10;
			v /= 10;
		} while(v != 0);
		while(j > 0)
			b[i++] = t[--j];
	}
	b[i] = 0;
	put(b);
}

__thread long tls_counter = 11;

void
main(int argc, char **argv)
{
	/* The kernel starts the first process with no open descriptors. */
	open("#c/cons", 0);
	open("#c/cons", 1);
	open("#c/cons", 1);
	put("apex-hello argc=");
	putnum(argc);
	put(" tls=");
	putnum(tls_counter);
	put("\n");
	if(tls_counter == 11)
		put("TLS-OK\n");
	exits(0);
}
"""


# nan.c pokes at the x86 FPdbleword union, and malloc.c would collide with
# the Plan 9 libc allocator (which also defines mallocz, so libc.a always
# pulls it in); the APEX allocator's heap lock comes with the thread port.
# Everything else in the plan9 directory goes into the archive, so the
# linker pulls exactly the members something references.
SKIP = {"nan.c", "malloc.c"}


def sources() -> list[Path]:
    plan9 = APEX / "src/ap/plan9"
    return [APEX / "src/ap/crt/crt0.c",
            *sorted(p for p in plan9.glob("*.c") if p.name not in SKIP),
            APEX / "src/ap/aarch64/startup.c",
            APEX / "src/ap/aarch64/notetramp.c"]


def flags() -> list[str]:
    return [
        "--target=aarch64-none-elf", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-mstrict-align", "-fcommon", "-nostdlibinc",
        "-O2", "-g", "-D_SUSV2_SOURCE", "-D_POSIX_SOURCE",
        "-D_LIMITS_EXTENSION", "-DHARVEY", "-D_PLAN9_SOURCE",
        "-D_BSD_EXTENSION", "-DIEEE_8087",
        "-I", str(APEX / "include"),
        "-I", str(APEX / "aarch64/include"),
        "-I", str(APEX / "src/ap/internal"),
    ]


LINK_SCRIPT = """\
ENTRY(_start)
PHDRS {
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
  tls PT_TLS FLAGS(4);
}
SECTIONS {
  /* Map the ELF header itself: __libc_start_main reads the program headers
   * through __ehdr_start to find PT_TLS. */
  . = 0x200000 + SIZEOF_HEADERS;
  .text : { *(.text .text.*) *(.rodata .rodata.*) } :text
  . = ALIGN(0x200000);
  .data : { *(.data .data.*) *(.data.rel.ro*) } :data
  .tdata : { *(.tdata .tdata.*) } :data :tls
  .tbss : { *(.tbss .tbss.*) } :data :tls
  .bss : { *(.bss .bss.* COMMON) } :data
  PROVIDE(end = .);
  /* __libc_start_main cannot read the ELF header on ARM64 (the first
   * PT_LOAD starts at its file offset), so the TLS segment is recorded
   * here instead of parsed at run time. */
  __tls_vaddr = ADDR(.tdata);
  __tls_filesz = SIZEOF(.tdata);
  __tls_memsz = SIZEOF(.tdata) + SIZEOF(.tbss);
  __tls_align = 16;
  /DISCARD/ : { *(.comment) *(.note*) }
}
"""


def run(cmd: list[str]) -> None:
    subprocess.run(cmd, check=True)


def main() -> None:
    keep = "--keep" in sys.argv[1:]
    if not LIBC.is_file():
        sys.exit(f"missing {LIBC}; run util/arm64/build_libc.py first")

    shutil.rmtree(OUT, ignore_errors=True)
    (OUT / "obj").mkdir(parents=True)
    (OUT / "hello.c").write_text(HELLO)
    (OUT / "hello.ld").write_text(LINK_SCRIPT)

    objects = []
    for index, source in enumerate(sources()):
        obj = OUT / "obj" / f"{index:02d}-{source.stem}.o"
        run([CC, *flags(), "-c", str(source), "-o", str(obj)])
        objects.append(obj)

    # Archive the layer, as the x86 build does: an archive only pulls the
    # members something actually references, so the x86 assembly pieces
    # that have no aarch64 port yet stay out of the link.
    libap = OUT / "libap.a"
    run(["llvm-ar", "rcs", str(libap), *[str(o) for o in objects]])
    hello = OUT / "obj/hello.o"
    run([CC, *flags(), "-c", str(OUT / "hello.c"), "-o", str(hello)])

    elf = OUT / "hello.elf"
    # The kernel's ELF loader maps each PT_LOAD at p_align and rejects
    # overlaps, so keep segment alignment at the 4 KiB page size.
    run([LD, "-z", "max-page-size=4096", "-T", str(OUT / "hello.ld"),
         "-e", "_start", "-u", "_start",
         str(hello), str(libap), str(LIBC), "-o", str(elf)])
    print(f"ARM64 APEX link PASS: {elf} ({elf.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
