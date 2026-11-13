# Samsung `goyave` bring-up — Phase 0

Hardware reconnaissance for the Samsung Galaxy Tab E Lite / Tab 3 Lite
(`SM-T113` Wi-Fi, `SM-T116` 3G), as a candidate Harvey target.

**Status:** Phase 0 (source and hardware inventory) done. No device attached
yet; the DTB, kernel source and reference ports are in hand.

## Device

| | |
|---|---|
| Models | SM-T113 (`goyavewifi`), SM-T116 (`goyave3g`) |
| SoC | Spreadtrum SC8830 / SCX35, bootloader board name `sc7730s` |
| CPU | 4× Cortex-A7 (ARMv7-A, NEON), `sc-id = <8830 4 0x20000>` |
| RAM | 1 GiB at `0x80000000` (`reg = <0x80000000 0x40000000>`) |
| Display | 1024×600, VX5B3D panel over DSI (`gen-panel-name = "vx5b3d"`) |
| Touch | Melfas MMS-449, I2C1 address `0x48` |
| Storage | eMMC, SDHCI "shark" |
| Wi-Fi/BT | Broadcom BCM4343 (SDIO, `bcmdhd`) |
| PMIC | Spreadtrum SC2723 regulators + ADI |
| Android | 4.4 KitKat (stock), TWRP and LineageOS 14.1 exist |

## What we have

- **Kernel source:** [pstglia/linux](https://github.com/pstglia/linux) at
  `e90d994cb5fe2531ff35ee13fb624f4bba2c07cc` (Linux 3.10.17 + Spreadtrum
  SC8830). Downloaded to `upstream/` (gitignored).
- **Board config:** `arch/arm/configs/goyavewifi-dt_hw04_defconfig`.
- **Device tree:** `arch/arm/boot/dts/sprd-scx35_goyavewifi_rev04.dts`
  (`sc-id = <8830 4 0x20000>`) plus `sprd-dsi-panel-vx5b3d-WSVGA_video.dtsi`.
- **Packed DTBs:** `upstream/dt.img` from
  [sixito007/android_device_samsung_goyave3g](https://github.com/sixito007/android_device_samsung_goyave3g)
  (branch `cm-14.1`). It is a SPRD-format image with five DTBs; entry 4 is
  the Wi-Fi board (`sc-id ... 4 ...`, model `Spreadtrum SP7730G board`).
- **Reference ports:**
  - postmarketOS downstream port:
    [kuskebabi/apkbuild-samsung-goyavewifi](https://github.com/kuskebabi/apkbuild-samsung-goyavewifi)
    — `deviceinfo` gives the boot-image layout and `console=ttyS1,115200n8`.
  - EDK2/UEFI experiment:
    [vizPilot/edk2-goyavewifi](https://github.com/vizPilot/edk2-goyavewifi)
    — boots to a UEFI shell with a working framebuffer; eMMC, USB, buttons
    and delay are **not** working.
  - LineageOS 14.1 / TWRP for `goyavewifi` (Android-side reference).

## Reproducing the inventory

```sh
# kernel
curl -L -o upstream/linux-sc8830.tar.gz \
  https://github.com/pstglia/linux/archive/e90d994cb5fe2531ff35ee13fb624f4bba2c07cc.tar.gz
mkdir -p upstream/linux
tar -xzf upstream/linux-sc8830.tar.gz -C upstream/linux --strip-components=1

# flattened device tree
./scripts/extract-dts.sh upstream/linux /tmp/goyave.flat.dts

# packed Spreadtrum DTBs
./scripts/sprd-dt.py upstream/dt.img upstream/dtbs
```

`docs/hardware.md` is the address/IRQ map and the boot chain.

## Why this matters for Harvey

Harvey has no 32-bit ARM kernel (`amd64`, `aarch64`, `386`, `riscv` only), so
this is a new `sys/src/9/arm` port plus an SC8830 board port. 9front's
`sys/src/9/bcm` and `omap` ports supply the ARMv7 CPU core (MMU, caches,
traps, SMP); none of the Spreadtrum drivers exist in either tree.

The non-obvious obstacles, now confirmed from the source:

- **UART is not 8250.** `drivers/tty/serial/serial_sprd.c` is a custom
  Spreadtrum UART. Console must be `uart1` at `0xf5362000`, IRQ 3.
- **Timers are custom** (`sprd,scx35-timer`), not the ARM generic timer;
  GIC is a GIC-400 at `0x12001000`.
- **eMMC is `sdhci-shark`** with Spreadtrum clock/reset bits. The EDK2 port
  has not made eMMC work, which is a warning for bring-up order.
- **USB is `sprd,usb`** over a DWC OTG core, not xHCI/virtio.
- **Display** is `sprd,sprdfb` with a DSI panel; the bootloader already
  leaves a framebuffer that EDK2's SimpleFbDxe can use, so a minimal
  framebuffer path may be possible without the full DSI driver.
- **No mainline Linux support.** The downstream 3.10 tree is the only
  driver reference.

## Next

1. **Phase 1/2:** scaffold `sys/src/9/arm` from 9front's ARMv7 core and add
   an ARM `virt` QEMU config (PL011, GIC-400, generic timer, virtio) to get a
   serial console test loop without hardware.
2. Add ARMv7 to `../9vm/mksys.py` and build `sys/src/libc/arm` +
   `klibc` assembly.
3. Then the board port in this order: UART → GIC/timer → eMMC → display →
   touch → USB → Wi-Fi.

The device is not connected to this machine. When it is, `adb devices`
should show it; extracting `boot.img` from the stock firmware would confirm
that DTB entry 4 matches the unit on the bench.
