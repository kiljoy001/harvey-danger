# `goyave` hardware map

Derived from the downstream Linux 3.10 tree
(`pstglia/linux@e90d994cb5fe2531ff35ee13fb624f4bba2c07cc`,
`arch/arm/boot/dts/sprd-scx35_goyavewifi_rev04.dts` and
`sprd-scx35_sc7730.dtsi`) and the packed DTB for `sc-id ... 4 ...`.

## Memory

| Region | Address | Size | Notes |
|---|---|---|---|
| DRAM | `0x80000000` | `0x40000000` (1 GiB) | `memory { reg }` |
| SMEM | `0x87800000` | `0x240000` | shared memory, bootloader/AP |
| Modem/CP | `0x88000000` | `0x1c00000` | unused on Wi-Fi-only work |
| Framebuffer | `0x9eef4000` | `0x708000` (7.4 MiB) | `sprd,fb_mem` |
| ION | `0x9f5fc000` | `0x504000` | Android ION |

The kernel command line from the bootloader is
`console=ttyS1,115200n8 androidboot.selinux=permissive`; the DTS `chosen`
node adds `init=/init root=/dev/ram0 rw`.

## Interrupt controller and timers

| Block | Address | IRQ(s) | Compatible |
|---|---|---|---|
| GIC-400 (dist + CPU) | `0x12001000`, `0x12002000` | — | `arm,cortex-a15-gic` |
| Timer block 0 | `0xf5204000` | 118 | `sprd,scx35-timer` |
| Timer block 1 | `0xf51f4000` | 28 | |
| Timer block 2 | `0xf5202000` | 29 | |
| Timer block 3 | `0xf5292000` | 119 | |
| Timer block 4 | `0xf5294000` | 121 | |

The DTS uses the Spreadtrum timer, not the ARM generic timer. A Harvey port
can prefer the generic timer if `CNTFRQ`/`CNTVCT` are usable (Cortex-A7), but
the downstream tree is the reference.

## Serial

| Port | Address | IRQ | Clock |
|---|---|---|---|
| `uart0` | `0xf5360000` | 2 | `clk_uart0` |
| **`uart1` (console)** | **`0xf5362000`** | **3** | `clk_uart1` |
| `uart2` | `0xf5364000` | 4 | `clk_uart2` |
| `uart3` | `0xf5366000` | 5 | `clk_uart3` |

Driver: `drivers/tty/serial/serial_sprd.c` — **not 8250-compatible**. It is
a Spreadtrum UART with its own register layout; a Harvey driver is a small
new driver, not a reused one. Baud for the console is 115200 8N1.

## Storage

| Block | Address | IRQ | Notes |
|---|---|---|---|
| eMMC (`sdhci3`) | `0xf511c000` | 60 | 8-bit, non-removable, `sprd,sdhci-shark` |
| SD card (`sdhci0`) | `0xf5117000` | 57 | 4-bit, card detect GPIO 0x47 |
| Wi-Fi SDIO (`sdhci1`) | `0xf5118000` | 58 | BCM4343, 4-bit |

`sdhci-shark` is an SDHCI core with Spreadtrum clock/reset/pinmap bits. A
generic SDHCI host driver plus the Spreadtrum glue is the shortest path.
EDK2's `SprdSdhciDxe`/`SprdEmmcDxe` are reference register banging, but the
EDK2 port reports eMMC and delay as not working.

## USB

| Block | Address | IRQ | Notes |
|---|---|---|---|
| `usb` | `0xf5116000` | 55 | `sprd,usb`, DWC OTG core |

Driver: `drivers/usb/gadget/dwc_otg/`, with a Spreadtrum wrapper. Not xHCI.

## Display

| Block | Address | IRQ | Notes |
|---|---|---|---|
| `sprdfb` | `0xf5122000`, `0xf5146000` | 46, 48, 49 | `sprd,sprdfb` |
| Framebuffer memory | `0x9eef4000` | — | `0x708000`, 1024×600 |
| Panel | — | — | VX5B3D, DSI video mode |
| Backlight | — | — | `gen-panel-backlight` |
| DSI panel node | — | — | `sprd,sprdfb-i2c-panel` |

Panel init is `arch/arm/boot/dts/sprd-dsi-panel-vx5b3d-WSVGA_video.dtsi`
plus the panel code in `drivers/video/sprdfb/`. The bootloader programs the
display for its logo, and EDK2's `SimpleFbDxe` can use the framebuffer, so a
first Harvey framebuffer might work by inheriting the bootloader's setup
before writing a full DSI driver.

## I2C and input

