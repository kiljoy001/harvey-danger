# RK3566 NPU

Status: `npufs` powers the block at boot and reads it.  Job submission
(regcmd/task rings) and the IOMMU are the next rungs.

## The rail was the blocker

The NPU hangs off its own buck, RK809 `DCDC_REG4` (`vdd_npu`), and U-Boot
leaves it **disabled and at 500 mV**.  With the rail off, the PMU never
acknowledges the NIU idle release and any access to `0xfde40000` takes the
bus down (in U-Boot the board just resets; in Harvey the fault surfaces
later as an external abort with stale ESR/FAR, which is why it looked like
a page-table problem for a while).

The RK809 enable registers do not take a plain write: bits 7:4 are a write
mask for the enables in bits 3:0.  `i2c mw 0x20 0xb1 0x0f 1` is silently
ignored (reads back 0x07); `0x8f` enables DCDC4 and reads back 0x0f.  That
is the whole trick, and `pmicfs` now sets the mask in `railctl`.

With `vdd_npu` enabled at 900 mV (`0xc4 = 0x20`), `PD_NPU` on and the NPU
clocks open, the NIU idle ack clears and the block answers:

```
VERSION            0x0000  0
VERSION_NUM        0x0004  0
INT_MASK           0x0020  0x8001ffff
PC_DATA_ADDR       0x0010  0x1
PC_DMA_BASE_ADDR   0x0034  0
```

`INT_MASK`'s reset value is the liveness signature; `VERSION` reads zero on
this part (the vendor driver would report the same).

## Bring-up order

1. RK809 `DCDC_REG4`: `set vdd_npu 900`, `on vdd_npu` (via `pmicfs`),
   then wait out the 6 ms ramp.
2. CRU: `CLKSEL_CON(7)` = `0x81cf0000 | (1<<6) | 1` (gpll/2 source),
   `CLKSEL_CON(8)` = `0x00ff0000 | 0x33` (hclk/pclk /4),
   `CLKGATE_CON(3)` = `0x1fff0000` (all NPU gates open),
   `SOFTRST_CON(2)` bits 11/12 = `SRST_A_NPU`/`SRST_H_NPU` (assert, delay,
   release).
3. PMU `PD_NPU`: `PmuPwr 0xa0 = 0x00020000` (hiword mask, clear power bit
   1), poll `0x98` bit 1 clear.
4. PMU `PmuReq 0x50 = 0x00040000` (release idle request bit 2), poll `0x60`
   and `0x68` bit 2 clear.
5. Only then read the block.

Clocks must be up before the idle release: the NPU NIU holds its ack until
its `hclk`/`aclk` run, unlike PD_GPU.

## SCMI clocks

TF-A owns the CPU/GPU/NPU/DDR clocks and exposes them over ARM SCMI on the
SMC transport (`arm,smc-id = <0x82000010>`, shared page at the reserved
`0x10f000`).

* The shared page is `no-map`, so the kernel identity map does not cover
  it; the early probe adds a 4 KiB device mapping.
* The **header layout is the new SCMI one**: id `[7:0]`, type `[9:8]`,
  protocol `[17:10]`, token `[27:18]`.  The old `protocol << 8` layout
  makes a healthy server answer `0xffffffff`.
* `CLOCK_CONFIG_SET` with `CLOCK_ENABLE` enables the clock; status 0.

| id | name | rate at boot |
| --- | --- | --- |
| 0 | `clk_scmi_cpu` | 816 MHz |
| 1 | `clk_scmi_gpu` | 594 MHz |
| 2 | `clk_scmi_npu` | 594 MHz |
| 3 | `clk_scmi_ddr` | 1056 MHz |

The block reads fine without touching SCMI (TF-A leaves the NPU clock on),
but `arm64main`'s probe (`ARM64_RK3566_NPU_PROBE=1`) exercises the protocol.

## The driver

`/boot/npufs` is a 9P service, started by `drvman` after `pmicfs`:

```
/srv/npu/status   model, rail, power, interrupt and PC state
/srv/npu/regs     decoded register window
/srv/npu/ctl      power | off | reset | ident | iommu | iommu-off
                  cna | bcna | test | read <off> | write <off> <v>
```

It maps the NPU window through `#P` (the board DTB must carry the
`rockchip,rk3568-rknpu` node with `status = "okay"`; the older served DTB
did not), asks `pmicfs` for the rail, then runs the sequence above.  The
service is up even when bring-up fails, so `drvman` reports `npu PASS` and
the reason is in `status`.

`regs`/`status` refuse to touch the block while the domain is off: an
access to an unpowered NPU is a bus abort, not a zero.

## PC-mode job submission

`echo test > /srv/npu/ctl` submits one task and works:

```
npufs: test cmd 0xe1001000 task 0xe1000000 amount 4 raw 0x0 status 0x0 pc 0xf000 dcomp 0xdeadbeef
```

