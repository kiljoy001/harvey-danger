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

### `#P`: DMA chunks, claims as files, regs: branch `pdma`

- Worktree `~/Repo/harvey-pdma`, two commits on top of `vmm-hypervisor`:
  "#P: the DMA arena as chunks each driver holds through a file" and
  "#P: a claim is an open file, and registers are files too".  Not yet
  merged into `vmm-hypervisor`.
- DMA: `#P/dma/clone` gives a chunk `#P/dma/N`, held by its open ctl
  (`alloc <size> [<align>] [cached|uncached]`, `map`,
  `clean|inval|cleaninval <off> <len>`), with `data` to read and write it;
  chunks are zeroed, mapped only into who asks, and freed when the last
  file closes and the last mapping goes.  There is no `dma` segment and no
  `#P/dma/{base,size,alloc,allocowned,free,clean,inval,cleaninval}`.
- Claims: the open `#P/<node>/claim` file is the claim (closing releases;
  children inherit it); only a holder may `segattach` the window, open
  `wait`, or use `regs`.  Exclusive unless written `"<name> shared"`;
  drivers claim clock/reset units, GRFs and GPIO banks shared.  The
  `release` file is gone.  LKL families mark a device two families share
  with a trailing `+` in `util/arm64/lkl-families.json`.
- `#P/<node>/regs`: one aligned 4- or 8-byte register per pread/pwrite,
  done on the kernel's page table; a synchronous abort is reported as
  `regs: bus error`.
- Contract: `doc/drivers/platform-bus.md`.  Verified in QEMU only:
  `util/arm64/test_pnp.py` (claims, regs against the PL031, DMA),
  `test_full_kernel.py`, `test_lkl_boot.py` (768 MiB passes only with
  this branch).  Claims checked on the phone on 2026-11-19 (support
  matrix).  Not run on the Orange Pi; the RK drivers
  (emmc, gmac, sdcard, sdprobe, i2cfs, gpiofs, o9xhci, usbpower, keyroot)
  only compile.
- Next on this line: grant a device's window alone instead of its whole
  2 MiB block (other devices' registers share the block); per-claim IOMMU
  contexts (rkiommu on the RK3566; an ARM SMMU driver for Qualcomm).

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

### PMIC keys, ADC, LPG and vibrator on the Bengal DTS

- The working tree's `util/arm64/lkl-bengal-usb.dts` carries the PMIC
  nodes (not yet committed): a `resin` key (KEY_VOLUMEDOWN), the PM6125
  and PMI632 ADC channels, PMI632 `pbs@7400` and `spmi-sdam@b600`, the
  LPG charge LED and `vibrator@5700`.  Every compatible, ADC `reg` and
  SPMI slave number was checked against 6.12 in `~/Repo/lkl`
  (`qcom,spmi-vadc.h` has 0x01 = ADC5_1P25VREF, 0x4b/0x4c =
  bat_id/xo_therm; PMI632 is slaves 2 and 3, the LPG in 3).
- `util/arm64/lkl-bengal.config` now enables what those nodes need:
  PWM, QCOM_PBS, LEDS_CLASS_MULTICOLOR (drivers/leds/rgb/Kconfig is
  inside that `if`, so LEDS_QCOM_LPG vanishes without it),
  LEDS_QCOM_LPG, INPUT_PM8XXX_VIBRATOR and NVMEM_SPMI_SDAM.
- Verified by build only: `olddefconfig` resolves all six, the five
  drivers compile, and a full `lkl.o` with the firmware lines stripped
  builds and contains every compatible.  Not run on the phone yet.
- A full LKL build in this tree needs `CONFIG_EXTRA_FIRMWARE_DIR`
  (`/tmp/opencode/fw`) populated: `uwe5622/wcnmodem.bin` is in
  `sys/src/cmd/sdio/`, but `uwe5622/wifi_2355b001_1ant.ini` is not in
  the repo (only the header generated from it, `sys/src/cmd/sdio/wifi_conf.h`),
  so the wifi fragment fails without it.

### Bengal phone bring-up

- Uncommitted work on `vmm-hypervisor` (lkl9p, lklusb, the Bengal DTS and
  LKL config, adrenofs).  Status and runbooks are in the support matrix.
- The `adrenofs -t` wedge happens at `segattach` of the whole DMA arena in
  a second process; the `pdma` branch removes that mapping (see above).
  Not yet tried on the phone.  `emmclog` (committed with the bring-up)
  maps the eMMC controller: it and the `bengal_usb` guest cannot both
  claim it.

## Rules for drivers

- Get DMA memory from `#P/dma/clone` (or `drvdma` in `drvkit`), never by
  mapping the arena: on `pdma` and after, there is no `dma` segment.
- Claim a device before mapping it or waiting on it, and keep the claim
  file open (`drvclaim`/`drvmap` in `drvkit` do this).
- Program devices only with physical addresses from your own chunks: the
  kernel isolates chunks from the CPU's side, not yet from devices.

## Building and testing (arm64)

- `python3 util/arm64/test_full_kernel.py` builds the kernel and userland
  and boots them in QEMU virt at 256/512 MiB with GICv2 and at 512 MiB
  with GICv3 with 1 and 8 CPUs.  It needs the earlier stages' outputs in
  `build/` (`util/arm64/build_emu.py` and others); in a fresh worktree,
  copy `build/` from a tree that has them (`cp -a`, not hard links).
- The build needs `../9vm` (for `mksys.py`) and `../purgatorio` (for the
  Dis runner, `util/arm64/build_emu.py`) beside the tree, and
  `util/arm64/build_libc.py` and `build_emu.py` run before the kernel; the
  CI job (`.github/workflows/aspera-quality.yml`) shows the commits it
  pins.  The limbo-generated headers are committed in
  `util/arm64/emu/gen` (regenerate with `gen/mkgen.sh`).
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
