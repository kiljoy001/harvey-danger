# Orange Pi 3B hardware and driver gaps

Audit date: 2026-11-01. Board: Orange Pi 3B v1.1 (RK3566).
Branch: `opi3-driver-audit`.

This is the full-board inventory behind
[`support-matrix.md`](support-matrix.md). It separates three different
problems that often get conflated:

1. **No driver** for a SoC block or board device.
2. **Driver exists**, but a board dependency (power domain, clock, pinmux,
   DTS node) is missing.
3. **Hardware is present**, but the mainline board DTS does not enable it.

The third case is common on this board: the schematic has eDP, MIPI DSI,
MIPI CSI, fan, IR, SARADC recovery key, headphone detect, and RTC battery
signals that the board DTS does not describe.

USB class services are moving to LKL (`lklusb`, see `lkl.md`); the USB
rows below describe the native `o9xhci` fallback and its parity-oracle role.

## What already works

- UART console on UART2 at 1500000 8N1.
- `gpiofs`, `i2cfs`, `pmicfs`.
- RK809 regulators, RTC/alarm, CLKOUT2, power key (polled), thermal reporting.
- eMMC: SDHCI/dwcmshc, 50 MHz 8-bit, HS200 200 MHz, ADMA2, `/srv/emmc`,
  `emmcfs` with a 9P block interface and boot-time read/write self-test.
- GMAC at 10 Mbps with `gmacfs`; MAC/PHY register path documented.
- xHCI root-port enumeration, control/bulk/interrupt transfers, UAC1 probe.
- VOP2 + DW-HDMI scanout at a fixed 1920x1080 RGB888.
- RK IOMMU two-level tables, used by VOP2.
- NPU register/task format decoded in `9rock`; vendor driver exists.
- TRNG: `rngfs` (Crypto V2) exposes `/srv/rng/{random,status,ctl}`.
- Watchdog: `wdtfs` exposes `/srv/wdt/{status,ctl}`; status/ping verified on v1.1.
- SARADC: `saradcfs` exposes `/srv/saradc/chan0..7` and `/status`; conversions and vref control verified on v1.1.
- 9P write note: control writes can arrive in several messages; `saradcfs` and `wdtfs` buffer until newline before applying a command.

## SoC and controller blocks