* The PC DMA-fetches a `struct rknpu_task` array from `PC_DMA_BASE_ADDR`.
  Each task is 40 bytes (eight u32 then `u64 regcmd_addr`).
* `regcmd_addr` points at the 64-bit regcmd stream; `PC_DATA_ADDR` is the
  first task's stream and `PC_DATA_AMOUNT` is `regcfg_amount + 3`.
  `regcfg_amount` counts the *register* words; the PC also fetches the
  four-word trailer that follows them (next-stream address, register
  amounts, the `0x0041` magic, and the operation-enable broadcast).
* A regcmd word is `target<<48 | value<<16 | reg`, where `reg` is the
  block-relative register offset and `target` is the block id + 1:
  PC `0x101`, CNA `0x201`, CORE `0x801`, DPU `0x1001`; the broadcast
  operation-enable uses target `0x81`.
* `PC_TASK_CONTROL = 6<<12 | ntasks`; `PC_OP_EN` is toggled 1 then 0.
* **Address operands are relative to `PC_DMA_BASE_ADDR`.**  A regcmd that
  writes `x` to an address register reads back `x + task_base` (mod 2^32):
  with the task array at `0xe1010000`, writing `0x2000` reads `0xe1012000`
  and writing `0xffff3000` reads `0xe1003000`.  So the operand to program a
  physical address is `pa - task_base`.  `npufs` has a `reloc()` for this.
* The test stream writes `CNA_DCOMP_REGNUM` (0x1104) with `0xdeadbeef`;
  reading it back proves the task fetch, stream fetch and decode all ran.
  `PC_TASK_STATUS` (0x3c) reads `0xf000` after the task.

**Hazard:** the trailer's last word is normally the broadcast
operation-enable.  With every CNA/DPU address register at zero that starts
the compute blocks and their DMA, which on a live machine can write low
physical memory (one such run corrupted kernel memory and panicked in
`arm64mmubuild`).  The `test` command deliberately replaces it with a
harmless second CNA write.  Before enabling the blocks for real, point the
feature/weight/DPU addresses at a scratch DMA buffer (or enable the IOMMU).

## IOMMU

The NPU's IOMMU (`rockchip,iommu-v2` at `0xfde4b000`) is now granted through
`#P` (the kernel no longer hides `Fdtclassiommu` nodes) and `npufs`
programmes it itself:

1. A 4 KiB directory and one 4 KiB page table come from the DMA arena.
2. `DTE[index]` points at the page table; the task, regcmd and scratch
   buffers are identity-mapped in it (`PTE = pa | 7`).
3. Reset command, `DTEADDR = dt|1`, zap, fault interrupt mask, autogating
   `| 0x80000000`, enable, wait for `STATUS.PAGING`.

`iommu paging`, `status 0x19` (paging + idle + replay) and no faults.  The
PC-mode task keeps working, so the identity mapping matches the physical
addresses.

This is also a safety net: `bcna` (the broadcast operation-enable) starts
every block, and a later run faulted at `0xc2002000` — the IOMMU reported
`raw 0x1 fault 0xc2002000` and `INT_RAW 0x10000` instead of writing to RAM.
Before that, the same mistake corrupted kernel memory and panicked in
`qfreeinternal`.

Two things surfaced from that run:

* Block configuration registers latch while the block is stalled: after a
  fault, direct `write` to `0x1070` is ignored and the readback stays at the
  stale value.  `reset` (soft reset) restores them to zero and writes work
  again.
* The address operands turned out to be task-relative (see above), which
  is what produced the `0xc2002000` read-back; the `conv` command now
  programs every block address through `reloc()` and runs with no IOMMU
  fault at all.

## The 1x1x1 conv

`echo conv > /srv/npu/ctl` configures CNA, CORE, DPU and DPU_RDMA for a
1x1x1 int8 convolution with every address inside the scratch buffer, then
issues the broadcast operation-enable.  With the `reloc()` fix there is no
IOMMU fault, so all of the DMA stays in the buffer.

It still does not complete, but the block state says the broadcast works.
With the `0x7f` operation-enable word (`rocket_ref`'s comment matches the
TRM; its code emits `0x1d`) the register read-back after the job is:

```
CNA op_en 0   CORE 1   DPU 1   DPU_RDMA 1
```

So the CNA ran and cleared its enable, while CORE/DPU/RDMA latched theirs
and stay busy - the chain stalls after the CNA, most likely waiting on
CBUF data (the CNA's 1-channel output has to fill the 32-channel atom the
CORE reads).  `INT_RAW` stays zero because the job only interrupts when the
whole chain finishes.

## Next

1. Feed the CORE what it waits for: the CBUF bank/entry counts and the
   channel alignment are the likely culprits for a 1-channel conv.  Read
   `CORE`/`DPU` state registers after the job and compare with the
   `rocket_ref` fill order.
2. A real operation (conv/depthwise) from the vendor `drivers/rknpu` and
   `9rock/rocket_ref` register tables.
3. Wire the completion interrupt (GIC SPI 151) instead of polling
   `INT_RAW`.
