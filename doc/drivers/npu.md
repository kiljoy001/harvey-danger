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
/srv/npu/ctl      power | off | reset | ident | read <off> | write <off> <v>
```

It maps the NPU window through `#P` (the board DTB must carry the
`rockchip,rk3568-rknpu` node with `status = "okay"`; the older served DTB
did not), asks `pmicfs` for the rail, then runs the sequence above.  The
service is up even when bring-up fails, so `drvman` reports `npu PASS` and
the reason is in `status`.

`regs`/`status` refuse to touch the block while the domain is off: an
access to an unpowered NPU is a bus abort, not a zero.

## Next

1. **Job submission.**  The PC block DMA-fetches a `struct rknpu_task` array
   (`PC_DMA_BASE_ADDR`), each task naming a 64-bit regcmd stream
   (`PC_DATA_ADDR`, `PC_DATA_AMOUNT`), with `PC_TASK_CONTROL = 6<<12 |
   ntasks` and `PC_OP_EN` toggled 1→0.  The regcmd encoding is
   `target<<48 | value<<16 | reg` (see `9rock/rocket_ref/rkt_regcmd.c` and
   the vendor `drivers/rknpu`).
2. **IOMMU.**  `0xfde4b000` answers (`0x04 = 0x18`).  The kernel currently
   drops `Fdtclassiommu` nodes from `#P`, so either expose the NPU IOMMU to
   `npufs` or use physical DMA first (`rk356x_rknpu_config.dma_mask` is
   32-bit and PC mode works without the IOMMU).
3. Keep the vendor `drivers/rknpu` (9rock) as the reference oracle for the
   register/task format.
