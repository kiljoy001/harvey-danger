# Orange Pi 3B driver gaps

Audit date: 2026-11-01. Board: Orange Pi 3B v1.1 (RK3566).
Branch: `opi3-driver-audit`.

This is the working gap list behind
[`support-matrix.md`](support-matrix.md). It distinguishes "no driver" from
"driver exists but a board-level dependency is missing", because the second
case is often the shorter path.

## What already works

These are not gaps; they are the base to build on.

- UART console, `gpiofs`, `i2cfs`, `pmicfs`.
- RK809 regulators, RTC/alarm, CLKOUT2, power key (polled), thermal reporting.
- eMMC: SDHCI/dwcmshc, 50 MHz 8-bit, HS200 200 MHz, ADMA2, `/srv/emmc`,
  `emmcfs` with a 9P block interface and a boot-time read/write self-test.
- GMAC at 10 Mbps with `gmacfs`; the MAC/PHY register path is documented.
- xHCI root-port enumeration, control/bulk/interrupt transfers, UAC1 probe.
- VOP2 + DW-HDMI scanout at a fixed 1920x1080 RGB888.
- RK IOMMU two-level tables, used by VOP2.
- NPU register/task format decoded in `9rock`; vendor driver exists.

## Missing or unfinished

Priority is for a usable OPi3 product, not for completeness.

| Area | Current state | What is missing | Priority | Next step |
| --- | --- | --- | --- | --- |
| PD_USB power domain | PD_PIPE is powered in `devxhci.c`; PD_USB is not | GMAC1 and parts of USB have no bus clock after U-Boot handoff; `fe010000` reads zero | **P0** | Add a PMU power-domain helper and power `PD_USB` before `ethergmacinit()`/`dwc3init()` |
| Root filesystem | `emmcfs` exposes a raw 9P block device; init only self-tests it | No partition layer, no on-disk filesystem, no root mount; boot archive lives in RAM | **P0** | Mount `/srv/emmc` as a root or `/n/emmc`, add a filesystem and boot from it |
| USB HID | xHCI control/bulk/interrupt works; no class driver | Keyboard/mouse; hubs; safe endpoint cancellation for idle interrupt reads | **P0** | User-space `#P` transport + HID class service; fix pending-read/cancel semantics |
| USB MSC | xHCI bulk works | Mass-storage class service; no USB root/removable storage | P1 | MSC bulk-only transport over the same L2 engine |
| Ethernet 100/1000 | 10 Mbps works; 100/1000 register path open, v1.1 PHY workaround documented | PD_USB, PHY delay sweep, tested 100M/1G | P1 | Power PD_USB, then sweep MAC/PHY delays and measure |
| Thermal / TSADC | PMIC reports temperature; no SoC TSADC driver | SoC temperature, trip points, warning/shutdown | P1 | TSADC driver + PMIC hot-die policy |
| Watchdog | none | RK WDT; reset-on-hang for unattended systems | P1 | Small WDT driver/service |
| TRNG | none | RK RNG hardware source | P1 | Small `/dev/random` or `/srv/trng` backend |
| TPM 2.0 add-on | `tpmfs` exists; CRB/TIS userspace code exists | RK SPI controller driver + TIS transport on ARM; no discrete TPM on board | P1/P2 | SPI driver, then TIS; ties into the root-of-trust work |
| Display modes | fixed 1920x1080 RGB888 | EDID read, modeset, hotplug, HDMI audio | P2 | DDC/EDID + mode table; keep fixed mode as fallback |
| Audio | UAC1 probe reads isochronous frames | UAC1/UAC2 service; RK809 codec + I2S; HDMI audio | P2 | PCM 9P service first, then on-board codec |
| PCIe / NVMe | FDT class recognized; no driver | DWC PCIe + combphy; NVMe | P2 | Only if eMMC/USB storage is insufficient |
| SD card slot | eMMC works; SD slot not driven | SDHCI instance for `sdmmc0`; card detect/power | P2 | Parameterize the eMMC SDHCI core for the SD slot |
| SPI NOR | FDT class recognized | RK SFC; boot flash read | P2 | Needed for firmware/env inspection, not rootfs |
| SARADC | FDT class recognized | ADC channels (keys, sensors) | P3 | Small driver if a board function needs it |
| PWM / fan | FDT class recognized | Fan/backlight/IR carrier | P3 | Pair with thermal policy |
| Crypto engine | none | AES/SHA offload | P3 | Only if CPU crypto is insufficient |
| eFuse / OTP | read via U-Boot/Linux only | Harvey efuse driver; unique device identity | P3 | Lot code is not a serial; do not treat it as one |
| VPU / VEPU | FDT class recognized | H.264/H.265 decode/encode | P3 | Product-dependent |
| RGA | none | 2D blit/scale | P3 | Product-dependent |
| GPU | FDT class recognized | Mali G52 | P3 | Product-dependent |
| ISP / camera | FDT class recognized | rkisp + vicap, MIPI CSI | P3 | Product-dependent |
| NPU | vendor driver in `9rock`; no Harvey driver | rknpu service/queue | P3 | Research path, not a product blocker |
| WiFi / BT | CP boots, scans, WPA2 associates; data path weak | Link budget, not framing; v1.1 CDW-20U5622 vs v2.1 AP6256 | P3 | Retest next to the AP before writing more code |

