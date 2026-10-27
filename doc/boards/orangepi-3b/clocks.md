# USB clocks, gates and resets

## CRU/PMUCRU bases

| Block | Base |
| --- | --- |
| CRU | `0xfdd20000` (see note) |
| PMUCRU | `0xfdd00000` |
| PMUGRF | `0xfdc20000` |

The RK3568 TRM lists the CRU at `0xfdd20000`; the vendor U-Boot/Linux
DTS uses `0xfdd20000` for CRU and `0xfdd00000` for PMUCRU. Verify against the
TRM address map before using a different base.

## DWC3 USB3 controllers

### OTG0 / download port — `usb@fcc00000`

| Resource | Name |
| --- | --- |
| `ref_clk` | `CLK_USB3OTG0_REF` |
| `suspend_clk` | `CLK_USB3OTG0_SUSPEND` |
| `bus_clk` | `ACLK_USB3OTG0` |
| `pipe_clk` | `PCLK_PIPE` |
| reset | `SRST_USB3OTG0` |
| power domain | `RK3568_PD_PIPE` |

### USB3 host — `usb@fd000000`

| Resource | Name |
| --- | --- |
| `ref_clk` | `CLK_USB3OTG1_REF` |
| `suspend_clk` | `CLK_USB3OTG1_SUSPEND` |
| `bus_clk` | `ACLK_USB3OTG1` |
| `pipe_clk` | `PCLK_PIPE` |
| reset | `SRST_USB3OTG1` |
| power domain | `RK3568_PD_PIPE` |

## EHCI/OHCI USB2 host controllers

Both EHCI and OHCI controllers in a pair share the same clocks:

```text
usbhost   HCLK_USB2HOST0 / HCLK_USB2HOST1
arbiter   HCLK_USB2HOST0_ARB / HCLK_USB2HOST1_ARB
pclk      PCLK_USB
utmi      usb2phy1 (PHY output clock)
```

| Pair | Controllers | PHY |
| --- | --- | --- |
| 0 | `fd800000` EHCI, `fd840000` OHCI | `u2phy1_otg` |
| 1 | `fd880000` EHCI, `fd8c0000` OHCI | `u2phy1_host` |

## USB2 PHYs

| PHY | Clock | Output |
| --- | --- | --- |
| PHY0 | `CLK_USBPHY0_REF` | `usb480m_phy` |
| PHY1 | `CLK_USBPHY1_REF` | 480 MHz to controllers |

`USB480M` is assigned to the PHY0 output in `rk356x.dtsi`.

## Naneng combphy (USB3)

`combphy1` is the USB3/SATA/PCIe combo PHY used by the blue port:

| Resource | Name |
| --- | --- |
| `refclk` | `CLK_PCIEPHY1_REF` (100 MHz) |
| `apbclk` | `PCLK_PIPEPHY1` |
| `pipe_clk` | `PCLK_PIPE` |
| reset | `SRST_P_PIPEPHY1`, `SRST_PIPEPHY1` |
| GRF | `pipegrf` `0xfdc50000`, `pipe_phy_grf1` `0xfdc80000` |

`combphy2` is the PCIe/SATA/USB3 combo used elsewhere on the SoC.

## Observed gate/reset offsets

These are the offsets used by the Harvey bring-up code and the vendor
U-Boot/Linux sequence. They are **observations**, not a complete CRU map.

| Write | Meaning |
| --- | --- |
| `CRU 0x0328` | `ACLK_PIPE`, `PCLK_PIPE`, USB3OTG1 gates |
| `CRU 0x0340` | `PCLK_USB`, `HCLK_USB2HOST0/1`, arbiter gates |
| `CRU 0x0424` | `SRST_USB3OTG0` bit 4, `SRST_USB3OTG1` bit 5 |
| `CRU 0x0388` | `PCLK_PIPEPHY1` gate |
| `CRU 0x0470` | combphy PIPE resets |
| `PMUCRU 0x0184` | GPIO0 `PCLK`/`DBCLK` gates |
| `PMUCRU 0x0188` | `XIN_OSC0_USBPHY0_G` bit 1, `XIN_OSC0_USBPHY1_G` bit 2 |
| `PMUCRU 0x0124` | combphy reference clock selection |

Use the Linux `clk-rk3568.c` and U-Boot `clk_rk3568.c` tables as the
authoritative source for bit positions. The TRM CRU chapter has the full
register descriptions.
