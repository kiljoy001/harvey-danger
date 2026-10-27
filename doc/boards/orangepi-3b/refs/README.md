# Reference material

## Local files

| File | SHA-256 |
| --- | --- |
| `orangepi-3b-v2.1-schematic.pdf` | `3a81a67b26ced7027180891ad88c57eaa12a50942979dca55270f69f8cd0537a` |
| `orangepi-3b-v2.1-schematic.txt` | `7e232625b227cb525c71aff2048c0d52ed8191a90af8c446a69638264e8390be` |

The schematic is the vendor v2.1 document. The v1.1 board differs in the
Ethernet PHY IO voltage/reset and WiFi module; the USB controller/PHY/VBUS
mapping is the same for the purposes of this dossier.

## External documents

Large vendor PDFs are intentionally not committed. Download them separately:

- RK3566 datasheet V1.5:
  <https://rockchip.fr/RK3566%20datasheet%20V1.5.pdf>
- RK3568 TRM V1.3 Part 1:
  <https://rockchip.fr/Rockchip%20RK3568%20TRM%20V1.3%20Part1.pdf>
- RK3566 Hardware Design Guide V1.1:
  <https://dl.xkwy2018.com/downloads/RK3568/RK356X/Hardware/Rockchip_RK3566_Hardware_Design_Guide_V1.1_EN.pdf>
- RK3568 TRM V1.1 Part 1/2 (Radxa mirror):
  <https://dl.radxa.com/rock3/docs/hw/datasheet/Rockchip%20RK3568%20TRM%20Part1%20V1.1-20210301.pdf>
  <https://dl.radxa.com/rock3/docs/hw/datasheet/Rockchip%20RK3568%20TRM%20Part2%20V1.1-20210301.pdf>

Useful TRM sections:

- Chapter 1.1 — address mapping
- Chapter 1.2 — boot
- Chapter 1.3 — interrupt connections
- Chapter 2.6 — PMUCRU registers
- Chapter 3.3 — PMU_GRF registers
- Chapter 3.10 — USB2PHY_U2_GRF registers
- Chapter 3.13 — USB_GRF registers

## DTS sources

The `dts/` directory contains mainline Linux files. They are GPL-2.0+ OR MIT.
The `rk3566-orangepi-3b.dtsi` includes `rk3566.dtsi`; the latter includes
`rk3566-base.dtsi` and `rk356x-base.dtsi`.

## Notes on revisions

Mainline v1.1 vs v2.1 differences:

- v1.1: Ethernet PHY IO is 3.3 V, reset GPIO3_C2, Ethernet forced to 100M on
  the tested board, WiFi module CDW-20U5622.
- v2.1: Ethernet PHY IO is 1.8 V, reset GPIO4_C4, Gigabit Ethernet, WiFi
  AP6256/BCM43456.

The board under test for the USB work is v1.1.
