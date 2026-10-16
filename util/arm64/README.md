# Harvey ARM64 bring-up

The ARM64 Image boots directly on QEMU `virt`. It enters Harvey's ARM64
assembly and C, clears BSS, sets a stack, parses the device tree to discover
RAM, PL011, GICv2 and the timer PPI, then receives three virtual timer
interrupts through an EL1 vector. It reports synchronous exceptions over
serial. It does **not** yet start the Plan 9 scheduler or user processes.

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
drivers. The next kernel milestones are reserved-memory discovery, physical
page management and MMU, then process context, syscalls and userland. Storage,
networking and display follow after a serial shell boots.

The build uses `clang-23` if present because this host's default `clang` only
contains the x86 backend. `-mgeneral-regs-only` prevents use of SIMD registers
before the kernel has enabled and saved their state. Set `ARM64_CC`, `ARM64_LD`
and `ARM64_OBJCOPY` to override the compiler, linker and binary converter.
