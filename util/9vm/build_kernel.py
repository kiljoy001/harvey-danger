#!/usr/bin/env python3
import os
import sys
import subprocess

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
BUILD_DIR = os.path.join(REPO_ROOT, "build/kernel")
OUT_ELF = os.path.join(REPO_ROOT, "build/harvey.elf")
OUT_32BIT = os.path.join(REPO_ROOT, "build/harvey.32bit")

CC = "clang"
LD = "ld.lld"
OBJCOPY = "llvm-objcopy"

CFLAGS = [
    "-c",
    "-fpic",
    "-fno-stack-protector",
    "-O2",
    "-fcommon",
    "-mno-implicit-float",
    "-mno-red-zone",
    "-ffreestanding",
    "-fno-builtin",
    "-DKERNDATE=1433623937",
    "-g",
    "-D__HARVEY__",
    "-U_LINUX",
    "-U__linux__",
    "-Wall",
    "-Wno-parentheses",
    "-Wno-missing-braces",
    "-Wno-unknown-pragmas",
    "-Wno-unused-function",
    "-Wno-unused-variable",
    f"-I{os.path.join(REPO_ROOT, 'sys/src/9/amd64')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/src/9/port')}",
    f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/src/9/ip')}",
    f"-I{os.path.join(REPO_ROOT, 'sys/src/libc/9syscall')}",
]

LDFLAGS = [
    "-T", os.path.join(REPO_ROOT, "sys/src/9/amd64/kernel.ld"),
    "-nostdlib",
    "-static",
    "-z", "max-page-size=0x1000",
    "-g",
]

