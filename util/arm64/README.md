# Harvey ARM64 bring-up

The ARM64 Image boots directly on QEMU `virt`. It enters Harvey's ARM64
assembly and C, clears BSS, sets a stack, parses the device tree to discover
RAM, PL011, GICv2 and the timer PPI. A monotonic early allocator excludes the
kernel image, DTB, firmware memory reservations and `/reserved-memory` `reg`
ranges. An identity map enables the MMU and caches with normal attributes for
RAM and device attributes for MMIO. The guest then receives three virtual timer
interrupts through an EL1 vector and reports synchronous exceptions over
serial. It enters EL0, resumes after a test syscall, and switches between two
page-table roots whose user mappings point to different physical pages. The
boot check confirms that EL0 cannot read kernel memory, write its code page,
or execute its stack. A timer interrupt can arrive from EL0 and return to the
interrupted instruction. The full kernel starts the shared Plan 9 scheduler;
it does **not** yet load user programs. The ARM64 `Label` save/restore routines match the calling
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
fault diagnostics still write directly to QEMU's PL011 before user space is
available; this temporary bootstrap console is not a serial service.
It does **not** yet offer an interactive shell.

```sh
python3 util/arm64/test_virt.py
python3 util/arm64/test_fdt.py
python3 util/arm64/check_port.py
python3 util/arm64/test_full_kernel.py
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

For the Orange Pi 3B RK3566 bring-up image, build separately so the QEMU
image remains available for regression tests:

```sh
ARM64_BOARD_RK3566=1 ARM64_KERNEL_OUT=build/arm64-rk3566 \
    python3 util/arm64/build_kernel.py
```

This image is linked and loaded at `0x40080000`. Its ARM64 Image header sets
the relocatable-kernel flag so U-Boot's `booti` path can retain that address.
Boot it with the board DTB as `booti 0x40080000 - <fdt address>` after loading
the Image into RAM. The board build selects UART2 at `0xfe660000` before its
first diagnostic, and its early allocator uses a conservative 1 GiB RAM window
starting at `0x40000000`. The NPU register/DMA probe is excluded from normal
boot; set `ARM64_RK3566_NPU_PROBE=1` only for a separate NPU experiment. The
board boot remains a hardware bring-up test, not an interactive userland.
For parser diagnosis, `ARM64_RK3566_TRACE_FDT=1` prints short UART milestones.
`ARM64_RK3566_SKIP_FDT=1` builds a temporary diagnostic image with fixed board
addresses; it must not be used as the normal boot image because it ignores the
DTB's reserved-memory entries. On an Orange Pi 3B v1.1, U-Boot `booti` has
loaded the Image over TFTP and the vendor DTB has parsed successfully. The
board passed the early memory, MMU, timer, EL0 isolation, scheduler, root walk,
and `#~` boot checks. Early code is built with `-mstrict-align`: before the MMU
is enabled, the RK3566 traps on unaligned multi-byte stores even with
`SCTLR_EL1.A` clear. Replacing EL0 test code at an identity-mapped address
also requires invalidating the instruction-cache alias used by its user
virtual address. A local `ic iallu` currently handles that single-core boot
probe; SMP code replacement will need cross-core synchronization. The board
has no virtio-9P device attached, so the optional `#9` exchange is skipped.

For the tested v1.1 board, U-Boot's current Ethernet setup needed a 100 Mbps
PHY workaround before the TFTP transfer. With a TFTP server at `192.168.2.1`
serving the board Image, the serial console sequence was:

```text
setenv ipaddr 192.168.2.250; setenv netmask 255.255.255.0; setenv serverip 192.168.2.1
ping 192.168.2.1
<interrupt the first failed ping with Ctrl-C>
mii write 0 9 0; mii write 0 0 0x1200; mii write 0 1e 0xa003; mii write 0 1f 0x40f1
ping 192.168.2.1
tftpboot 0x40080000 aspera-rk3566.img
booti 0x40080000 - 0xeb9f5e30
```

The `0xeb9f5e30` address is the vendor DTB address observed in this U-Boot
session and must be checked after each reboot. This probe reached the 9ring
test at roughly 3.6 MiB/s TFTP throughput. U-Boot's automatic `bootm` attempt
after TFTP reports an image error; the explicit `booti` command is required.

This uses QEMU's direct ARM64 Image loading. No UEFI or Limine is involved.
The Image header and x0 device tree handoff also match U-Boot's `booti`
interface. Board builds select an Orange Pi link address and UART; the default
build uses QEMU `virt`. The early parser supports root-level 1- or 2-cell
`reg` addresses, a single RAM range, PL011 or DesignWare UART, and GICv2 or
GICv3. The GICv3 timer path has passed its boot probes on the RK3566; broader
device interrupt routing still needs work. The early allocator hands out
zeroed 4 KiB pages; the full build also
has a free-list allocator for process pages and page tables.
The identity map remains writable and executable in EL1, while its entries are
inaccessible to EL0. Separate user roots and a complete general-register trap
frame are in place, but the current trap handler is still a boot check. The
linked kernel has `Proc`/`Mach`, page mapping, the shared scheduler and core
devices. The runtime handoff runs `procinit0()`, `psinit()`, `devtabreset()`,
`pageinit()`, `timersinit()`, and `schedinit()`. The test kernel processes exit
after the root walk. Before launching untrusted processes, it needs a separate
high address kernel mapping: the bootstrap identity map overlaps the range
used for user addresses. Normal syscall dispatch, demand-fault handling,
executable loading, and an ARM64 init binary follow.
The shared `#9` driver uses PCI on amd64 and device-tree discovered virtio-MMIO
on ARM64. Both legacy MMIO v1 and modern v2 work in QEMU `virt`. The ARM64
transport currently polls for replies; device interrupts are not routed yet.
`#~` provides named local IPC on ARM64 and has a tagged request/response boot
check. Device services belong in the Objective-9C
`sys/src/cmd/o9driverfs` user-space 9P filesystem, with backends supplied by a
trusted host or VM transport. That service currently proxies named backend
files and is tested in the x86 microVM; it does not access PL011 MMIO directly.
The ARM64 kernel still needs the user process and syscall boot path before it
can start `o9driverfs` or another Objective-9C service there.

The build uses `clang-23` if present because this host's default `clang` only
contains the x86 backend. Kernel sources and integer/text kernel libc use
`-mgeneral-regs-only`. Floating-point libc routines retain the standard ARM64
ABI; they must stay unused until per-process FP state is managed.
Set `ARM64_CC`, `ARM64_LD` and `ARM64_OBJCOPY` to override the tools.

`check_port.py` remains a fast compile-only check. `test_full_kernel.py` checks
the complete link for undefined symbols and boots that linked Image under QEMU
with 256 and 512 MiB of RAM. It exercises physical allocation and free after
the early MMU and trap probes, then checks device reset, page allocation,
process scheduling, clock ticks, and the root walk.
`test_virtio9p.py` attaches a QEMU host directory and checks a real 9P version
exchange through both MMIO transport versions. The boot check also exercises
the local `#~` request and reply path without a host ring.
The 32 MiB static kernel heap is a bring-up
limit; it will need growth and accounting before general use.
