# Harvey ARM64 bring-up

The ARM64 Image boots directly on QEMU `virt`. It enters Harvey's ARM64
assembly and C, clears BSS, sets a stack, parses the device tree to discover
RAM, the chosen PL011 or DesignWare UART, GICv2 or GICv3, and the timer PPI. A monotonic early allocator excludes the
kernel image, DTB, firmware memory reservations and `/reserved-memory` `reg`
ranges. An identity map enables the MMU and caches with normal attributes for
RAM and device attributes for MMIO. The guest then receives three virtual timer
interrupts through an EL1 vector and reports synchronous exceptions over
serial. It enters EL0, resumes after a test syscall, and switches between two
page-table roots whose user mappings point to different physical pages. The
boot check confirms that EL0 cannot read kernel memory, write its code page,
or execute its stack. A timer interrupt can arrive from EL0 and return to the
interrupted instruction. The full kernel starts the shared Plan 9 scheduler,
creates a `Proc` with its own page-table root, and runs a small libc-backed C
program at EL0. Its AArch64 ELF is validated and loaded from its
`PT_LOAD` records into separate RX text, RW data, and NX stack mappings, with a
device-tree-selected UART page. The program enters through the ELF entry and ARM64 libc `_main`,
calls the generated `nsec` wrapper through the normal syscall table, and writes
its result marker through the UART mapping. A normally segmented bootstrap
then executes the filesystem-backed `/boot/init`. The ARM64 `Label` save/restore routines match the calling
convention used by Harvey's shared scheduler in `sys/src/9/port/proc.c`. QEMU
tests switch between two independent kernel stacks and check callee-saved
register preservation while the timer is active. Interrupt masking is also
checked across two timer periods.

`build_kernel.py` now links the shared core port sources, the local `#~` ring
transport, the `#9` virtio-MMIO 9P transport, ARM64 architecture sources, and
ARM64 builds of kernel libc, libip, and libsec. Hardware drivers and the kernel
IP stack are excluded. The output is an AArch64 ELF and direct-boot
Image. After the boot diagnostics, the full Image initializes process and image
allocation, resets the device tables, primes the page allocator, and starts
Harvey's shared scheduler with two kernel processes. It drives the shared
clock from the ARM generic timer, walks into the root device's `dev`
directory, and exchanges a tagged 9P request and response over a named `#~`
connection. With a virtio-9P device present, it also negotiates 9P over `#9/0`.
There is no `#t/eia0` UART device in the kernel. Early boot and
fault diagnostics write to the UART selected by `/chosen/stdout-path` before user space is
available; this temporary bootstrap console is not a serial service.
It does **not** yet offer an interactive shell.

### APEX on ARM64

`util/arm64/build_apex.py` cross-builds the APEX startup layer for ARM64:
crt0, `__libc_start_main`, the mmap arena and the raw syscall wrappers,
linked against the ARM64 Plan 9 libc.  `util/arm64/test_apex_boot.py`
substitutes the resulting binary for the init image in the generated
userland header and boots it in QEMU virt, so the first EL0 program the
kernel execs is the APEX one:

```text
apex-hello argc=1 tls=11
TLS-OK
```

That exercises the entry convention (argc/argv/Tos in registers), the heap
stack switch, the mapping arena, and variant-I TLS end to end: the startup
builds the block, publishes it through `#p/<pid>/tls`, the kernel loads
`TPIDR_EL0`, and the program reads its initialised `__thread` variable.