SOURCES = [
    # AMD64 Asm
    "sys/src/9/amd64/entry.S",
    "sys/src/9/amd64/l64v.S",
    "sys/src/9/amd64/l64fpu.S",
    "sys/src/9/amd64/cpuidamd64.S",
    "sys/src/9/amd64/l64acidt.S",
    "sys/src/9/amd64/l64idt.S",
    "sys/src/9/amd64/l64vsyscall.S",

    # AMD64 C
    "sys/src/9/amd64/acore.c",
    "sys/src/9/amd64/apic.c",
    "sys/src/9/amd64/arch.c",
    "sys/src/9/amd64/archamd64.c",
    "sys/src/9/amd64/backtrace.c",
    "sys/src/9/amd64/ctype.c",
    "sys/src/9/amd64/devarch.c",
    "sys/src/9/amd64/fpu.c",
    "sys/src/9/amd64/i8254.c",
    "sys/src/9/amd64/i8259.c",
    "sys/src/9/amd64/ioapic.c",
    "sys/src/9/amd64/main.c",
    "sys/src/9/amd64/map.c",
    "sys/src/9/amd64/memory.c",
    "sys/src/9/amd64/mmu.c",
    "sys/src/9/amd64/multiboot.c",
    "sys/src/9/amd64/pamap.c",
    "sys/src/9/amd64/physalloc.c",
    "sys/src/9/amd64/qmalloc.c",
    "sys/src/9/amd64/sipi.c",
    "sys/src/9/amd64/syscall.c",
    "sys/src/9/amd64/systab.c",
    "sys/src/9/amd64/tcore.c",
    "sys/src/9/amd64/trap.c",
    "sys/src/9/amd64/vsvm.c",
    "sys/src/9/amd64/cga.c",
    "sys/src/9/amd64/msi.c",
    "sys/src/9/amd64/mpacpi.c",
    "sys/src/9/amd64/devacpi.c",
    "sys/src/9/amd64/micro.c",
    "sys/src/9/amd64/ipproto.c",
    "sys/src/9/amd64/ether.c",
    "sys/src/9/386/pci.c",
    "sys/src/9/386/uarti8250.c",
    "sys/src/9/386/i8042.c",
    "sys/src/9/amd64/mouse.c",
    "sys/src/9/amd64/softscreen.c",

    # Port C
    "sys/src/9/port/alarm.c",
    "sys/src/9/port/allocb.c",
    "sys/src/9/port/cache.c",
    "sys/src/9/port/chan.c",
    "sys/src/9/port/cpu_buffer.c",
    "sys/src/9/port/dev.c",
    "sys/src/9/port/devcap.c",
    "sys/src/9/port/devcons.c",
    "sys/src/9/port/devdup.c",
    "sys/src/9/port/devenv.c",
    "sys/src/9/port/devkprof.c",
    "sys/src/9/port/devmnt.c",
    "sys/src/9/port/devpipe.c",
    "sys/src/9/port/devproc.c",
    "sys/src/9/port/devramfs.c",
    "sys/src/9/port/devroot.c",
    "sys/src/9/port/devsegment.c",
    "sys/src/9/port/devsrv.c",
    "sys/src/9/port/devether.c",
    "sys/src/9/port/virtio10.c",
    "sys/src/9/port/devtab.c",
    "sys/src/9/port/devuart.c",
    "sys/src/9/port/devdraw.c",
    "sys/src/9/port/devmouse.c",
    # Kernel memdraw/memlayer (klibmemdraw.json, klibmemlayer.json)
    "sys/src/libmemdraw/alloc.c",
    "sys/src/libmemdraw/arc.c",
    "sys/src/libmemdraw/cload.c",
    "sys/src/libmemdraw/cmap.c",
    "sys/src/libmemdraw/cread.c",
    "sys/src/libmemdraw/defont.c",
    "sys/src/libmemdraw/draw.c",
    "sys/src/libmemdraw/ellipse.c",
    "sys/src/libmemdraw/fillpoly.c",
    "sys/src/libmemdraw/line.c",
    "sys/src/libmemdraw/load.c",
    "sys/src/libmemdraw/openmemsubfont.c",
    "sys/src/libmemdraw/poly.c",
    "sys/src/libmemdraw/read.c",
    "sys/src/libmemdraw/string.c",
    "sys/src/libmemdraw/subfont.c",
    "sys/src/libmemdraw/unload.c",
    "sys/src/libmemdraw/write.c",
    "sys/src/libmemlayer/draw.c",
    "sys/src/libmemlayer/lalloc.c",
    "sys/src/libmemlayer/layerop.c",
    "sys/src/libmemlayer/ldelete.c",
    "sys/src/libmemlayer/lhide.c",
    "sys/src/libmemlayer/line.c",
    "sys/src/libmemlayer/load.c",
    "sys/src/libmemlayer/lorigin.c",
    "sys/src/libmemlayer/lsetrefresh.c",
    "sys/src/libmemlayer/ltofront.c",
    "sys/src/libmemlayer/ltorear.c",
    "sys/src/libmemlayer/unload.c",
    # Kernel libdraw helpers needed by memdraw (from klibdraw.json)
    "sys/src/libdraw/arith.c",
    "sys/src/libdraw/bytesperline.c",
    "sys/src/libdraw/computil.c",
    "sys/src/libdraw/drawrepl.c",
    "sys/src/libdraw/chan.c",
    "sys/src/libdraw/defont.c",
    "sys/src/libdraw/fmt.c",
    "sys/src/libdraw/icossin.c",
    "sys/src/libdraw/icossin2.c",
    "sys/src/libdraw/rectclip.c",
    "sys/src/libdraw/rgb.c",
    # Single External I/O Driver
    "sys/src/9/port/devvirtio9p.c",
    "sys/src/9/port/dev9ring.c",
    "sys/src/9/port/edf.c",
    "sys/src/9/port/ethervirtio.c",
    "sys/src/libsec/port/sha1.c",
    "sys/src/libsec/port/sha1block.c",
    "sys/src/libsec/port/hmac.c",
    "sys/src/9/port/fortuna.c",
    "sys/src/9/port/execaout.c",
    "sys/src/9/port/elf64.c",
    "sys/src/9/port/getput.c",
    "sys/src/9/port/mul64fract.c",
    "sys/src/9/port/fault.c",
    "sys/src/9/port/image.c",
    "sys/src/9/port/latin1.c",
    "sys/src/9/port/page.c",
    "sys/src/9/port/pager.c",
    "sys/src/9/port/parse.c",
    "sys/src/9/port/pgrp.c",
    "sys/src/9/port/portclock.c",
    "sys/src/9/port/print.c",
    "sys/src/9/port/proc.c",
    "sys/src/9/port/ps.c",
    "sys/src/9/port/qio.c",
    "sys/src/9/port/qlock.c",
    "sys/src/9/port/random.c",
    "sys/src/9/port/rebootcmd.c",
    "sys/src/9/port/segment.c",
    "sys/src/9/port/sysauth.c",
    "sys/src/9/port/syscallfmt.c",
    "sys/src/9/port/sysfile.c",
    "sys/src/9/port/sysproc.c",
    "sys/src/9/port/sysseg.c",
    "sys/src/9/port/syszio.c",
    "sys/src/9/port/taslock.c",
    "sys/src/9/port/tod.c",
    "sys/src/9/port/watermarks.c",
    "sys/src/9/port/netif.c",

    # IP stack, including the improved IL (protocol 40)
    "sys/src/9/ip/arp.c",
    "sys/src/9/ip/chandial.c",
    "sys/src/9/ip/devip.c",
    "sys/src/9/ip/ethermedium.c",
    "sys/src/9/ip/gre.c",
    "sys/src/9/ip/icmp6.c",
    "sys/src/9/ip/icmp.c",
    "sys/src/9/ip/il.c",
    "sys/src/9/ip/inferno.c",
    "sys/src/9/ip/ipaux.c",
    "sys/src/9/ip/ip.c",
    "sys/src/9/ip/ipifc.c",
    "sys/src/9/ip/ipmux.c",
    "sys/src/9/ip/iproute.c",
    "sys/src/9/ip/ipv6.c",
    "sys/src/9/ip/loopbackmedium.c",
    "sys/src/9/ip/netdevmedium.c",
    "sys/src/9/ip/netlog.c",
    "sys/src/9/ip/nullmedium.c",
    "sys/src/9/ip/pktmedium.c",
    "sys/src/9/ip/ptclbsum.c",
    "sys/src/9/ip/rudp.c",
    "sys/src/9/ip/tcp.c",
    "sys/src/9/ip/udp.c",

    # Kernel subset of libip (klibip.json)
    "sys/src/libip/bo.c",
    "sys/src/libip/classmask.c",
    "sys/src/libip/eipfmt.c",
    "sys/src/libip/equivip.c",
    "sys/src/libip/ipaux.c",
    "sys/src/libip/parseip.c",
]

