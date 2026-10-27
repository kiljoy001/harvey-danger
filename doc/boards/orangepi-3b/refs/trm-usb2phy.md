# RK3568 TRM excerpts — USB2 PHY and USB GRF

Source: Rockchip RK3568 TRM V1.3 Part 1, chapter 3.10 (USB2PHY_U2_GRF) and
chapter 3.13 (USB_GRF). RK3566 is the same family.

## USB2PHY_U2_GRF

Base (board DTS): PHY0 `0xfdca0000`, PHY1 `0xfdca8000`.

Register summary:

| Offset | Name | Reset | Description |
| --- | --- | --- | --- |
| `0x0000` | `USBPHY_U2_GRF_CON0` | `0x00000c52` | host0 UTMI control |
| `0x0004` | `USBPHY_U2_GRF_CON1` | `0x000001d2` | host1 UTMI control |
| `0x0008` | `USBPHY_U2_GRF_CON2` | `0x00000000` | PHY control |
| `0x000c` | `USBPHY_U2_GRF_CON3` | `0x00000019` | PHY control |
| `0x0040` | `USBPHY_U2_GRF_LS_CON` | `0x00030100` | linestate control |
| `0x0044` | `USBPHY_U2_GRF_DIS_CON` | `0x00030100` | disconnect control |
| `0x0048` | `USBPHY_U2_GRF_BVALID_CON` | `0x00030100` | bvalid control |
| `0x004c` | `USBPHY_U2_GRF_ID_CON` | `0x00030100` | ID control |
| `0x0080` | `USBPHY_U2_GRF_INT_MASK` | `0` | interrupt mask |
| `0x0084` | `USBPHY_U2_GRF_INT_STATUS` | `0` | interrupt status |
| `0x0088` | `USBPHY_U2_GRF_INT_STATUS_CLR` | `0` | interrupt clear |
| `0x00c0` | `USBPHY_U2_GRF_STATUS` | `0` | status |

### CON0 (offset `0x0000`)

| Bits | Field | Notes |
| --- | --- | --- |
| 31:16 | write_enable | per-bit write mask |
| 11 | `usb2host0_utmi_idpullup` | reset 1 |
| 10 | `usb2host0_utmi_iddig` | reset 1 |
| 9 | `usb2host0_utmi_iddig_sel` | 0 = from PHY, 1 = from GRF |
| 8 | `usb2host0_utmi_dmpulldown` | |
| 7 | `usb2host0_utmi_dppulldown` | |
| 6 | `usb2host0_utmi_termselect` | FS/LS/HS termination |
| 5:4 | `usb2host0_utmi_xcvrselect` | FS/LS/HS transceiver |
| 3:2 | `usb2host0_utmi_opmode` | |
| **1** | `usb2host0_utmi_suspend_n` | **0 = suspend, 1 = normal** |
| 0 | `usb2host0_utmi_sel` | 0 = controller UTMI, 1 = GRF UTMI |

### CON1 (offset `0x0004`)

The same layout for host1: `suspend_n` bit 1, 0 = suspend, 1 = normal.

This confirms the Linux/vendor value convention:

```text
0x1d2  -> bit 1 = 1 -> normal/power-on
0x1d1  -> bit 1 = 0 -> suspend
```

PHY register offsets `0x0000`/`0x0004` are therefore **UTMI control registers
with a suspend bit**, not a single opaque `phy_sus` field. The Linux driver's
`{offset, bitend, bitstart, disable, enable}` tuple writes the low bits,
including `suspend_n`.

For PHY1 host/otg, power-on is `0x1d2`; suspend is `0x1d1`. Setting `0x1d1`
leaves the PHY suspended and prevented the FIFINE K670 (full-speed) from
enumerating.

## USB_GRF

Base: `0xfdcf0000` (64 KiB). This is **not** the same block as the
`rockchip,usbgrf` properties, which point at `0xfdca0000`/`0xfdca8000`.

| Offset | Name | Reset | Description |
| --- | --- | --- | --- |
| `0x0700` | `USB_GRF_HOST0_CON0` | `0x00000820` | host0 fladj |
| `0x0704` | `USB_GRF_HOST0_CON1` | `0x000004bc` | host0 arbiter/OHCI control |
| `0x0708` | `USB_GRF_HOST1_CON0` | `0x00000820` | host1 fladj |
| `0x070c` | `USB_GRF_HOST1_CON1` | `0x000004bc` | host1 arbiter/OHCI control |
| `0x0890` | `USB_GRF_HOST0_STATUS` | `0` | host0 status |
| `0x0894` | `USB_GRF_HOST1_STATUS` | `0` | host1 status |

`HOST0_CON1` controls the EHCI/OHCI arbiter pause (`bit 13`), OHCI suspend
legacy (`bit 12`), OHCI clock counter reset (`bit 10`), port overcurrent
(`bit 9`) and auto-port-power-down on overcurrent (`bit 8`).

## Observed values

At the point the FIFINE enumerated:

```text
PHY0 CON1 = 0x1d2  (host0 normal)
PHY1 CON0 = 0x1d2  (host1 normal)
PHY1 CON1 = 0x1d2  (host1 normal)
```

The earlier failing state had PHY1 at `0x1d1` (suspend).
