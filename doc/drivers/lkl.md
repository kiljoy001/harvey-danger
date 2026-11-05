# LKL as the final driver architecture

Status: adopted, in progress
Date: 2026-11-05

## Decision

Harvey's board support becomes a **small native core plus one or more Linux
kernel instances (LKL) that own the complex peripherals**, with 9P as the
boundary between them.  Native code provides the things a driver host cannot
provide for itself: boot, console, memory, interrupts, the resource manager,
display/rio, and the Plan 9 network stack.  Everything else is driven by the
real Linux drivers inside LKL.

This replaces the "semantic re-implementation of each Linux driver" plan.  A
re-implementation can only ever approach the original; running the original
is the definition of done.

## Correctness rule

- **The Linux driver is the oracle.**  For this SoC the reference is the
  vendor tree `9rock/linux-rockchip` 6.1.115, because that is what the
  silicon was validated against.  Mainline is a secondary reference.
- **Parity first.**  A behaviour is "done" when it matches the reference:
  same register sequences, same error classification, same retry counts,
  same timeouts, same state transitions, same power behaviour.
- **Deviations are explicit.**  A deviation is allowed only for: an erratum
  workaround, a Plan 9 interface requirement, additive observability, or a
  measured improvement.  Each one is recorded with the reason, the test that
  proves it, and the flag that isolates it.  "Cleaner" or "simpler" is not a
  reason.
- **Hardware capability beyond the vendor stack is a legitimate outcome**,
  but only with a probe, a golden reference, and a number.  The NPU is the
  example: vendor parity defines the supported subset; probes define the
  ceiling.

## Shape

```text
        rc, rio, filesystem servers            (unchanged)
                    |  9P
        +-----------+-----------+
        |  driver servers       |
        |  lklemc, lklusb, ...  |   each = one LKL kernel + 9P export
        +-----------+-----------+
                    |  host ops
        +-----------+-----------+
        |  #P platform bus      |   mmio, irq, dma, clk, reset, regulator,
        |  (kernel)             |   gpio, power-domain, inventory
        +-----------+-----------+
        |  native core          |   boot, console, GIC, timer, MMU, PSCI,
        |                       |   display/rio, #l + IP
        +-----------+-----------+
```

One process per driver **family**, not per driver.  Families are defined by
shared hardware or bus: mmc+sdio, xhci+classes, drm, audio, camera.  Splitting
a family recreates the ownership problem inside one device.

## Resource contract (`#P`)

`#P` is the platform bus.  For every device node it must expose, in a form LKL
can turn into a `platform_device`:

- `reg` / `size` (physical base and length)
- `irq` / `irqflags`
- `compatible`, `class`
- clock and reset references (enable, disable, rate)
- regulator references (enable, voltage)
- power-domain references
- GPIO references (reset, card-detect, enable)
- DMA constraints (address width, coherency, IOMMU)

Ownership: **one owner per device**.  The boot configuration selects native or
LKL for each device; `#P` does not arbitrate, so the selection must.

Open question: whether clocks/regulators/power domains are owned by native
board code (LKL gets fixed-clock/fixed-regulator shims) or by LKL (Linux
clk/regulator frameworks, same FDT).  Boot-critical rails stay native; devices
driven by LKL get their clocks from LKL, one owner per controller.

## Family model (adopted from lux9-kernel)

Lux9's HAL groups hardware into **families** (PCI, USB, I2C) registered in a
`FamilyRegistry` and routed through a congruent 9P tree (`/dev/family/...`).
Each family exposes per-device resources (`config`, `bar{N}`) and a `ctl`;
a channel id identifies an access grant; an exchange page carries zero-copy
requests and IRQ events; a borrow checker enforces single-writer ownership.
Its rump driver server plan is the same shape as this one: a `rumpuser`
backend (clock, threads, locks, block I/O) over a HAL, exposed as 9P,
storage first.

Harvey adopts the parts that fit a process-per-family design:

- **`#P` as the family router.** `#P/<family>/<device>/...` with `reg`,
  `size`, `irq`, `irqflags`, `compatible`, `class`, the resources from the
  contract, and a `ctl` for `claim`, `release`, `reset`, `scan`.
- **Claim/lease instead of a token game.** One owner per device; shared
  readers for monitoring; explicit transfer. `#P` records the owner and
  refuses a second claim. This is the borrow-checker idea reduced to what
  the boot model needs, and it enforces "one owner per device" in the
  kernel rather than by convention.