def build_init():
    init9_src = os.path.join(REPO_ROOT, "sys/src/9/amd64/init9.c")
    initcode_src = os.path.join(REPO_ROOT, "sys/src/9/port/initcode.c")
    init_out = os.path.join(REPO_ROOT, "build/init")
    init_h = os.path.join(REPO_ROOT, "sys/src/9/amd64/init.h")
    libc_a = os.path.join(REPO_ROOT, "build/libc.a")
    elf2c_py = os.path.join(REPO_ROOT, "util/9vm/elf2c.py")

    init9_obj = os.path.join(REPO_ROOT, "build/init9.o")
    initcode_obj = os.path.join(REPO_ROOT, "build/initcode.o")

    init_cflags = [
        "-c", "-g", "-Wall", "-O0", "-static", "-mno-red-zone",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
        "-mcmodel=small",
        f"-I{os.path.join(REPO_ROOT, 'amd64/include')}",
        f"-I{os.path.join(REPO_ROOT, 'sys/include')}",
    ]

    subprocess.run([CC] + init_cflags + [init9_src, "-o", init9_obj], check=True)
    subprocess.run([CC] + init_cflags + [initcode_src, "-o", initcode_obj], check=True)

    init_ldflags = [
        "-e_main", "-static", "-Ttext=0x200020", "-Tdata=0x400000",
        "-z", "max-page-size=0x1000", "-z", "common-page-size=0x1000",
        init9_obj, initcode_obj, libc_a, "-o", init_out
    ]
    subprocess.run([LD] + init_ldflags, check=True)
    subprocess.run([sys.executable, elf2c_py, init_h, init_out], check=True)