| Bus | Address | Devices |
|---|---|---|
| `i2c0` | `0xf536a000` | camera sensors (`0x3c`, `0x21`) |
| `i2c1` | `0xf536c000` | **Melfas MMS-449 touch `0x48`** |
| `i2c2` | `0xf5370000` | K2HH accelerometer `0x1d`, SX9500 `0x28` |
| `i2c3` | `0xf5372000` | — |
| `i2c_gpio6` | GPIO 159/160 | SMB328 charger `0x34` |
| `i2c_gpio7` | GPIO 150/151 | SM5504 `0x14` |

Touch driver: `drivers/input/touchscreen/mms_ts.c` (Melfas MMS-449), with
max 1024×600. Keypad: `sprd,sci-keypad` at `0xf5208000`, plus a GPIO home
key on GPIO 113. udev calibration is `0 1 0 -1 0 1` (landscape).

## Wi-Fi / Bluetooth / GPS

| Block | Notes |
|---|---|
| Wi-Fi | Broadcom BCM4343 over SDIO (`sdhci1`), `bcmdhd` driver |
| Firmware | `bcmdhd_sta.bin`, `bcmdhd_apsta.bin`, `bcmdhd_mfg.bin`, `nvram_net.txt` |
| Bluetooth | BCM4343 UART + rfkill/LPM GPIOs 231/233/232/235 |
| GPS | BCM47520, power GPIO 168 |

`CONFIG_BCM4343=y`, `CONFIG_BCMDHD_FW_PATH="/system/etc/wifi/bcmdhd_sta.bin"`.
Wi-Fi is the last bring-up item; USB or SD-card transfer is enough before
that.

## PMIC, clocks, regulators

- Regulators: `sprd,sc2723-regulator` (SC2723), plus ADI/efuse nodes.
- Clocks: `sprd,scx30g-clocks` (`clockdevice`), with `clk_emmc`, `clk_sdio0`,
  `clk_sdio1`, `clk_sdio2`, `clk_dispc*`, `clk_uart*`, PLLs.
- Charger: SMB328 on `i2c_gpio6`; fuel gauge/charger SM5504 on `i2c_gpio7`.
- Battery/PMIC initialisation is required before eMMC and USB; the EDK2
  clock driver is still WIP.

## Packed DTBs (`dt.img`)

`upstream/dt.img` is a Spreadtrum image, magic `SPRD`:

```
header: "SPRD", version:u32, count:u32, chip:u32
entry : id:u32, rev:u32, offset:u32, size:u32, chip:u32   (count entries)
```

For the SM-T113 image: version 1, count 5, chip 8830; every entry is
`0x20000` revision and `0xd000` bytes, at offsets `0x800`, `0xd800`,
`0x1a800`, `0x27800`, `0x34800`.

| id | model | sc-id |
|---|---|---|
| 0 | Spreadtrum SP7727S board | `<8830 0 0x20000>` |
| 1 | Spreadtrum SP7727S board | `<8830 1 0x20000>` |
| 2 | Spreadtrum SP7730G board | `<8830 2 0x20000>` |
| 3 | Spreadtrum SP7730G board | `<8830 3 0x20000>` |
| **4** | **Spreadtrum SP7730G board** | **`<8830 4 0x20000>`** |

Entry 4 matches `sprd-scx35_goyavewifi_rev04.dts`, so that is the DTB for
the Wi-Fi tablet. `scripts/sprd-dt.py` unpacks all five.

## Boot chain and image layout

The Samsung/Spreadtrum bootloader loads an Android boot image from the
`KERNEL` partition (16 MiB), then the kernel. The postmarketOS `deviceinfo`
and the LineageOS `BoardConfig.mk` agree on:

| | |
|---|---|
| Boot partition | 16 MiB (`BOARD_BOOTIMAGE_PARTITION_SIZE`) |
| Kernel base | `0x00000000` |
| Kernel offset | `0x00008000` |
| Ramdisk offset | `0x01000000` |
| Second stage | `0x00f00000` |
| Tags offset | `0x00000100` |
| Page size | 2048 |
| Flash method | Heimdall (`KERNEL`, `system`) |
| Cmdline | `console=ttyS1,115200n8` |

A Harvey boot image would need the kernel at the kernel offset and a DTB
(the bootloader passes ATAGS/DTB per the Spreadtrum boot protocol; the
downstream kernel uses the packed `dt.img`). Keep a stock `boot.img` and
TWRP available before flashing.

## Source hashes

```
492f29875db97a9e0fb0cafd563d4ad60e7b6fad50b722030632e0e3a7ea61c2  linux-sc8830.tar.gz
10c2d0c496e133b858acab06813a19925ef826e694b6e2f4e754d3f747d44b54  dt.img
```
