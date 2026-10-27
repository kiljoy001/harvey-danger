# Orange Pi 3B (RK3566) board dossier

This is a working dossier for the Orange Pi 3B v1.1/v2.1, built from the
vendor schematic, the mainline Linux device tree, the RK3568/RK3566 TRM, and
the observed register state on the board. It is intended to make the next
driver start from documented facts instead of a fresh bring-up session.

## Board revisions

The board revisions are not identical. The mainline DTS encodes these
differences:

| Item | v1.1 | v2.1 |
| --- | --- | --- |
| Ethernet PHY IO voltage | 3.3 V (`vccio5 = vcc_3v3`) | 1.8 V (`vccio_phy1`) |
| Ethernet PHY reset | GPIO3_C2 | GPIO4_C4 |
| WiFi/BT | CDW-20U5622 | AP6256 / BCM43456 |
| Ethernet speed | forced 100M on tested v1.1 | Gigabit |
| WiFi node | not described in mainline v1.1 DTS | `brcmf` on SDIO |

The USB controller and PHY wiring used by this dossier is the same on both
revisions.

Confirmed board under test: **v1.1**.

## Sources

Local copies:

- `dts/` — mainline Linux DTS for the board and RK3566/RK356x.
- `refs/orangepi-3b-v2.1-schematic.pdf` — vendor schematic (v2.1).
- `refs/orangepi-3b-v2.1-schematic.txt` — text extraction of the schematic.

Public sources:

- Orange Pi 3B downloads:
  <http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/service-and-support/Orange-Pi-3B.html>
- Armbian schematic cache:
  <https://armbian.lv.auroradev.org/cache/board-schematics/orangepi3b.pdf>
- Mainline DTS:
  `arch/arm64/boot/dts/rockchip/rk3566-orangepi-3b.dtsi`
  `rk3566-orangepi-3b-v1.1.dts`
  `rk3566-orangepi-3b-v2.1.dts`
- RK3566 datasheet V1.5:
  <https://rockchip.fr/RK3566%20datasheet%20V1.5.pdf>
- RK3568 TRM V1.3 Part 1 (RK3566 is the same family):
  <https://rockchip.fr/Rockchip%20RK3568%20TRM%20V1.3%20Part1.pdf>
- RK3566 Hardware Design Guide V1.1:
  <https://dl.xkwy2018.com/downloads/RK3568/RK356X/Hardware/Rockchip_RK3566_Hardware_Design_Guide_V1.1_EN.pdf>
- Vendor Linux/BSP:
  `orangepi-xunlong/linux-orangepi`, `orangepi-xunlong/orangepi-build`
- Rockchip binary blobs / DDR init:
  `rockchip-linux/rkbin`

## Documents

- `usb.md` — USB controllers, PHYs, VBUS, port mapping, observed state.
- `drivers-usb-audio.md` — UAC1 capture driver design and current status.
- `refs/trm-usb2phy.md` — TRM excerpts for USB2PHY_U2_GRF and USB_GRF.
- `pinmux.md` — GPIO0_A5/A6 VBUS pins, PMUGRF and GPIO register layout.
- `clocks.md` — USB clocks, gates and resets.
- `regulators.md` — 5 V VBUS rails and supply relationships.
- `irq-map.md` — USB and PHY interrupt assignments.
- `memory-map.md` — USB/PHY/GRF/CRU MMIO ranges.
- `dts/` — DTS source used to generate the tables.
- `refs/` — schematic and extraction notes.

## How to use this dossier

A user-space driver should need only:

1. the controller/PHY/GRF/CRU bases and sizes;
2. the IRQ numbers;
3. the VBUS GPIO and polarity;
4. the PHY power-on/suspend values;
5. the board revision differences.

Everything else is in the referenced Linux driver logic and the USB/xHCI/UAC
class specifications.

## Caveats

- The vendor schematic is v2.1. Revisions v1.1 and v2.1 differ in Ethernet and
  WiFi, not in the USB controller/PHY mapping used here.
- The RK3566 full TRM is not public. The RK3568 TRM is the closest public
  register reference; Linux drivers remain the practical source for exact
  sequences.
- Register values in this dossier are observations from Linux, U-Boot and the
  board, not guarantees for every board revision or bootloader.