def main():
    os.makedirs(BUILD_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(OUT_ELF), exist_ok=True)

    from prism import compile_o9
    gefs_init = os.path.join(REPO_ROOT, "boot/gefs_init.o9")
    gefs_init_bin = os.path.join(REPO_ROOT, "boot/gefsinit")
    gefs_server = os.path.join(REPO_ROOT, "build/gefs-guest")
    o9irc_server = os.path.join(REPO_ROOT, "build/o9irc-ircfs")
    if not os.path.exists(gefs_init_bin) or os.path.getmtime(gefs_init_bin) < os.path.getmtime(gefs_init):
        compile_o9(gefs_init, gefs_init_bin)
    if not os.path.exists(gefs_server):
        subprocess.run([sys.executable, os.path.join(REPO_ROOT, "util/9vm/build_gefs_guest.py")], check=True)
    embedded = os.path.join(BUILD_DIR, "embedded_gefs.S")
    with open(embedded, "w") as f:
        f.write('.section .rodata\n.balign 4096\n')
        for label, path in (("init", gefs_init_bin), ("server", gefs_server),
                            ("o9irc", o9irc_server)):
            f.write(f'.global aspera_gefs_{label}_start\n.global aspera_gefs_{label}_end\n')
            f.write(f'aspera_gefs_{label}_start:\n')
            if os.path.exists(path):
                f.write(f'.incbin "{path}"\n')
            f.write(f'aspera_gefs_{label}_end:\n')
    embedded_obj = os.path.join(BUILD_DIR, "embedded_gefs.o")
    subprocess.run([CC, "-c", embedded, "-o", embedded_obj], check=True)

    build_init()

    objs = [embedded_obj]
    print(f"Compiling {len(SOURCES)} microkernel sources with {CC}...")

    init_h = os.path.join(REPO_ROOT, "sys/src/9/amd64/init.h")
    init_h_mtime = os.path.getmtime(init_h) if os.path.exists(init_h) else 0

    for src_rel in SOURCES:
        src = os.path.join(REPO_ROOT, src_rel)
        if not os.path.exists(src):
            print(f"Error: {src} not found!")
            sys.exit(1)

        base = os.path.splitext(os.path.basename(src))[0]
        parent = os.path.basename(os.path.dirname(src))
        obj_name = f"{parent}_{base}.o"
        obj_path = os.path.join(BUILD_DIR, obj_name)
        objs.append(obj_path)

        script_mtime = os.path.getmtime(__file__)
        if src_rel == "sys/src/9/amd64/main.c" and os.path.exists(obj_path) and os.path.getmtime(obj_path) < init_h_mtime:
            pass
        elif os.path.exists(obj_path) and os.path.getmtime(obj_path) >= os.path.getmtime(src) and os.path.getmtime(obj_path) >= script_mtime:
            continue

        cmd = [CC] + CFLAGS + [src, "-o", obj_path]
        res = subprocess.run(cmd)
        if res.returncode != 0:
            print(f"Compilation failed for {src}")
            sys.exit(1)

    libc_a = os.path.join(REPO_ROOT, "build/libc.a")
    if not os.path.exists(libc_a):
        print(f"Error: libc.a not found at {libc_a}")
        sys.exit(1)

    print(f"Linking microkernel with {LD} -> {OUT_ELF}...")
    link_cmd = [LD] + LDFLAGS + objs + [libc_a, "-o", OUT_ELF]
    res = subprocess.run(link_cmd)
    if res.returncode != 0:
        print("Link failed!")
        sys.exit(1)

    print(f"Converting {OUT_ELF} to 32-bit Multiboot ELF {OUT_32BIT}...")
    objcopy_cmd = [OBJCOPY, "-I", "elf64-x86-64", "-O", "elf32-i386", OUT_ELF, OUT_32BIT]
    subprocess.run(objcopy_cmd, check=True)

    size = os.path.getsize(OUT_32BIT)
    print(f"SUCCESS! Microkernel created at {OUT_32BIT} ({size / 1024:.1f} KB)")

if __name__ == "__main__":
    main()