- **Exchange ring for events (later).** `#P/<dev>/wait` is enough for eMMC;
  a shared ring for IRQ and completion events is the right step for network
  and USB families, where per-event reads dominate.
- **Family manifest and supervisor.** A family registers with the
  supervisor (name, devices, endpoint); the supervisor mounts and restarts
  it. The `lkl.devices` file in a BSP is the manifest.
- **Error translation table.** Linux `errno` to Plan 9 strings, in one
  place, so a driver failure is legible to the 9P client.

Not adopted: WASM-inside-HAL isolation (a process is a stronger boundary),
Pebble/CLR tokens (not needed here), and the kernel "diverter" (Harvey has no
native driver to divert; the server *is* the driver).

Also borrowed from the rump plan: storage first (eMMC only, no SD slot),
the `rumpuser`-style host mapping (identical to the LKL host ops), the 9P
POSIX bridge, boot integration, and the functional test list (stat, read,
write, readdir, seek, rename, write/reboot/read back).

## Memory

- A single physically contiguous arena is reserved at boot from the top of
  usable RAM, before the page allocator exists, and published as `#P/dma`.
- Target size: **1 GiB for LKL plus 256 MiB** for other user-space DMA users.
- LKL's guest RAM is that arena.  `arch/lkl` `__pa`/`__va` become an offset
  translation through the arena base (`virt - ram_va + ram_pa`), not identity.
  Every Linux allocation (page tables, slab, `kmalloc`,
  `dma_alloc_coherent`) then has a real, device-addressable physical address.
- **The arena must sit below 4 GiB** while any enabled controller uses 32-bit
  DMA descriptors (the eMMC ADMA2 path does).  Verify per SoC whether 64-bit
  descriptors are supported before relaxing this.
- Cache maintenance for DMA goes through `#P/dma/clean|inval|cleaninval`.
- DMA on stack or static kernel buffers is invalid; drivers use `kmalloc` or
  `dma_alloc_coherent`.  This is documented, not worked around.

## Host interface

`arch/lkl/include/uapi/asm/host_ops.h` mapped to Harvey:

| LKL host op | Harvey |
| --- | --- |
| `sem_*`, `mutex_*` | `Lock`, `QLock`, `Rendez` |
| `thread_*`, `tls_*` | `rfork` + per-process private data |
| `mem_alloc`, `page_alloc`, `mmap` | the `#P/dma` arena |
| `time`, `timer_*` | `nsec()`, timer threads |
| `ioremap`, `iomem_access` | `#P/<dev>/reg` + `segattach` (as `emmcfs` does) |
| IRQ delivery | `#P/<dev>/wait` thread -> `lkl_trigger_irq()` |
| `jmp_buf_*` | `setjmp`/`longjmp` |
| `print`, `panic` | console |

`ioremap` resolves a physical address to the `#P` node whose `reg` covers it,
maps the 2 MiB big page, and returns the virtual address.  `iomem_access` is
not used for hot paths (the driver uses `readl`/`writel` on the mapping).

## Instances

| Instance | Owns | Exposes | RAM |
| --- | --- | --- | --- |
| `lklemc` | dwcmshc + mmc core + block | `/srv/emmc` block device | 512 MiB |
| `lklusb` | xhci + usb core + hubs + hid + storage | `/srv/input`, `/srv/usbN` | 256 MiB |
| `lklnet` | stmmac or sdio + wifi | raw frames / 9P net | 256 MiB |
| `lklmedia` | vpu/vepu/rga | codec devices | 256 MiB |
| `lklmisc` | thermal, wdt, crypto, rng, pcie | `/srv/misc` | 128 MiB |

Native stays: boot, console, GIC/timer/MMU/PSCI, `#P`, GPIO/I2C/PMIC/UART,
VOP2/HDMI/rio, `#l` + IP, USB PHY/port power.

## Build

- One LKL binary per configuration, cross-compiled on the host.  The board
  does not build LKL.
- One vendor kernel tree per SoC family, pinned by commit; host layer, `#P`
  board file and DMA ops live in clearly separated files so a rebase is
  mechanical.
- The device list is selected at run time (`-d dwcmshc -m 512M -s /srv/emmc`),
  so one binary serves several instances.
- Keep the native `emmc.c` as the fallback path behind a boot switch until LKL
  has read and written sector 0 across a power cycle.

## Build recipe (validated 2026-11-05)

LKL tree: `/home/scott/Repo/lkl` (6.12 with the LKL arch patches).
Host: x86_64, LLVM 23.  Target: aarch64, `ARCH=lkl`, no MMU, 64-bit.