| Block | Current state | Missing / next | Priority |
| --- | --- | --- | --- |
| UART0-9 | UART2 console works; `uartfs` exists | UART1 BT HCI, UART0/3-9 on header/debug, general UART service | P1/P2 |
| GPIO / pinctrl | `gpiofs`; banks GPIO0-4, per-pin IRQ | 40-pin header policy, more pinmux, LED/IR consumers | P1 |
| I2C0-5 | `i2cfs`; I2C0 = PMIC, I2C1 header | Device drivers on I2C1-5 (sensors, display DDC, camera) | P1 |
| SPI0-3 | FDT class recognized | No SPI controller driver; header and TPM need it | P1 |
| PWM0-15 | FDT class recognized | No PWM driver; eDP backlight, fan, IR, header | P1 |
| SARADC | `saradcfs` serves `/srv/saradc/chan0..7` (10-bit); board test: chan0 1023/1798 mV, chan1 951/1671 mV, vref ctl 3.3 V -> 3296 mV | Add a key/header consumer | P2 |
| TSADC / thermal | FDT class recognized; PMIC reports temperature | No SoC TSADC, trip points, thermal zones | P1 |
| Watchdog | `wdtfs` reports CR/TORR/CCVR and accepts disable/ping/enable; board status and ping tested, ctl now buffers split writes | Enable/disable policy and timeout calibration | P1 |
| TRNG | `rngfs` serves `/srv/rng`; board test generated 425,984 bytes with 0 errors | Clock/reset handoff is fine on v1.1; add a selftest/entropy consumer | — |
| Crypto engine | SoC block | No AES/SHA offload driver | P3 |
| eFuse / OTP | Read via U-Boot/Linux only | No Harvey efuse driver; lot code is not a serial | P3 |
| PMU power domains | PD_PIPE powered before GMAC, PD_NPU probed | Other domain policy absent; GMAC reset now released in the driver | P1 |
| CRU / GRF / PMUGRF | Substrate; clocks for UART/eMMC/VOP/USB/GMAC | More clock/reset coverage, DVFS coupling | P1 |
| GIC / timer / MMU / DMA | Working substrate | Not drivers; keep out of the driver list | — |
| IOMMU | VOP2 working | Other masters (VPU, VEPU, ISP, RGA, NPU) | P2 |
| eMMC | HS200/ADMA2; MBR partitions; 128 MiB root + 200 GiB data at /mnt/data | fsck policy, unattended boot | P1 |
| SD card (sdmmc0) | DTS enabled | No driver; second SDHCI instance | P2 |
| SDIO (sdmmc1) | DTS enabled | No SDIO driver; WiFi/BT module | P2 |
| GMAC1 / PHY | 10/100 Mbps link, `/net` up, ping passes | 1 Gbps: TX DMA underflow, no traffic (v1.1) | P2 |
| USB2 PHY0/1 + EHCI/OHCI | Board probe | No class drivers; OTG role | P1 |
| USB3 xHCI | Root ports, control/bulk/interrupt, UAC1 probe | Hubs, HID, MSC, audio service, `#P` transport, pending-read cancellation | **P0** |
| PCIe2x1 / combphy | FDT class recognized | No PCIe driver; M.2 NVMe | P2 |
| VOP2 / HDMI | Fixed 1920x1080 RGB888 | EDID/DDC, modeset, hotplug, CEC, HDMI audio | P2 |
| MIPI DSI0/1 | DTS nodes exist; board DSI not enabled | No DSI driver/panel; DSI PHY | P2 |
| eDP | Schematic has EDP_TX, BL_PWM/EN, HPD | No eDP node/driver, backlight, HPD | P2 |
| HDMI audio | `hdmi_sound` enabled in DTS | No audio service/driver | P2 |
| I2S0/1/2/3, SPDIF, PDM | I2S1 + RK809 sound card in DTS | No I2S/PDM/SPDIF driver or PCM service | P2 |
| RK809 codec | Regulators/RTC work | No codec driver; analog out/in, HP detect | P2 |
| GPU (Mali G52) | DTS enabled; `gpufs` powers PD_GPU, releases resets and reads G52 feature registers | No GPU MMU, job queues, shader power or graphics stack | P3 |
| NPU | Vendor driver/`9rock` decode | No Harvey driver/queue | P3 |
| VPU (decode) | FDT class recognized | No driver | P3 |
| VEPU (encode) | FDT class recognized | No driver | P3 |
| RGA (2D) | FDT class recognized | No driver | P3 |
| ISP / VICAP | FDT class recognized | No driver | P3 |
| MIPI CSI / DPHY | FDT class recognized | No camera/CSI driver | P3 |
| SFC / SPI NOR | FDT class recognized; boot flash present | No SFC driver | P2 |
| SATA | Shared PCIe/USB3 lanes | No SATA connector exposed; not applicable | — |
| Mailbox / SCMI | Firmware interface | No driver; only if firmware services are needed | P3 |
| TEE / OP-TEE | Vendor BL32, keyroot oracle | No general TEE driver; vendor TEE is unauditable | P2 |

## Board hardware and connectors

