# USB/PHY MMIO map — Orange Pi 3B

## USB controllers and PHYs

| Region | Base | Size | Notes |
| --- | --- | --- | --- |
| DWC3 OTG0 | `0xfcc00000` | `0x400000` | xHCI at `+0x10000` |
| DWC3 host1 | `0xfd000000` | `0x400000` | xHCI at `+0x10000`; blue port |
| EHCI0 | `0xfd800000` | `0x40000` | USB2 host pair 0 |
| OHCI0 | `0xfd840000` | `0x40000` | companion |
| EHCI1 | `0xfd880000` | `0x40000` | USB2 host pair 1 |
| OHCI1 | `0xfd8c0000` | `0x40000` | companion |
| USB2 PHY0 | `0xfe8a0000` | `0x10000` | host/otg PHY |
| USB2 PHY1 | `0xfe8b0000` | `0x10000` | host/otg PHY |
| Combphy1 | `0xfe830000` | `0x100` | USB3/SATA/PCIe, blue port |
| Combphy2 | `0xfe840000` | `0x100` | PCIe/SATA/USB3 |
| USB2PHY0 GRF | `0xfdca0000` | `0x8000` | PHY0 suspend/line-state |
| USB2PHY1 GRF | `0xfdca8000` | `0x8000` | PHY1 suspend/line-state |

The RK3568 TRM also names a `USB_GRF` at `0xfdcf0000` (64 KiB). It is not the
same block as the `rockchip,usbgrf` properties in the board DTS, which point
at `0xfdca0000` and `0xfdca8000`.

## Clock/reset and pinmux

| Region | Base | Size | Notes |
| --- | --- | --- | --- |
| CRU | `0xfdd20000` | `0x10000` | clock/reset unit |
| PMUCRU | `0xfdd00000` | `0x10000` | PMU clock/reset |
| PMUGRF | `0xfdc20000` | `0x10000` | GPIO0 mux, PMU IO domains |
| SYS_GRF | `0xfdc60000` | `0x10000` | pinmux for GPIO1..4 |
| PIPE_GRF | `0xfdc50000` | `0x1000` | combphy PIPE |
| PIPE_PHY_GRF1 | `0xfdc80000` | `0x1000` | combphy1 |
| PIPE_PHY_GRF2 | `0xfdc90000` | `0x1000` | combphy2 |
| GPIO0 | `0xfdd60000` | `0x100` | VBUS host/otg pins |
| GPIO1 | `0xfe740000` | `0x100` | |
| GPIO2 | `0xfe750000` | `0x100` | |
| GPIO3 | `0xfe760000` | `0x100` | |
| GPIO4 | `0xfe770000` | `0x100` | |

## Power/IOMMU

| Region | Base | Notes |
| --- | --- | --- |
| PMU | `0xfdd90000` | power domains |
| VOP IOMMU | `0xfe043e00` / `0xfe043f00` | display |
| IOMMU | `0xfdea0800`, `0xfdee0800` | other masters |

## Boot memory

- Kernel load address: `0x40080000`.
- DTB load address used by the board boot flow: `0x48000000`.
- The board's RAM banks observed by U-Boot:
  - `0x00200000`–`0xf0000000`
  - `0x100000000`–`0x200000000`

## DMA

The RK3566 has IOMMUs for display and other masters, but USB DMA on this
board is normally identity-mapped by the vendor boot chain. A user-space
driver must not assume it can DMA anywhere: reserve a range, program the
relevant IOMMU if present, and pass only that range to the device.

The `#P` dossier exposes MMIO bases and sizes; DMA ranges and IOMMU
assignments still need to be derived from the board's reserved memory and the
SoC IOMMU topology.
