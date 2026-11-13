# RK3566 NPU / SCMI clocks

Status: the NPU clock is now under our control through TF-A's SCMI server.
The NPU register block itself still aborts on the first read, so there is no
driver yet.

## SCMI over SMC

The RK3566's TF-A owns the CPU/GPU/NPU/DDR clocks (DVFS and some gates live
in the secure world), and exposes them with the ARM SCMI clock protocol on
the SMC transport.  The vendor device tree describes it:

```
firmware/scmi: arm,scmi-smc, arm,smc-id = <0x82000010>,
                shmem = <0x10f000, 0x100>  (reserved, no-map)
protocol@14:   SCMI clock protocol
```

Everything needed to talk to it:

* SMC function id `0x82000010`; the shared page is the reserved 0x10f000.
  The page is `no-map`, so the kernel identity map does not cover it; the
  NPU probe adds a 4 KiB device mapping for it.
* Shared memory layout (0x10f000): `reserved`, `status` (1 = free, 2 =
  error), 2 reserved words, `flags`, `length`, `header`, payload.
* **Header layout is the new SCMI one**: message id `[7:0]`, type `[9:8]`,
  protocol `[17:10]`, token `[27:18]`.  Using the old `protocol << 8` layout
  makes every call return `0xffffffff` from a perfectly working server.
* `length` is the header plus the request payload; the reply overwrites it
  (header + status + data) and the reply status is the first payload word.

## Clock protocol (0x14)

| id | name | rate at boot |
| --- | --- | --- |
| 0 | `clk_scmi_cpu` | 816 MHz |
| 1 | `clk_scmi_gpu` | 594 MHz |
| 2 | `clk_scmi_npu` | 594 MHz |
| 3 | `clk_scmi_ddr` | 1056 MHz |

Commands used: `CLOCK_ATTRIBUTES` (3, id → attributes + 16-byte name),
`CLOCK_RATE_GET` (6), `CLOCK_CONFIG_SET` (7, `{id, CLOCK_ENABLE}`).
`arm64main`'s NPU probe (build with `ARM64_RK3566_NPU_PROBE=1`) prints all
four and enables the NPU clock; it returns status 0.

## Native NPU setup (what is known to be right)

* Power: RK3568 PMU, `PD_NPU` (`pwr_offset 0xa0`, `status 0x98`,
  `req 0x50`; pwr bit 1, req bit 2).  Idle-request, power on, release.
  Status reads 1 (on) with the inverted power-bit convention.
* Rail: RK809 `DCDC_REG4` (`vdd_npu`) is a separate buck; U-Boot leaves it
  at 500 mV (reg 0xc4 = 0).  Linux's `regulator-init-microvolt` sets
  900 mV (`0xc4 = 0x20`, 12.5 mV steps from 500 mV).
* CRU: `CLKSEL_CON(7)` = `0x81cf0000 | gpll | div` (clk_npu_src),
  `CLKSEL_CON(8)` hclk/pclk dividers, `CLKGATE_CON(3)` bits 0-12 ungated,
  `SOFTRST_CON(2)` bits 11/12 = `SRST_A_NPU`/`SRST_H_NPU`.
* With all of that plus the SCMI NPU clock enabled, reading 0xfde40000 or
  0xfde4b000 still takes a level-0 translation abort.  The NPU NIU / IOMMU
  window is the open question.

## Next

1. Work out why the NPU/IOMMU windows do not answer (NIU clock, PD_PIPE,
   or an IOMMU bypass).  The vendor `drivers/rknpu` source in 9rock is the
   reference.
2. Once the block answers, a 9P service (`npufs`) with `version`, `regcmd`
   and `run` files mirrors the gpu/usb split: native clock/power, textual
   register window.
3. The alternative that skips the register protocol is the vendor rknpu
   driver under LKL, with Harvey owning SCMI and the rails.
