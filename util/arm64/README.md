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
QEMU image now provides an interactive `rc`; hardware verification is pending
after reflashing the board with the same image.
On an Orange Pi 3B v1.1, the common Image and the full OS DTB loaded over TFTP
passed the early memory, MMU, timer, EL0 isolation, scheduler, root walk, and
`#~` boot checks. A later router-hosted TFTP boot of the same Image and the
full OS DTB also passed every driver probe: `DRVMAN` enumerated the on-board
I2C, UART, GPIO, MMC, GMAC, USB, PCIe, HDMI, and codec nodes; `uartfs`,
`drvman`, `o9driverfs`, and `hubfs` reached `PASS`, and the driver manager
reported its `i2c` and `gpio` backends `PASS`. The image now embeds Plan 9
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
inaccessible to EL0. Separate user roots and a complete general-register trap
frame are in place. The lower-EL synchronous trap path can now dispatch a
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
support libraries, then packages ARM64 `o9drvman` and `o9driverfs` ELF images
as `/boot/o9drvman` and `/boot/o9driverfs`. Init does not launch them yet.
The shared `#9` driver uses PCI on amd64 and device-tree discovered virtio-MMIO
on ARM64. Both legacy MMIO v1 and modern v2 work in QEMU `virt`. The ARM64
transport currently polls for replies; device interrupts are not routed yet.
`#~` provides named local IPC on ARM64 and has a tagged request/response boot
check. Device services belong in the Objective-9C
`sys/src/cmd/o9driverfs` user-space 9P filesystem, with backends supplied by a
trusted host or VM transport. That service currently proxies named backend
files and is tested in the x86 microVM. The ARM64 user-mode UART probe proves
the privilege boundary needed by a driver server, but it is deliberately not
the final Objective-9C `o9uartfs`: a restricted resource-grant interface is
still required.

The build uses `clang-23` if present because this host's default `clang` only
contains the x86 backend. Kernel sources and integer/text kernel libc use
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