| Board device | Evidence | Current state | Missing / next | Priority |
| --- | --- | --- | --- | --- |
| HDMI Type-A | `hdmi-con`, VOP2/HDMI | Fixed 1080p scanout | EDID/DDC, modeset, HPD, CEC, audio | P2 |
| eDP output | Schematic: EDP_TX, EDP_BL_PWM/EN, EDP_HPD | No DTS node/driver | eDP controller, backlight PWM, HPD | P2 |
| MIPI DSI | Schematic: MIPI_DSI_TX1; DSI0/1 nodes | Not enabled on board | DSI panel/connector, DSI PHY | P2 |
| MIPI CSI camera | Schematic: MIPI_CSI_RX, CAMERA_PDN_L | Not enabled | CSI/DPHY, ISP, camera power | P3 |
| 3.5 mm earphone/mic | `EARPHONE`, `MIC1_IN`, `HP_DET_L` | No audio service | RK809 codec, HP detect, mic path | P2 |
| Speaker out | `SPK_OUTp/n` | No audio service | RK809 speaker path | P2 |
| Fan connector | `FAN1`, 5 V 500 mA | No driver/thermal policy | PWM/tach or always-on policy | P1 |
| IR receiver | `GPIO0_C6-PWM7_IR` | No input driver | IR decode or GPIO event | P2 |
| Recovery key | `SARADC_VIN0_KEY/RECOVERY` | No SARADC/key driver | ADC key event | P2 |
| Power key | `PMIC_PWRON`, RK809 IRQ | Polled by `pmicfs` | Wire GPIO0_A3 IRQ; event service | P1 |
| T2Key / reset | `T2Key`, `PCIE20_BUTTONRSTn` | Not modeled | Board policy if used | P3 |
| RTC battery | `VCC_RTC`, SX-3215 | RK809 RTC works | Battery status/monitoring | P3 |
| Work LED | GPIO0_C0, `work_led` | `gpiofs` can drive | Heartbeat/status service | P3 |
| Ethernet LEDs | RJ45 PHY LEDs | Hardware default | Optional LED policy | P3 |
| 40-pin header (CON2) | 2x20 header | GPIO/I2C/UART partial | SPI, PWM, SARADC; header pin policy | P1 |
| M.2 M-key | `M.2-KEY-M-H3R2`, PCIe2x1 | No driver | PCIe + NVMe | P2 |
| TF card slot | `TF2`, sdmmc0 | No driver | SDHCI instance, card detect | P2 |
| eMMC | `sdhci`, HS200 | Works; boots with `/` on the ext2 root partition | fsck, larger partitions | P1 |
| SPI NOR | `sfc`, `jedec,spi-nor` | No driver | SFC read/write | P2 |
| USB3 host | USB3_HOST1, xHCI | Root ports work | HID/MSC/hubs, `#P` transport | **P0** |
| USB2 host x2 | USB2_HOST2/3, EHCI/OHCI | Board probe | HID/MSC classes | P1 |
| USB2 OTG / Type-C | `USB_OTG0`, `TYPEC1` power | PHY/regulator only | OTG role, CC/PD policy | P2 |
| Ethernet RJ45 | GMAC1 + PHY | 10/100 Mbps, `/net` up, ping passes | 1 Gbps underflow on v1.1 | P2 |
| WiFi/BT | SDIO1 + UART1, wake GPIOs | WiFi CP/scan/WPA2; BT not driven | BT HCI, WiFi data path/link budget | P2/P3 |
| Debug UART | UART2, 1500000 | Works | — | — |
| Power input | DC 5 V / Type-C power | RK809 rails | No PD/CC policy | P3 |

## Power, thermal, performance, security

| Area | Current state | Missing / next | Priority |
| --- | --- | --- | --- |
| RK809 regulators | Rails readable/settable via `pmicfs` | Dynamic voltage for DVFS; full rail policy | P1 |
| SYR827 CPU regulator | I2C0 device | No regulator/cpufreq coupling | P1 |
| CPU OPP / DVFS | OPP tables in DTS | No cpufreq driver; fixed frequency | P1 |
| GPU/NPU DVFS | OPP tables in DTS | No devfreq | P2 |
| Power domains | PD_PIPE, PD_NPU probe | PD_USB, PD_VPU, PD_RGA, PD_ISP, PD_NPU policy | **P0** |
| Thermal zones | DTS has GPU/SoC trips | No TSADC driver or cooling policy | P1 |
| Fan control | FAN1 hardware | No PWM/tach/thermal loop | P1 |
| Watchdog | WDT node | No driver/reset policy | P1 |
| TRNG | RNG node | No driver | P1 |
| Crypto offload | SoC crypto | No driver | P3 |
| OTP / eFuse | Read via U-Boot/Linux | No driver; not a unique serial | P3 |
| TEE / secure boot | Vendor BL32 + keyroot oracle | No general TEE driver; no fused secure boot | P2 |
| SCMI / mailbox | Firmware | No driver; needed only for firmware services | P3 |

