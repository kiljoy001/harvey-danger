# Notes for agents working on Harvey

Several agents (and people) work on this tree.  Read this first; keep it
current when you start, finish or abandon a line of work.  Facts here were
true when written: check the code and `git log` before relying on them.

## Where things are

- `README.md`: what Harvey is.  `doc/drivers/`: the driver model, one file
  per subsystem.  `doc/boards/`: per-board notes.
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
- The RK3566 NPU/SCMI quirk now lives in `platform_rk3566.c`, selected
  by `board`; `main.c` calls `rk3566npuprobe(&info, ...)` and nothing else
  in the generic path knows the SCMI/PMU/CRU addresses.
- `#P/platform` exposes the same identity to user space: `board`,
  `compatible`, `ram`, `bootarchive`, `teeshm`, `scm` and `framebuffer`
  as `key value...` lines (see `doc/drivers/platform-bus.md`).
- `util/arm64/pack_boot.py` can stamp those ranges with
  `--bootarchive BASE:SIZE`, `--teeshm BASE:SIZE` and `--scm BASE:SIZE`,
  so an image does not rely on the fallbacks.
- Verified on QEMU virt: the kernel builds with clang-23 (with and
  without `ARM64_RK3566_NPU_PROBE`) and boots; a patched DTB carrying
  `simple-framebuffer` and the three `/chosen` properties parses them and
  prints `fb:`, `bootarchive:`, `teeshm:` in the early banner.
- RK3588 peripheral compatible strings are classified in `fdt.c` and the
  host FDT test covers a real `rockchip,rk3588-evb-v10` root compatible;
  no RK3588 hardware has been booted yet.
- Next: drivers consuming `Fdtdev` base/size/irq instead of constants,
  and replacing `mmu.c`'s fixed Rockchip MMIO list with mappings derived
  from those devices.

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
  and boots them in QEMU virt.  It needs the earlier stages' outputs in
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