## The three short paths

### 1. PD_USB first

`doc/drivers/gmac-bringup.md` records the important fact: after U-Boot,
`fe010000` reads zero and ignores writes while the GMAC clocks are parented
to `aclk_usb`/`pclk_usb` in **PD_USB**. `devxhci.c` powers **PD_PIPE**, but
not PD_USB. This one PMU change should improve both GMAC and USB, and it is
much smaller than a new Ethernet driver.

Evidence to collect after the change:

```text
gmac: phy ... link 1 speed 100/1000
/net/ether0 exists
ping 192.168.2.1
```

### 2. Root filesystem, not another block driver

`emmcfs` already serves `/srv/emmc` at HS200 with ADMA2 and has a boot-time
self-test. The missing layer is above it: partition discovery, a filesystem,
and a root mount. Until that exists the system runs from the RAM boot archive,
so every reboot loses state and the image size is bounded by the boot archive.

The SD slot is a convenience, not a prerequisite: eMMC is soldered and already
works. Fix the root path before adding a second SDHCI instance.

### 3. USB input before USB storage

The OPi3 has no PS/2 keyboard. `platform.c` reports that ARM64 scan-code input
is unavailable, and xHCI has no HID class. A desktop is not usable until a USB
keyboard/mouse works, so HID is ahead of MSC.

The blocker is not enumeration; it is the transport lifetime. `o9xhci`'s
timeout path poisons the controller, which is wrong for an idle interrupt-IN
read. The first-class work is:

- a user-space `#P` MMIO/DMA transport (the kernel `#X` device was removed);
- HID report descriptor parsing and interrupt-IN reads;
- safe pending-read cancellation and endpoint reset.

## Board revision note

The v1.1 board under test has the 3.3 V Ethernet PHY, the CDW-20U5622 WiFi/BT
module, and a forced 100M Ethernet setup in the mainline DTS. The v2.1 board
has a 1.8 V PHY, AP6256/BCM43456, and Gigabit. Do not generalize a v1.1 PHY
result to v2.1 without checking the rail and the DTS.

## References

- [`support-matrix.md`](support-matrix.md) — canonical status table.
- [`gmac-bringup.md`](gmac-bringup.md) — PD_USB handoff finding.
- [`emmc-bringup.md`](emmc-bringup.md) — SDHCI/HS200/ADMA2 and `emmcfs`.
- [`pmic-bringup.md`](pmic-bringup.md) — RK809, RTC, power key, thermal gaps.
- [`wifi-bringup.md`](wifi-bringup.md) — UWE5622 CP, association, link budget.
- [`../boards/orangepi-3b/`](../boards/orangepi-3b/) — DTS, schematic, USB map.
- [`../../sys/src/cmd/o9xhci/README.md`](../../sys/src/cmd/o9xhci/README.md) — xHCI scope and class gaps.
- [`../../sys/src/cmd/tpm/README.md`](../../sys/src/cmd/tpm/README.md) — TPM 2.0 userspace.
- [`../../../9rock/OPTEE_RK3566_FINDINGS.md`](../../../9rock/OPTEE_RK3566_FINDINGS.md) — RK3566 TEE/OTP findings.
