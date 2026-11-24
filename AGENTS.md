# Notes for agents working on Harvey

Several agents (and people) work on this tree.  Read this first; keep it
current when you start, finish or abandon a line of work.  Facts here were
true when written: check the code and `git log` before relying on them.

## Where things are

- `README.md`: what Harvey is.  `doc/drivers/`: the driver model, one file
  per subsystem.  `doc/boards/`: per-board notes; `single-image.md` is
  the one-image runbook (DTB properties, `mk-sdimg.py`, boot script).
- `doc/drivers/platform-bus.md`: `#P`, the only way a user-space driver
  reaches hardware (device tree discovery, MMIO windows, interrupts,
  claims, the DMA arena, the OP-TEE gate).  It is the contract; the code is
  `sys/src/9/aarch64/devpnp.c`.
- `doc/drivers/borneo-support-matrix.md`: the Moto G Power (2021, SM6115
  "Bengal") phone bring-up, with what was observed on the hardware.

## Work in progress

### `#P` DMA arena as chunks: branch `pdma`

- Worktree `~/Repo/harvey-pdma`, commit `2ee6d2a24` "#P: the DMA arena as
  chunks each driver holds through a file".  Not yet merged into the branch
  the phone work is on.
- What it does: `#P/dma/clone` gives a chunk `#P/dma/N`, held by its open
  ctl (`alloc <size> [<align>] [cached|uncached]`, `map`,
  `clean|inval|cleaninval <off> <len>`), with `data` to read and write it.
  The ctl is the capability; chunks are zeroed, mapped only into who asks,
  and freed when the last file closes and the last mapping goes.  The old
  `segattach(0, "dma", ...)` of the whole arena and the
  `#P/dma/{base,size,alloc,allocowned,free,clean,inval,cleaninval}` files
  are gone.  Full contract: `doc/drivers/platform-bus.md`, "DMA".
- Verified in QEMU only: `util/arm64/test_full_kernel.py`,
  `util/arm64/test_pdma.py` (the contract), `util/arm64/test_lkl_boot.py`
  (passes at 768 MiB with this branch; panics "page-table memory exhausted"
  without it, from mapping the whole arena with 4 KiB pages).  Not run on
  the phone or the Orange Pi yet; emmc, gmac, sdprobe, o9xhci, keyroot and
  ethergmac only compile.
- Next on this line: claims as open files and a claim-gated
  `#P/<node>/regs` file (registers through pread/pwrite); EL0 cache
  maintenance; per-device IOMMU contexts.

### DTB-first platform: branch `dtb-platform`

- Worktree `~/Repo/harvey-dtb`; baseline tag `pre-dtb-platform`.
  The branch changes only `sys/src/9/aarch64`; the `vmm-hypervisor`
  worktree stays untouched.
- `Fdtinfo` now carries platform identity and early resources: `board`
  from the root `compatible`, `rootcompat`, the `simple-framebuffer`
  geometry, and the `/chosen` properties `harvey,bootarchive`,
  `harvey,teeshm` and `harvey,scm`.  Lookups are `fdtiscompat()`,
  `fdtdev()` and `fdtboardname()`.
- `main.c` no longer scans the blob for `qcom,bengal` or assumes
  `0x5c000000`, `0x50000000`/`0x49000000`, or `0x09200000`; those remain
  only as documented fallbacks for DTBs that lack the properties.
  `boot.c` selects the RK3566 WDT/PCIe quirk by `board`, taking the WDT
  base from `#P` discovery.
- The RK3566 NPU/SCMI quirk (`rk3566npuprobe`) and the late watchdog,
  PCIe-PHY and GMAC bring-up (`rk3566lateinit`) now live in
  `platform_rk3566.c`, selected by `board`.  `main.c` and `boot.c` keep
  one board-selected call each and know no SCMI/PMU/CRU addresses.
- `#P/platform` exposes the same identity to user space: `board`,
  `model`, `compatible`, `ram` (image bank), `bootarchive`, `teeshm`,
  `scm`, `framebuffer` and one `memory` line per usable bank, as
  `key value...` lines (see `doc/drivers/platform-bus.md`).  Boot walks
  and reads it, requires the model/compatible/ram/memory lines, and
  prints `HARVEY-ARM64 PLATFORM PASS`.
- `util/arm64/pack_boot.py` can stamp those ranges with
  `--bootarchive BASE:SIZE`, `--teeshm BASE:SIZE`, `--scm BASE:SIZE` and
  `--ram BASE:SIZE`, so an image does not rely on the fallbacks.
- `mk-sdimg.py` takes repeated `--dtb [NAME=]PATH`, writes `dtbs/`,
  aliases the default as `board.dtb`, and emits `boot.cmd` (plus
  `boot.scr` when `mkimage` is installed) for multi-board images.  It
  stamps `harvey,bootarchive` from `--sdk-load` and the SDK size,
  accepts `--teeshm/--scm/--ram`, and never modifies the input DTBs.
  `harvey,ram` is the kernel's fallback when the firmware DTB has no
  `/memory`; the real Orange Pi v1.1 DTB is such a file, and the host
  test covers the fallback.
- Verified on QEMU virt: the kernel builds with clang-23 (with and
  without `ARM64_RK3566_NPU_PROBE` and `ARM64_NO_GMAC`) and boots; a
  patched DTB carrying `simple-framebuffer` and the three `/chosen`
  properties parses them and prints `fb:`, `bootarchive:`, `teeshm:` in
  the early banner.
- Board classes are `qemu-virt`, `rk3566`, `rk3568`, `rk3588`,
  `bengal`, `sun50i-h6` and `unknown`.  RK3588 peripherals and a real
  `rockchip,rk3588-evb-v10` root compatible are covered by the host FDT
  test; no RK3588 hardware has been booted yet.
