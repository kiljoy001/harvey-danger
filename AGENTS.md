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
  this branch).  Not run on the phone or the Orange Pi; the RK drivers
  (emmc, gmac, sdcard, sdprobe, i2cfs, gpiofs, o9xhci, usbpower, keyroot)
  only compile.
- Next on this line: grant a device's window alone instead of its whole
  2 MiB block (other devices' registers share the block); per-claim IOMMU
  contexts (rkiommu on the RK3566; an ARM SMMU driver for Qualcomm).

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
  and boots them in QEMU virt.  It needs the earlier stages' outputs in
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
