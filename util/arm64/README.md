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
interrupted instruction. It does **not** yet start the Plan 9 scheduler or load
user programs. The ARM64 `Label` save/restore routines now match the calling
convention used by Harvey's shared scheduler in `sys/src/9/port/proc.c`. QEMU
tests switch between two independent kernel stacks and check callee-saved
register preservation while the timer is active. Interrupt masking is also
checked across two timer periods.

`build_kernel.py` now compiles and links the shared port and IP kernel sources,
the local `#~` ring device, ARM64 architecture sources, and ARM64 builds of
kernel libc, libip, and libsec. The output is an AArch64 ELF and direct-boot
Image. This is a link milestone: the Image enters the same boot diagnostic and
then waits; it does not yet enter Harvey's scheduler or load a user program.

```sh
python3 util/arm64/test_virt.py
python3 util/arm64/test_fdt.py
python3 util/arm64/check_port.py
python3 util/arm64/test_full_kernel.py
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

This uses QEMU's direct ARM64 Image loading. No UEFI or Limine is involved.
The Image header and x0 device tree handoff also match U-Boot's `booti`
interface. The link address and initial diagnostic UART are specific to QEMU
`virt`. The early parser supports root-level 1- or 2-cell `reg` addresses, a
single RAM range, PL011 and GICv2. The GIC path has only been exercised with
QEMU's default GICv2; GICv3 and real board interrupt routing need separate
drivers. The early allocator hands out zeroed 4 KiB pages; the full build also
has a free-list allocator for process pages and page tables.
The identity map remains writable and executable in EL1, while its entries are
inaccessible to EL0. Separate user roots and a complete general-register trap
frame are in place, but the current trap handler is still a boot check. The
linked kernel has `Proc`/`Mach`, page mapping, the shared scheduler and core
devices, but its runtime handoff has not been wired to `procinit0()` and
`schedinit()`. Before launching untrusted processes, it also needs a separate
high address kernel mapping: the bootstrap identity map overlaps the range
used for user addresses. Normal syscall dispatch, demand-fault handling,
timer-driven `hzsched`, executable loading, and an ARM64 init binary follow.
The existing virtio-9P transport uses x86 PCI and requires an ARM64 virtio
transport; `#~` is linked for local endpoints. Storage, networking and display
drivers follow after the serial userland boot path works.

The build uses `clang-23` if present because this host's default `clang` only
contains the x86 backend. Kernel sources use `-mgeneral-regs-only`. Kernel
libc includes floating-point formatting and math and uses the standard ARM64
ABI; those routines must stay unused until per-process FP state is managed.
Set `ARM64_CC`, `ARM64_LD` and `ARM64_OBJCOPY` to override the tools.

`check_port.py` remains a fast compile-only check. `test_full_kernel.py` checks
the complete link for undefined symbols and boots that linked Image under QEMU
with 256 and 512 MiB of RAM. It exercises physical allocation and free after
the early MMU and trap probes. The 32 MiB static kernel heap is a bring-up
limit; it will need growth and accounting before general use.