- `mmu.c` no longer has a fixed Rockchip MMIO list.  It maps the
  DT-discovered cru/grf/vop/hdmi/iommu/wdt/gmac/usb/gpio windows; the
  board-gated RK3566 NPU probe windows are the only non-DT mappings.
- RAM is multi-bank: `fdtselectram` keeps every usable bank (image bank
  first, 2 MiB aligned) and `phys.c`/`mmu.c` manage and map all of them.
  `ARM64_TEST_MULTIBANK=1` splits QEMU's first bank into a low half and a
  4 GiB half; with `-m 4G` the banner reports `banks 2` and the free-page
  count proves the high bank is in the allocator.
- GICv3 per-CPU setup finds the redistributor by scanning `GICR_TYPER`
  affinity values, not by a 4-CPU-per-affinity index.  QEMU virt with 8
  CPUs now reports every CPU scheduler/timer and `IPI PASS`; the GICR
  size comes from the DT, so the scan stays inside the mapped window.
- The non-Bengal DMA arena is half the largest bank, capped at 512 MiB
  and floored at 32 MiB, so `#P/dma` exists on 256/512 MiB boards.  Boot
  walks `#P/dma/base` and `#P/dma/size`, checks both are non-zero, and
  prints `HARVEY-ARM64 DMA-FS PASS`.
- `devxhci.c` and `ethergmac.c` resolve their bases with
  `fdtdevcompat()`, `fdtdevcompatn()` and `fdtdevname()`; the old
  addresses remain as fallbacks for a DTB that lacks the nodes.  Both
  only compile; neither has run on the Orange Pi yet.
- QEMU virt with a synthetic DTB carrying DWC3/EHCI/OHCI/GPIO/GMAC
  nodes passes `MMU-ON`, so the DT-derived mappings are exercised.
- Next: run `devxhci`/`ethergmac` on the Orange Pi, then remove the
  per-driver fallback constants once the DT nodes are confirmed.

### Bengal phone bring-up

- Uncommitted work on `vmm-hypervisor` (lkl9p, lklusb, the Bengal DTS and
  LKL config, adrenofs).  Status and runbooks are in the support matrix.
- The `adrenofs -t` wedge happens at `segattach` of the whole DMA arena in
  a second process; the `pdma` branch removes that mapping (see above).
  Not yet tried on the phone.

## Rules for drivers

- Get DMA memory from `#P/dma/clone` (or `drvdma` in `drvkit`), never by
  mapping the arena: on `pdma` and after, there is no `dma` segment.
- Program devices only with physical addresses from your own chunks: the
  kernel isolates chunks from the CPU's side, not yet from devices.

## Building and testing (arm64)

- `python3 util/arm64/test_full_kernel.py` builds the kernel and userland
  and boots them in QEMU virt at 256/512 MiB with GICv2 and at 512 MiB
  with GICv3 with 1 and 8 CPUs.  It needs the earlier stages' outputs in
  `build/` (`util/arm64/build_emu.py` and others); in a fresh worktree,
  copy `build/` from a tree that has them (`cp -a`, not hard links).
- The committed `util/arm64/build_userland.py` compiles
  `sys/src/cmd/emmclog/emmclog.c`, which is not committed yet: a clean
  checkout needs that directory before it builds.
- LKL: `python3 util/arm64/build_lkl.py --lkl
  /tmp/opencode/lkl-install/lib/lkl.o --include
  /tmp/opencode/lkl-install/include`, then
  `util/arm64/test_lkl_boot.py --kernel build/arm64-kernel/harvey.img
  --elf build/arm64-lkl/lklboot`.
- QEMU tests that type at the shell must wait until after
  `HARVEY-ARM64 RC START` (init starts an LKL guest first; about 50 s).
- `build/arm64/test-fdt <dtb> inspect` prints the board class, early
  ranges and every device the kernel would discover.  A U-Boot control
  DTB may lack `/memory`; add one with `fdtput` before inspecting.  The
  Orange Pi 3B v1.1 DTB (with a synthetic 2 GiB `/memory`) reports
  `board rk3566` and 46 classified devices.
- `python3 util/arm64/test_sdimg.py` builds a two-board SD image,
  extracts the FAT contents, and checks `dtbs/`, `board.dtb`, `boot.cmd`,
  the stamped `/chosen` ranges and that the input DTBs are untouched.
- `python3 util/arm64/test_packboot.py` builds an Android boot image,
  checks the header sizes, the stamped `/chosen` ranges and the
  `harvey,lkl-arena-size` value.
- Multi-bank RAM: build with `ARM64_TEST_MULTIBANK=1` and boot QEMU virt
  with `-m 4G`; the banner must show `banks 2`, `total 0x80000000`, and
  `ARM64 multibank:` free pages well above 262144 (the first bank alone).
- A Plan 9 wait message is `"<prog> <pid>: <status>"`: match the status
  with `strstr`, not `strcmp`.

## The phone (borneo) on the bench

- RAM-boot only (`fastboot boot`); never flash.  Recovery from a wedged
  console is a long power-button hold back to stock Android.
- Console: `/dev/serial/by-id/usb-Harvey_Harvey_ARM64_USB_harvey01-if00`.
  Do not background a blocking read on it (it wedges rc).  In the current
  uncommitted `lklusb.c`, any `~` typed on the console triggers a 100-line
  test burst and drops the input.
- The USB mass-storage export of the eMMC in `lklusb.c` cannot start as
  written: it checks for the eMMC in the `bengal_ctl` guest, but the eMMC
  is in `bengal_usb` (`util/arm64/lkl-families.json`).