## The three short paths

### 1. GMAC release: solved

`doc/drivers/gmac-bringup.md` records the resolution. The bootloader leaves
`SRST_A_GMAC1` (reset id `0xec`, `SOFTRST_CON(14)` bit 12) **asserted**, so
`fe010000` reads zero and MDIO returns no PHY. Rockchip soft-reset registers
are hiword-masked, so the deassert is a single masked write of `1<<28`.

Two rules came out of the fix:

- Deassert the reset only. Do **not** open `CLKGATE_CON(17)`: the bootloader
  leaves every GMAC clock gate closed and the MAC still answers MDIO, while
  enabling those gates wedges the register block.
- Zero every `Rendez` before `tsleep()`. `gmacreset()` and `gmacproc()` used
  uninitialised stack rendezvous, which only became visible once MDIO got past
  the old failure point: `sleep()` locked on stack garbage and the kernel
  reported a lock loop in `qmalloc`.

A cold boot now reports `gmac: ver 3051 ... phy 0 status 7c40`,
`#l0: gmac: 100Mbps`, and `NET-BIND`, `NET-IFC`, `NET-PING` all pass.
The probe advertises 10/100 and runs at whatever autonegotiation picks;
`/net/ether0/0/ctl` can force a speed at runtime.  1 Gbps still links but
transmits nothing (TX DMA underflow), see `gmac-bringup.md`.

### 2. Root filesystem, not another block driver

`emmcfs` serves `/srv/emmc` at HS200 with ADMA2, parses the DOS partition
table and exposes `/partN`.  `ext2srv` is built for ARM64 and init mounts the
ext2 root partition at `/mnt/emmcroot` during boot, so the filesystem stack is
complete, and init re-roots onto it: it mounts `/srv/ext2` on `/`, aliases
`/boot` to `/bin` and rebinds `/dev`, `/env`, `/srv`, `/mnt` and `/proc`.
A file written before a power cycle is still there afterwards.

Both filesystems are writable: a 128 MiB system image and a 200 GiB
data partition mounted at `/mnt/data`, with `/tmp` and `/mnt` on the
kernel RAM filesystem for scratch.  See [`filesystem.md`](filesystem.md)
for how the data filesystem is created without transferring 200 GiB.

What is left is unattended boot (the kernel and archive still arrive
over TFTP) and an fsck story for the data partition.

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

## Reset behaviour under sustained eMMC I/O

The board sometimes resets (full DDR retrain, not a kernel panic) while
writing large runs to the eMMC, and the same has been seen while
copying a large file out of the ext2 data partition.  Reads of the
200 GiB partition are the least reliable operation.  The Rockchip
preloader and ATF come back cleanly, so this looks like a supply or
watchdog event rather than memory corruption; it is still open.

Two operational consequences:

- Write the root filesystem in chunks (`mmc write` of 32 MiB at a time)
  rather than one 128 MiB write.
- The bench USB switch does not cut the board's power, so it cannot be
  used as a remote power cycle.  From a running system, a real reset is
  `echo reboot > /dev/reboot` (PSCI SYSTEM_RESET), provided the caller
  passes the `iseve` check.

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
- [`../boards/orangepi-3b/dts/rk3566-orangepi-3b.dtsi`](../boards/orangepi-3b/dts/rk3566-orangepi-3b.dtsi) — board nodes.
- [`../boards/orangepi-3b/refs/orangepi-3b-v2.1-schematic.txt`](../boards/orangepi-3b/refs/orangepi-3b-v2.1-schematic.txt) — schematic extraction.
- [`../../sys/src/cmd/o9xhci/README.md`](../../sys/src/cmd/o9xhci/README.md) — xHCI scope and class gaps.
- [`../../sys/src/cmd/tpm/README.md`](../../sys/src/cmd/tpm/README.md) — TPM 2.0 userspace.
- [`../../../9rock/OPTEE_RK3566_FINDINGS.md`](../../../9rock/OPTEE_RK3566_FINDINGS.md) — RK3566 TEE/OTP findings.
