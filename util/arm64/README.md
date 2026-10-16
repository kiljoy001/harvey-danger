# Harvey ARM64 bring-up

The first milestone is a direct-boot ARM64 Image for QEMU `virt`. It enters
Harvey's own ARM64 assembly and C, clears BSS, sets a stack, validates the
firmware-provided device tree header, and writes to the QEMU PL011 serial port.
It does **not** yet start the Plan 9 kernel scheduler or user processes.

```sh
python3 util/arm64/test_virt.py
```

For interactive inspection:

```sh
python3 util/arm64/build_virt.py
qemu-system-aarch64 -machine virt -cpu cortex-a57 -m 512M -smp 1 \
    -nographic -monitor none -kernel build/arm64/harvey-virt.img
```

This uses QEMU's direct ARM64 Image loading. No UEFI or Limine is involved.
The Image header and x0 device tree handoff also match U-Boot's `booti`
interface. The link address and UART are currently specific to QEMU `virt`;
a board port must select its memory map and obtain devices from the device
tree. The next kernel milestones are a real DT parser, timer and interrupt
controller, physical memory management and MMU, then traps, process context,
syscalls and userland. Storage, networking and display follow after a serial
shell boots.

The build uses `clang-23` if present because this host's default `clang` only
contains the x86 backend. Set `ARM64_CC`, `ARM64_LD` and `ARM64_OBJCOPY` to
override the compiler, linker and binary converter.
