# USB map — Orange Pi 3B

## Controllers

| Block | Base | Size | IRQ | Role | PHY |
| --- | --- | --- | --- | --- | --- |
| DWC3 OTG0 | `0xfcc00000` | `0x400000` | 169 | USB2 OTG / download port | `u2phy0_otg` |
| DWC3 host1 | `0xfd000000` | `0x400000` | 170 | USB3 host, blue port | `u2phy0_host` + `combphy1` USB3 |
| EHCI0 | `0xfd800000` | `0x40000` | 130 | USB2 host 0 | `u2phy1_otg` |
| OHCI0 | `0xfd840000` | `0x40000` | 131 | USB2 host 0 companion | `u2phy1_otg` |
| EHCI1 | `0xfd880000` | `0x40000` | 133 | USB2 host 1 | `u2phy1_host` |
| OHCI1 | `0xfd8c0000` | `0x40000` | 134 | USB2 host 1 companion | `u2phy1_host` |

The DWC3 blocks expose an xHCI register interface at `base + 0x10000`.
The USB2-only DWC3 (`fcc00000`) is limited to high speed by
`rk3566-base.dtsi` (`maximum-speed = "high-speed"`) and uses the OTG PHY port.

## USB2 PHYs

| PHY | Base | GRF | IRQ | Ports | Supplies |
| --- | --- | --- | --- | --- | --- |
| PHY0 | `0xfe8a0000` | `0xfdca0000` | 135 | host, otg | host: `vcc5v0_usb_host`; otg: `vcc5v0_usb_otg` |
| PHY1 | `0xfe8b0000` | `0xfdca8000` | 136 | host, otg | both: `vcc5v0_usb_host` |

PHY register layout for the RK3568 family:

| Offset | Field | Notes |
| --- | --- | --- |
| `0x0000` | PHY0 otg / PHY1 otg `phy_sus` | 9-bit field, bit 8..0 |
| `0x0004` | PHY0 host / PHY1 host `phy_sus` | 9-bit field, bit 8..0 |
| `0x0008` | `clkout_ctl` bit 4 | 480 MHz output; 0 = enabled on this board |
| `0x0040` | `ls_filter_con` | line-state filter |
| `0x0080` | `ls_det_en` | PHY1 otg bit 0, host bit 1; PHY0 host bit 1, otg bit 0 |
| `0x0084` | `ls_det_st` | line-state detect status |
| `0x0088` | `ls_det_clr` | line-state detect clear |
| `0x00c0` | `utmi_ls` | PHY1 host bits 17:16, otg bits 5:4; `utmi_hstdet` bit 19 / 7 |

Linux `phy-rockchip-inno-usb2.c` stores these as
`{offset, bitend, bitstart, disable, enable}`. `power_on()` writes the
**disable** value, not the enable value:

| Port | `disable` (power-on) | `enable` (suspend) |
| --- | --- | --- |
| PHY0 host | `0x1d2` | `0x1d2` |
| PHY0 otg | `0x0` | `0x1d1` |
| PHY1 host | `0x1d2` | `0x1d1` |
| PHY1 otg | `0x1d2` | `0x1d1` |

This is the value set that made the FIFINE K670 enumerate. Setting PHY1 to
`0x1d1` is the suspend value, not power-on.

The PHY0 OTG row is the vendor table value; the blue host port uses PHY0
**host**, not OTG. Verify the OTG row against the TRM before using the OTG
port as a host.

## VBUS

| Rail | Control | Polarity | Consumers |
| --- | --- | --- | --- |
| `vcc5v0_usb_host` | GPIO0_A6 | active high | PHY0 host, PHY1 host, PHY1 otg |
| `vcc5v0_usb_otg` | GPIO0_A5 | active high | PHY0 otg |

Both rails are fixed 5 V regulators fed from `vcc5v0_sys`.

The GPIO0 controller uses the RK356x V2 register layout: the upper 16 bits
are a write-enable mask. A plain write of `0x40` does nothing. To set
GPIO0_A6 high, write `0xffff0040` (mask all, value bit 6) to the appropriate
port register. See `pinmux.md`.

## Port mapping

From the v2.1 schematic page 5:

| Connector | USB2 D+/D- | USB3 SS | Controller |
| --- | --- | --- | --- |
| Blue USB3 Type-A | `USB_HOST1_DP/DM` | `USB3_HOST1_SS*` | DWC3 `fd000000` + `combphy1` |
| Regular USB2 HOST_2 | `USB2_HOST2_DP/DM` | none | EHCI/OHCI pair 0 |
| Regular USB2 HOST_3 | `USB2_HOST3_DP/DM` | none | EHCI/OHCI pair 1 |
| USB-C / OTG | `USB_OTG0_DP/DM` | none | DWC3 `fcc00000` + `u2phy0_otg` |

The regular HOST_2/HOST_3 labels map to the two EHCI/OHCI pairs by the PHY
port assignment in the DTS; confirm the physical top/bottom connector with a
continuity check if the distinction matters.

## Observed device

FIFINE K670 USB microphone, blue port:

- USB 1.10, full speed
- VID `3142`, PID `0001`
- EP0 `bMaxPacketSize0 = 16`
- Configuration descriptor length `235`
- Interface 2 alternate 1: isochronous IN endpoint `0x82`,
  `wMaxPacketSize = 200`, `bInterval = 1`
- Interface 1 alternate 1: isochronous OUT endpoint `0x01`,
  `wMaxPacketSize = 200`, `bInterval = 1`
- Interface 3 alternate 0: interrupt IN endpoint `0x87`,
  `wMaxPacketSize = 16`, `bInterval = 2`

The K670 is UAC1, 48 kHz/16-bit/stereo. The 200-byte full-speed isochronous
packet is 192 bytes of PCM plus overhead.

## Reference driver logic

- `drivers/phy/rockchip/phy-rockchip-inno-usb2.c` — PHY power, suspend,
  line-state detect, charger detect.
- `drivers/usb/dwc3/` — DWC3 core, host mode, GUSB2PHYCFG/GUSB3PIPECTL.
- `drivers/usb/host/xhci*`, `ehci*`, `ohci*` — host controller programming.
- `drivers/clk/rockchip/clk-rk3568.c` — USB clock gates and parents.
- `drivers/pinctrl/pinctrl-rockchip.c` — GPIO0/PMUGRF mux and V2 GPIO mask.
- `drivers/regulator/` — fixed-regulator GPIO handling.
- `drivers/usb/class/` and `sound/usb/` — UAC1 audio class logic (reference
  for the next step; the K670 is UAC1 full speed).

## Debug notes

- `PORTSC = 0x2a0` means powered, not connected, disabled, RxDetect.
- A stale `#X` transport can block the next probe; the probe must call
  `o9xhci_stop()` on every exit path.
- `ohci` port status `0x10100` means PPS + CSC with no CCS; do not treat that
  as a connected device.
- A full-speed device must be tested on both the xHCI USB2 port and the
  EHCI/OHCI companion path; high-speed flash drives can hide a broken FS path.
