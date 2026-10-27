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
| GPIO | `#P` | gpio bank | gpiofs | `/srv/gpio` | partial | bank/pinmux discovery on board |
| I2C | `#P` | RK I2C | i2cfs | `/srv/i2c` | experimental | controller setup, no device drivers |
| USB xHCI | `#P` | o9xhci | — | — | partial | root-port enumeration; no hubs |
| USB UAC1 | `#P` | o9xhci | UAC1 | pending | experimental | K670 48k/16/2 probe; no audio service |
| USB HID | `#P` | o9xhci | HID | pending | unsupported | HID class driver not written |
| USB storage | `#P` | o9xhci | MSC | pending | unsupported | class driver not written |
| Display / HDMI | `#P` | VOP2/DW-HDMI | — | `/dev/display` | experimental | kernel driver removed; user-space port pending |
| eMMC / SD | `#P` | SDHCI | — | `/srv/...` | unsupported | no block driver in kernel |
| Ethernet | `#P` | GMAC/PHY | — | `/srv/net` | unsupported | no native driver |
| WiFi / BT | `#P` | SDIO/UART | — | — | unsupported | vendor BSP candidate for compatibility host |
| PMIC / regulators | `#P` | RK809 | — | — | partial | fixed rails; no full PMIC service |

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