The system `clang` is LLVM 22 and has no AArch64 target; use the LLVM 23
binaries.  `ld.lld` must be told the emulation, and the config's
`OUTPUT_FORMAT` must be set for the target because defconfig detects it with
the host compiler.

```sh
LLVM=/usr/lib/llvm-23/bin
# a directory with clang/llvm-* from LLVM 23 and ld.lld from the system
mkdir -p /tmp/lkl-tools && cd /tmp/lkl-tools
for t in clang llvm-ar llvm-nm llvm-objcopy llvm-objdump llvm-strip; do
    ln -sf $LLVM/$t .
done
ln -sf $(which ld.lld) ld.lld

make -C /home/scott/Repo/lkl ARCH=lkl O=/tmp/lkl-build \
     LLVM=/tmp/lkl-tools/ CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu defconfig

# defconfig detected the host format; override for the target
/home/scott/Repo/lkl/scripts/config --file /tmp/lkl-build/.config \
     --set-str OUTPUT_FORMAT elf64-littleaarch64
make -C /home/scott/Repo/lkl ARCH=lkl O=/tmp/lkl-build \
     LLVM=/tmp/lkl-tools/ CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu \
     LD="/tmp/lkl-tools/ld.lld -m aarch64elf" olddefconfig

# kernel object
make -C /home/scott/Repo/lkl ARCH=lkl O=/tmp/lkl-build \
     LLVM=/tmp/lkl-tools/ CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu \
     LD="/tmp/lkl-tools/ld.lld -m aarch64elf" -j8

make -C /home/scott/Repo/lkl ARCH=lkl O=/tmp/lkl-build \
     LLVM=/tmp/lkl-tools/ CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu \
     LD="/tmp/lkl-tools/ld.lld -m aarch64elf" \
     INSTALL_PATH=/tmp/lkl-install install
```

Result: `/tmp/lkl-install/lib/lkl.o`, ELF64 aarch64 relocatable, ~207 MiB
with debug info (strip before deployment), headers under
`/tmp/lkl-install/include`.  The only undefined symbols are `lkl_bug` and
`lkl_printf`, both provided by the host layer; everything else resolves
inside the object or through `lkl_host_operations`.

M1 therefore reduces to `harvey-host.c` (the ops table, the two symbols,
and the arena) plus a link script that keeps the kernel's `.init.*` sections.

## Supervision

A supervisor (extension of `drvman`) starts instances in dependency order,
mounts their 9P endpoints, and restarts a dead one.  A killed driver server
must not take storage, console or the network with it.  Consumers tolerate a
disappearing endpoint as they do for any Plan 9 server.

## Milestones

- **M1 — LKL builds and boots for aarch64.** Minimal config, banner printed
  inside a Harvey process; arena handed over as `MemTotal`.
- **M2 — eMMC.** `sdhci` + `sdhci-of-dwcmshc` probe, `/dev/mmcblk0` readable
  with ADMA, then writable, then across a power cycle.
- **M3 — 9P.** `emmcfs` becomes a shim over `lkl_sys_pread/pwrite/fsync`;
  ext2 root and gefs mount and boot from it.
- **M4 — USB.** Enumerate, HID to `/srv/input`, MSC as a 9P block device.
- **M5 — Reuse.** A second family (WiFi or media) proves the model.

## BSP template

A new board is data, not a driver project:

```text
boards/<soc>/<board>/
    board.dts        # inventory: addresses, irqs, clocks, rails
    quirks.c         # "needs X before Y" lines, with reasons
    bsp.md           # what is board-specific and why
    expect.txt       # console/storage/net/usb/display pass criteria
    lkl.devices      # device families this board enables
```

## Validation

- Parity ledger: one row per behaviour, Linux reference vs Harvey, evidence.
- Hardware-in-the-loop: `make BOARD=... test` boots and runs `expect.txt`;
  fails loudly.
- Power-cut test for storage: write, verify, cut, remount, verify.
- Error injection: forced timeouts, short transfers, bad blocks.
- Deviation register: each entry with reason, test, isolation flag, owner.

## Open questions

1. Clocks/regulators: native shims or LKL frameworks (see contract).
2. Whether the RK3566 SDHCI can use 64-bit ADMA, which decides the arena
   placement constraint.
3. LKL on aarch64: supported in principle (`arch/lkl` is generic C), not a
   well-tested configuration; M1 validates it.
4. Vendor-tree policy per SoC family (one tree, or one per family).
5. Display: keep native until a KMS bridge to rio is justified.