The whole layer builds: stdio (including quad-precision `%Lf`, with the
soft-float routines compiled from compiler-rt's builtins), signal, locale,
math, regex and APEX's locked allocator, which replaces the Plan 9 libc
one.  `_envsetup` runs, so the environment is populated from `#e`.

### libc++ on ARM64

`build_libunwind.py` and `build_libcxx.py` build the C++ runtime for ARM64
- libunwind, libc++ and the libc++abi pieces - against the APEX layer, and
`build_libcxx.py --test` links a program that uses std::string,
std::vector, std::thread and a thread-local:

```text
cxx-libcxx harvey arm64 sum=6 tls=7 shared=42
LIBCXX-ARM64-OK
```

Two platform adaptations: `pthread.cpp`'s `thread_spawn` has an aarch64
body (rfork, switch to the new stack, branch), and `dl_iterate_phdr`
synthesises the phdrs libunwind needs from linker symbols, because the
kernel does not map the ELF header on ARM64.

The unwinder needed one build fix: `aarch64-none-elf` does not get
asynchronous unwind tables by default, so libunwind's own frames had no
FDEs and its first step reported end of stack.  libunwind and the APEX
layer are now built with `-funwind-tables`, and `throw`/`catch` works:

```text
cxx-libcxx harvey arm64 sum=6 tls=7 shared=42 int=1 what=boom
LIBCXX-ARM64-OK
```

### Running ARM tools with arguments

The kernel starts its first user process with no descriptors and no
arguments.  `util/arm64/mkrun.py` wraps an ARM64 program in a launcher that
opens the console, unions a ramfs over the read-only boot root, writes the
embedded program (and optionally a tree of data files) out, and execs it
with a command line:

```sh
python3 util/arm64/mkrun.py /tmp/opencode/llvm-arm64/bin/llvm-tblgen \
    --tree /home/scott/Repo/llvm-project/llvm/include:/td \
    -- -gen-vt -I /td /td/llvm/Target/ValueTypes.td
python3 util/arm64/test_apex_boot.py --elf build/arm64-run/runner.elf \
    --marker '#include'
```

LLVM builds for aarch64 with `harvey-aarch64-toolchain.cmake`, and
`util/arm64/test-tblgen.py` runs the result against the real .td tree:

```text
tblgen: 46644 bytes generated on Harvey/ARM64, identical to the host tool
```

That is `llvm-tblgen -gen-vt` producing `GenVT.inc` byte-for-byte the same
as the host build of the same revision.

```sh
python3 util/arm64/test_virt.py
python3 util/arm64/test_fdt.py
python3 util/arm64/check_port.py
python3 util/arm64/test_syscalls.py
python3 util/arm64/build_libc.py
python3 util/arm64/build_o9_services.py
python3 util/arm64/test_user_runtime.py
python3 util/arm64/test_full_kernel.py
python3 util/arm64/test_rc.py
python3 util/arm64/test_virtio9p.py
```

For interactive inspection:

```sh
python3 util/arm64/build_virt.py
qemu-system-aarch64 -machine virt -cpu cortex-a57 -m 512M -smp 1 \
    -nographic -monitor none -kernel build/arm64/harvey-virt.img

python3 util/arm64/build_kernel.py
qemu-system-aarch64 -machine virt -cpu cortex-a57 -m 512M -smp 1 \
    -nographic -monitor none -kernel build/arm64-kernel/harvey.img
```

The **same** `build/arm64-kernel/harvey.img` is used with QEMU `virt`, the
Orange Pi 3B (RK3566), and the Orange Pi 3 (Allwinner H6). No board flag or
separate kernel build is needed. The firmware
passes a device tree in `x0`; the kernel chooses its console, timer controller,
MMIO ranges, and a RAM bank containing the loaded Image from that tree. The
current Image is linked at `0x40080000`, so firmware must place it there. A
board whose RAM does not cover that address still needs a relocatable Image
before it can use this kernel. The Image header allows firmware to retain the
load address, but the kernel itself has no relocation support yet.

For another board, the boot contract is: load this Image at `0x40080000`, pass
an FDT in `x0` with `/memory`, `/chosen/stdout-path`, a supported UART, an ARM
GICv2 or GICv3, and an ARMv8 timer. The parser accepts an alias in
`stdout-path` and chooses the RAM bank containing the Image. Hardware not
needed for early boot belongs in a user-space driver filesystem; adding that
driver should not alter the kernel image. A future relocation stage must remove
the fixed load address before this is a general ARM64 distribution image.

For the Orange Pi 3B RK3566 bring-up:

```sh
python3 util/arm64/build_kernel.py
```

Boot it with the full **OS** board DTB as `booti 0x40080000 - 0x48000000`
after loading the Image at `0x40080000` and the DTB at `0x48000000`. The
installed vendor U-Boot's control DTB at `0xeb9f5e30` is stripped of its GIC
and timer nodes and cannot be used for this boot. U-Boot adds valid `/memory`
nodes to the OS DTB during `booti`. The early parser
uses `/chosen/stdout-path` (including `/aliases`) to select UART2 on this board,
and chooses the RAM bank containing the Image. The NPU register/DMA probe is excluded from normal
boot; set `ARM64_RK3566_NPU_PROBE=1` only for a separate NPU experiment. The
QEMU image now provides an interactive `rc`. The same image was reflashed over
router-hosted TFTP and verified on the v1.1 board: `echo hardware-rc` executed
`/boot/echo`, `whatis cd` reported the builtin, and `exit` relaunched `rc`.
The vendor U-Boot reports `FB base = 0` and exposes only the serial console, so
it does not hand off a framebuffer. It also reports no SD/eMMC card and no USB
storage. The display path therefore still requires a VOP2/HDMI driver; the
RK3566 VOP compatible is now recognized by the device-tree inventory.
On an Orange Pi 3B v1.1, the common Image and the full OS DTB loaded over TFTP
passed the early memory, MMU, timer, EL0 isolation, scheduler, root walk, and
`#~` boot checks. A later router-hosted TFTP boot of the same Image and the
full OS DTB also passed every driver probe: `DRVMAN` enumerated the on-board
I2C, UART, GPIO, MMC, GMAC, USB, PCIe, HDMI, and codec nodes; `uartfs`,
`drvman`, and `hubfs` reached `PASS`, and the driver manager reported its `i2c`
and `gpio` backends `PASS`. The image now embeds Plan 9
`rc`, its `rcmain`, and a small `/boot/echo`; `init` forks `rc` on the console
after the driver services start and relaunches it after `exit`. UART input is
sampled by the ARM64 clock callback and fed through the console line editor.
`test_rc.py` verifies the prompt, an external `/boot/echo` execution, a
built-in, and exit/relaunch under QEMU. The scheduled libc-backed C process
entered through `_main`, called its generated `nsec` wrapper, wrote its
acceptance marker through a user-only mapping of the RK3566 DesignWare UART,
and returned to the kernel without an exception. A real root filesystem and
storage/network services are still missing. The board's 8 GiB appears as two memory banks in U-Boot;
the kernel currently manages the bank containing the Image (roughly 3.8 GiB)
and leaves the second bank for later multi-bank allocator work. Early code is
built with `-mstrict-align`: before the MMU
is enabled, the RK3566 traps on unaligned multi-byte stores even with
`SCTLR_EL1.A` clear. Replacing EL0 test code at an identity-mapped address
also requires invalidating the instruction-cache alias used by its user
virtual address. A local `ic iallu` currently handles that single-core boot
probe; SMP code replacement will need cross-core synchronization. The board
has no virtio-9P device attached, so the optional `#9` exchange is skipped.

### Rockchip IOMMU

The RK3568/RK3566 IOMMU is implemented in `sys/src/9/aarch64/rkiommu.c`.
It is a two-level Rockchip v2 table: 1024 directory entries, 1024 page-table
entries per directory, and 4 KiB pages.  This gives a 4 GiB IOVA space, while
the RK3568 PTE format carries a 40-bit physical address so pages from the high
DRAM bank can be mapped into the VOP's 32-bit address space.

The implementation includes:

- page-table allocation and teardown
- v2 DTE/PTE encoding
- map/unmap with TLB shootdown
- reset, paging enable/disable, interrupt mask, and auto-gating setup
- page-fault status reporting
- a hardware-independent page-table self-test

The FDT parser now classifies `rockchip,rk3568-iommu` nodes and records the
VOP IOMMU's two register ranges.  The ARM64 MMU build also identity-maps the
SoC device ranges discovered from the device tree; user processes still reach
device MMIO only through `#P`/`segattach` grants.

A Linux 6.18 boot with the VOP2/DW-HDMI drivers initialized a 2560x1440,
32-bpp framebuffer.  Its register state, including VOP, HDMI, CRU, PMU, GRF,
and PMUGRF, is recorded in
`snapshots/rk3566-linux-2560x1440.txt`.  The snapshots show `FB base = 0`
because DRM exposes the display through the VOP IOMMU, not as a physical
address.

The IOMMU page-table self-test passes under QEMU.  Board-level enumeration and
the VOP2/HDMI scanout driver are the next steps.

### Orange Pi 3 (Allwinner H6)

The H6 path is earlier than the RK3566 path. The common early kernel now
recognizes the H6's `arm,gic-400` GICv2, its DesignWare UART0, the ARMv8
timer, and the H6 MMC/GPIO/I2C/EMAC device-tree classes. `test_fdt.py` builds
`testdata/h6-orangepi3.dts` and checks the expected memory, console, GIC,
timer, and class discovery.

Boot contract:

```text
load harvey.img at 0x40080000
load the full OS sun50i-h6-orangepi-3.dtb at 0x48000000
booti 0x40080000 - 0x48000000
```

The early UART assumes firmware/U-Boot has already initialized UART0
(`serial0`, 115200). The next hardware step is a serial boot on the board to
confirm the H6 GIC-400, timer, MMU, and EL0 probes before adding storage or
network drivers.

Still missing for an interactive H6 system:

- sunxi MMC/SD and eMMC user-space driver, for a real root filesystem
- H6 pinctrl/GPIO backend instead of the RK356x register layout
- H6 I2C and AXP805 PMIC/regulator support
- H6 EMAC (`dwmac-sun8i`) plus the board PHY/MDIO setup
- USB, HDMI/display, and thermal/RTC support
- a durable U-Boot/ATF/SD-card boot image rather than manual TFTP/`booti`

### SoC family and board drivers

The RK3566 shares its SoC family with the RK3568 ("rk356x"). SoC-specific
register layouts live in `sys/include/rockchip/rk356x.h`: GPIO bank offsets,
the GRF/PMUGRF/CRU base addresses, and the pinmux (iomux) formula. The kernel
already classifies the board's peripherals from the device tree through `#P`,
so each driver discovers its live base addresses from `#P`, maps the MMIO with
`segattach` (the resource grant), and serves 9P.

That discovery, mapping, 9P-tree and health boilerplate is the driver kit:
`sys/include/drv.h` and `sys/src/cmd/drvkit/drv.c`. A new driver calls
`drvfind`/`drvmap`, keeps its register code, describes its files as `DrvFile`
entries and posts them with `drvsrv`; `drvhealthrun` keeps it in `drvman`'s
table. `uartfs` is the worked example, and
[`sys/src/cmd/drvkit/README.md`](../../sys/src/cmd/drvkit/README.md) has the
API, the 9front-to-drvkit mapping and the limits (no IOMMU isolation, one wait
per interrupt, no PCI config space on amd64).

The first driver is `gpiofs` (`sys/src/cmd/gpiofs/gpiofs.c`), served at
`/srv/gpio`:

- `/status` lists the discovered banks and pinmux.
- `/gpioN/data` reads the actual pin state, or writes the output data.
- `/gpioN/dir` reads/writes the 32-bit direction mask (1 = output).
- `/gpioN/ctl` takes `"N in"` or `"N out [0|1]"` and muxes that pin to GPIO.

The five RK3566 banks (GPIO0..GPIO4) are discovered and sorted by base address;
GPIO0's pinmux is in PMUGRF, GPIO1-4's in GRF. The mux offsets and the V2 GPIO
register layout are encoded in the family header and need on-board verification
against the RK3566 TRM. `init` launches gpiofs non-fatally; the QEMU `virt`
machine has no GPIO banks, so it reports `GPIOFS PENDING` there and `PASS` on
the real board.

For the tested v1.1 board, the installed vendor U-Boot needed a 100 Mbps PHY
workaround before the TFTP transfer. With a TFTP server at `192.168.2.1`
serving `aspera-arm64.img` and the full `orangepi-3b-v1.1.dtb`, the serial
console sequence was:

```text
setenv ipaddr 192.168.2.250; setenv netmask 255.255.255.0; setenv serverip 192.168.2.1
ping 192.168.2.1
<interrupt the first failed ping with Ctrl-C>
mii write 0 9 0; mii write 0 0 0x1200; mii write 0 1e 0xa003; mii write 0 1f 0x40f1
ping 192.168.2.1
tftpboot 0x40080000 aspera-arm64.img
tftpboot 0x48000000 orangepi-3b-v1.1.dtb
booti 0x40080000 - 0x48000000
```

The first ping initializes the PHY and may need Ctrl-C to return; the MDIO
changes must follow that initialization. The board then reached the 9ring
test at roughly 3.8 MiB/s TFTP throughput. U-Boot's automatic `bootm` attempt
after each TFTP transfer reports an image error; the explicit `booti` command
is required. The repeatable development path is a serial boot script; the
durable fix is in the board's U-Boot PHY setup and boot script, keeping these
MDIO commands out of the common kernel.

This uses QEMU's direct ARM64 Image loading. No UEFI or Limine is involved.
The Image header and x0 device tree handoff also match U-Boot's `booti`
interface. The early parser supports root-level 1- or 2-cell
`reg` addresses, multiple RAM ranges, PL011 or DesignWare UART, and GICv2 or
GICv3. It currently manages only the contiguous RAM bank containing the Image
and requires device registers to have direct physical `reg` addresses; bus
`ranges` translation and runtime kernel relocation are future work. The GICv3 timer path has passed its boot probes on the RK3566; broader
device interrupt routing still needs work. The early allocator hands out
zeroed 4 KiB pages; the full build also
has a free-list allocator for process pages and page tables.
The identity map remains writable and executable in EL1, while its entries are
inaccessible to EL0. Because user and kernel share TTBR0, user text starts
above the identity-mapped window (`UTZERO` is 16 GiB, matching `Maxidentity`
in `mmu.c`) and must stay there until the kernel moves to TTBR1. Separate user
roots and a complete general-register trap frame are in place. The lower-EL synchronous trap path can now dispatch a
scheduled process's register-based syscall through the generated syscall
table. Error unwinding and return-value propagation are exercised by the boot
probe, but notes and the complete production syscall-exit path remain. The
linked kernel has `Proc`/`Mach`, page mapping, the shared scheduler and core
devices. The runtime handoff runs `procinit0()`, `psinit()`, `devtabreset()`,
`pageinit()`, `timersinit()`, and `schedinit()`. A scheduled EL0 probe runs after
the root walk and local transport checks. Before launching general untrusted
programs, resource grants and the remaining process lifecycle paths still need
hardening. ARM64 libc and all 42 generated syscall wrappers now cross-build,
and a small `_main` process links against that libc. The embedded probe is
loaded as ELF. A second, normally segmented bootstrap process calls
`exec("/boot/init")`; the shared ELF loader replaces its address space, and the
filesystem-backed ARM64 init image is demand-paged through tracked per-process
page tables. The same build cross-compiles the Objective-9C runtime and Harvey
support libraries, then packages the ARM64 `o9drvman`, `o9xhci`, and
`usbpower` ELF images as `/boot/o9drvman`, `/boot/o9xhci`, and
`/boot/usbpower`. Init launches the C `drvman` on the board; the driver manager
starts `usbpower` after `gpiofs` and before any USB controller service.
The shared `#9` driver uses PCI on amd64 and device-tree discovered virtio-MMIO
on ARM64. Both legacy MMIO v1 and modern v2 work in QEMU `virt`. The ARM64
transport currently polls for replies; device interrupts are not routed yet.
`#~` provides named local IPC on ARM64 and has a tagged request/response boot
check. Device services belong in user-space 9P servers. Each driver uses the
shared `DriverHealth` writer for readiness, health, and stop markers and
receives hardware access through `#P` resource grants. The ARM64 user-mode UART
probe proves the privilege boundary needed by a driver server; a restricted
resource-grant interface is still required.

The build uses `clang-23` if present because this host's default `clang` only
contains the x86 backend.

`o9c` (Objective-9C's compiler) is a Plan 9 program: the copy built inside
the 9front VM is a Plan 9 a.out and cannot run on a Linux build host.  Build
a host copy first with plan9port's `9c`/`9l`:

```sh
sh util/arm64/build_o9c.sh          # writes build/host/o9c
python3 util/arm64/build_userland.py --output build/arm64-o9/userland.h \
        --archive build/arm64-o9/userland.img
```

`build_o9_services.py` prefers `build/host/o9c` and falls back to the repo
copy, so a host without plan9port still builds when the sources were
transpiled elsewhere.  The host build was verified end to end: all eight
services transpile with it, compile and link, and the userland and kernel
built from a clean tree boot to `rc` under QEMU `virt`. Kernel sources and integer/text kernel libc use
`-mgeneral-regs-only`. Floating-point libc routines retain the standard ARM64
ABI; they must stay unused until per-process FP state is managed.
Set `ARM64_CC`, `ARM64_LD` and `ARM64_OBJCOPY` to override the tools.

`check_port.py` remains a fast compile-only check. `test_full_kernel.py` checks
the complete link for undefined symbols and boots that linked Image under QEMU
with 256 and 512 MiB of RAM. It exercises physical allocation and free after
the early MMU and trap probes, then checks device reset, page allocation,
process scheduling, clock ticks, the root walk, a scheduled EL0 process, its
restricted UART mapping, and generated syscall-table dispatch.
`test_virtio9p.py` attaches a QEMU host directory and checks a real 9P version
exchange through both MMIO transport versions. The boot check also exercises
the local `#~` request and reply path without a host ring.
The 32 MiB static kernel heap is a bring-up
limit; it will need growth and accounting before general use.

## APEX and a native clang

The POSIX surface on ARM64 is APEX (`apex/`) over the Plan 9 libc, plus
`util/libcxx` and libc++/libunwind/libc++abi.  It is built with
`util/arm64/build_apex.py`, `build_libcxx.py` and `build_libunwind.py`; the
result is what a guest program links:

    libc++  libunwind  libc++abi   C++ runtime
    libap.a                        POSIX/ANSI: stdio, errno, TLS, startup, mmap
    libc.a                         Plan 9 libc: syscalls, fmt, allocator

`util/arm64/install-sdk.py` lays that stack out as a self-contained tree
(clang, clang++, ld.lld, resource headers, machine/apEX/libc++ includes,
archives, `apex.ld`) whose driver configs use the guest root, and can emit it
as a ustar archive wrapped in an `O9BOOT01` boot archive.  `mkrun.py` loads
that archive at 0x50000000, unpacks it into a ramfs and runs a program from
it, so a 360M toolchain reaches the guest without being embedded in the
kernel image.

Building clang for the guest found a series of platform bugs, all fixed at
the source rather than worked around:

* `writev()` returned `i32`, so a failed stdout write became +4294967295 and
  `__stdio_write` retried with a wild pointer.
* APEX's `setenv()` built `"#e/\0NAME"` and silently failed, so variables
  never reached `#e`/`/env` and `execve` dropped them.
* `stat()` decided a file was a character device from `Dir.type`, which is
  the *device letter* for namespace files, so every ramfs file looked like a
  device and LLVM rejected its own config files.
* `ino_t` was 16 bits.  LLVM's FileManager keys files by (st_dev, st_ino),
  and `dirent.h` collided with `stdint.h`; clang returned the cached
  `stdint.h` and silently skipped `dirent.h`.
* ramfs capped a file at 32M and read a directory as if it were a byte
  stream, so a directory whose entries did not fit one 8K block could not be
  listed at all (clang saw `__type_traits` as empty).
* the kernel heap was a static 32M array; it now grows from `physalloc()`.
* libc had a 32-bit `labs`, no `llabs`, and only part of the libm surface.
* user text started at 2 MiB in the same TTBR0 address space as the kernel
  identity map.  Clang is about 120 MB, so its data segment crossed
  0x08000000 and replaced the kernel's GIC mapping; every timer tick then
  read a spurious interrupt ID, the timer was never acknowledged, and the
  CPU stayed in the exception vector forever.  User text now starts at
  16 GiB, above every identity mapping.
* the ARM64 kernel never called `fmtinit()`, so the `%q` format verb was
  never installed.  `sysawait()` wrote the literal `%q%` where the exit
  message belonged, APEX's `waitstatus()` could not parse it, and every
  child was reported as exiting with status 1.  Clang consequently thought
  its successful `ld.lld` child had failed and never kept the output
  binary.  `arm64kernelstart()` now installs the format verbs, as amd64
  and riscv already did.

With those fixed, `clang++ hello.cpp -o hello` compiles, links against the
guest's libc++ and APEX archives, and the resulting static binary runs on the
ARM64 guest and prints its marker.

The 32 MiB heap limit noted above is gone: `qmalloc.c` draws new chunks from
the physical allocator, doubling up to 64 MiB, and ramfs block maps grow on
demand.
