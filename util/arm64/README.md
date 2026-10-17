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
register preservation while the timer is active. The build still links only
the boot probe; it does not yet link `port/proc.c`.

```sh
python3 util/arm64/test_virt.py
python3 util/arm64/test_fdt.py
```

For interactive inspection:

```sh
python3 util/arm64/build_virt.py
qemu-system-aarch64 -machine virt -cpu cortex-a57 -m 512M -smp 1 \
    -nographic -monitor none -kernel build/arm64/harvey-virt.img
```

This uses QEMU's direct ARM64 Image loading. No UEFI or Limine is involved.
The Image header and x0 device tree handoff also match U-Boot's `booti`
interface. The link address and initial diagnostic UART are specific to QEMU
`virt`. The early parser supports root-level 1- or 2-cell `reg` addresses, a
single RAM range, PL011 and GICv2. The GIC path has only been exercised with
QEMU's default GICv2; GICv3 and real board interrupt routing need separate
drivers. The allocator hands out zeroed 4 KiB pages but has no free operation.
The identity map remains writable and executable in EL1, while its entries are
inaccessible to EL0. Separate user roots and a complete general-register trap
frame are in place, but the current syscall handler is a boot check. The next
milestones are ARM64 `Proc`/`Mach` definitions, process MMU hooks, timer-driven
`hzsched`, and linking Harvey's existing `port/proc.c`, followed by normal
syscall dispatch, executable loading and userland. Storage, networking and
display follow after a serial shell boots.

The build uses `clang-23` if present because this host's default `clang` only
contains the x86 backend. `-mgeneral-regs-only` prevents use of SIMD registers
before the kernel has enabled and saved their state. Set `ARM64_CC`, `ARM64_LD`
and `ARM64_OBJCOPY` to override the compiler, linker and binary converter.
