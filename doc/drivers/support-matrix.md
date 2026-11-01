# Driver support matrix

Status values:

```text
supported     tested on hardware, documented, expected to work
partial       works for the listed scope; limitations documented
experimental  code exists, not board-qualified
unsupported   no native driver; use a compatibility host or sponsor a port
```

## Orange Pi 3B v1.1

| Function | L1 | L2 | L3 | L4 | Status | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| UART console | `#P` | PL011 | — | `/dev/cons` | partial | early console; user-space UART service exists |
| GPIO | `#P` | gpio bank | gpiofs | `/srv/gpio` | partial | banks, pinmux, per-pin IRQ via `/gpioN/wait` |
| I2C | `#P` | RK I2C | i2cfs | `/srv/i2c` | partial | six controllers, polled transfers |
| PMIC / regulators | `#P` | RK809 | pmicfs | `/srv/pmic` | partial | rails, RTC+alarm, CLKOUT2, power key (IRQ), thermal reporting |
| USB xHCI | `#P` | o9xhci | — | — | partial | root-port enumeration; no hubs |
| USB UAC1 | `#P` | o9xhci | UAC1 | pending | experimental | K670 48k/16/2 probe; no audio service |
| USB HID | `#P` | o9xhci | HID | pending | unsupported | HID class driver not written |
| USB storage | `#P` | o9xhci | MSC | pending | unsupported | class driver not written |
| Display / HDMI | `#P` | VOP2/DW-HDMI | o9display | `#V/display` | partial | fixed 1920x1080 RGB888 on HDMI0; no EDID/modeset, no audio |
| IOMMU | `#P` | rkiommu | — | — | partial | two-level tables, 40-bit PA; used by VOP2 |
| eMMC | `#P` | SDHCI (dwcmshc) | emmc/emmcfs | `/srv/emmc` | partial | user-space PIO+ADMA2, HS200 8-bit |
| SD card | `#P` | SDHCI (sdmmc0) | — | — | unsupported | slot not driven |
| SPI NOR | `#P` | RK SFC | — | — | unsupported | boot flash not readable |
| Ethernet | `#P` | GMAC/PHY | — | `/srv/net` | partial | kernel `#l`, 10 Mbps, ping works |
| WiFi / BT | `#P` | SDIO/UART | — | — | experimental | CP boots, scans, WPA2 associates; data path blocked by link budget |
| Audio codec | `#P` | RK809 codec + I2S | — | — | unsupported | analog out/in, headphone detect |
| HDMI audio | `#P` | DW-HDMI | — | — | unsupported | — |
| SARADC | `#P` | RK SARADC | — | — | unsupported | — |
| TSADC / thermal | `#P` | RK TSADC | — | — | unsupported | SoC temperature and trip points |
| PWM / fan | `#P` | RK PWM | — | — | unsupported | fan and IR carrier |
| Watchdog | `#P` | RK WDT | — | — | unsupported | — |
| TRNG | `#P` | RK RNG | — | — | unsupported | hardware random source |
| Crypto engine | `#P` | RK crypto | — | — | unsupported | AES/SHA offload |
| PCIe 2x1 | `#P` | DWC PCIe + combphy | — | — | unsupported | M.2 slot; no NVMe |
| VPU (decode) | `#P` | rkvdec | — | — | unsupported | H.264/H.265 decode |
| VEPU (encode) | `#P` | vepu | — | — | unsupported | — |
| RGA (2D) | `#P` | rga | — | — | unsupported | blit/scale |
| NPU | `#P` | rknpu (NVDLA-derived) | — | — | experimental | regcmd/task format and 0xfde40000 block decoded in 9rock; vendor driver present; no Harvey driver yet |
| GPU | `#P` | Mali G52 | — | — | unsupported | — |
| ISP / camera | `#P` | rkisp + vicap | — | — | unsupported | MIPI CSI |
| eFuse / OTP | `#P` | RK efuse | — | — | unsupported | — |
| TPM 2.0 (add-on) | `#P` | CRB/TIS | tpmfs | `/mnt/tpm` | experimental | SPI/CRB module, PCR and seal/unseal |

## Policy

- Native L2/L3 drivers are the supported path for on-board devices.
- Common USB classes are native where practical.
- Add-on USB/PCIe devices are best effort, third-party, or paid ports.
- WiFi/GPU/camera may use a Linux compatibility host if a native port is not
  justified.
- An `experimental` row is not a support promise; it is a documented starting
  point.

## Adding a row

A new driver entry must include:

- board revision and device identity;
- the four levels it implements;
- the 9P/URI interface;
- test evidence;
- known quirks and limitations;
- license.

If a level is missing, the row says so. “USB works” is not a support claim;
“this class on this board with these endpoints works” is.
