# Interrupt map — USB

Linux DTS uses `GIC_SPI n`. The GIC INTID is normally `n + 32`; a kernel
interrupt number may differ again after the GIC driver maps it.

| Device | DTS interrupt | GIC INTID | Notes |
| --- | --- | --- | --- |
| DWC3 OTG0 `fcc00000` | `GIC_SPI 169` | 201 | USB3 OTG / USB2 download |
| DWC3 host1 `fd000000` | `GIC_SPI 170` | 202 | blue USB3 port |
| EHCI0 `fd800000` | `GIC_SPI 130` | 162 | USB2 host pair 0 |
| OHCI0 `fd840000` | `GIC_SPI 131` | 163 | USB2 host pair 0 |
| EHCI1 `fd880000` | `GIC_SPI 133` | 165 | USB2 host pair 1 |
| OHCI1 `fd8c0000` | `GIC_SPI 134` | 166 | USB2 host pair 1 |
| USB2 PHY0 `fe8a0000` | `GIC_SPI 135` | 167 | line-state / VBUS detect |
| USB2 PHY1 `fe8b0000` | `GIC_SPI 136` | 168 | line-state / VBUS detect |

## GIC

| Block | Base | Size |
| --- | --- | --- |
| GIC distributor | `0xfd400000` | `0x10000` |
| GIC ITS | `0xfd440000` | `0x20000` |
| GIC redistributor | `0xfd460000` | `0xc0000` |

The GICv3 PPI for the generic timer is described by the timer node; the
Harvey early boot log reports the timer IRQ as `0x1b` (27). Treat that as an
observation, not a derived GIC rule.

## PHY interrupt use

The RK356x USB2 PHY driver uses the PHY IRQ for:

- line-state detection (`ls_det_st` at `0x0084`)
- VBUS/OTG ID changes
- charger detection

For a simple host driver, the PHY IRQ can be left masked; the controller’s
own port-change interrupt is enough. If the PHY IRQ is used, clear
`ls_det_st` via `ls_det_clr` at `0x0088`.

## OBSERVED

Linux `/proc/interrupts` on the board showed the USB2 PHY interrupts registered
by `rockchip_usb2phy`, and the xHCI/EHCI/OHCI controllers registered their own
IRQs. The exact Linux virq numbers should be read from `/proc/interrupts` on
the running board, not assumed from the DTS.
