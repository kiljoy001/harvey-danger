# WiFi bring-up on the Orange Pi 3B (rk3566)

## What is actually on the board

The two board revisions use different WiFi modules, and the vendor and
Armbian device trees disagree about it:

| revision | module | evidence |
| --- | --- | --- |
| v1.1 | Unisoc **UWE5622** (AW859A), chipid `0x2355b001` | vendor DTS `unisoc_uwe_bsp`; SDIO IDs `0000:0000`; firmware `wcnmodem.bin` |
| v2.1 | Broadcom **BCM4345C5** (AP6256 family) | vendor DTS `brcm,bcm4345c5` bluetooth node, Broadcom SDIO IDs |

Armbian's DTB declares `brcm,bcm43456-fmac` for both revisions.  On a
v1.1 board that does not match the hardware: the SDIO function reports
vendor/device `0x0000/0x0000` with no readable CIS, which is the Unisoc
signature, and `brcmfmac` has nothing to bind to.  This is a known
board/DT mismatch, not a dead card.

Wiring, from the v1.1 vendor DTS:

```
sdmmc1          DW MSHC at 0xfe2c0000, IRQ 99, 4-bit, SDIO function 1
WL_REG_ON       gpio0 RK_PD3 (bank 0, pin 27), active high
host-wake       gpio0 RK_PD6 (bank 0, pin 30)
BT_REG_ON       gpio2 RK_PB7 (bank 2, pin 15)
32 kHz LPO      RK809 CLK32KOUT (register 0x20, bit 7)
firmware        /vendor/etc/firmware/wcnmodem.bin
pins            GPIO2 A3..A7 = D0..D3, CMD; GPIO2 B0 = CLK
```

The module needs WL_REG_ON high, a 200 ms wait, and the 32 kHz output
from the PMIC before its SDIO core answers.

## Firmware and reference driver

The Armbian image in `9rock/` (Armbian_community_26.2.0 ... 6.18.21)
ships everything needed to study a working setup:

```
/lib/firmware/uwe5622/wcnmodem.bin            947120 bytes
/lib/firmware/uwe5622/wcnmodem-38222.bin     1753088 bytes
/lib/firmware/uwe5622/wifi_2355b001_1ant.ini
/lib/modules/6.18.21-current-rockchip64/kernel/drivers/net/wireless/uwe5622/
    unisocwifi/sprdwl_ng.ko     (775712 bytes, not stripped)
    tty-sdio/sprdbt_tty.ko
```

The low-level transport (`uwe5622_bsp_sdio`, "sdiohal"/"marlin") is
built into Armbian's kernel; `sprdwl_ng.ko` is the cfg80211 WLAN driver.
Neither is in mainline.  Out-of-tree sources exist, e.g.
`unifreq/linux-5.10.y`, `orangepi-xunlong/linux-orangepi` and
`MichaIng/uwe5622`.

Booting the Armbian initramfs on this board confirms the hardware:

```
dwmmc_rockchip fe2c0000.mmc: DW MMC controller at irq 69, 256 deep fifo
mmc_host mmc2: Bus speed (slot 0) = 375000Hz (slot req 400000Hz, div = 0)
mmc2: new UHS-I speed SDR104 SDIO card at address 8800
```

So the card enumerates at 375 kHz and later runs SDR104 at 150 MHz.

## Scope

Working native WiFi is a large project:

1. **SDIO host + enumeration** (this work).
2. **Firmware download** ("marlin"): push `wcnmodem.bin` into the chip
   over SDIO, start the CP, and get the WCN boot handshake.
3. **WLAN command path** (`sprdwl`): scan/join, WPA2 four-way
   handshake, data path, then expose Ethernet to `/net`.

Phase 1 is the `sdprobe` program in `sys/src/cmd/sdio/sdprobe.c`.  It
powers the module, muxes and clocks the controller, resets it, and runs
CMD5/CMD3/CMD7 followed by CCCR/CIS dumps.

## Host driver notes learned the hard way

- **DW MSHC clock update needs START.**  Writing only
  `UPD_CLK|PRV_DAT_WAIT` leaves the CIU unclocked and no command ever
  completes.  U-Boot's `dwmci_control_clken()` uses
  `START|PRV_DAT|UPD_CLK` and waits for `CMD_DONE`.
- **The card clock is the CRU clock divided by two.**  Linux sets
  `clk_sdmmc1` to 750 kHz (mux 5 = `clk_osc0_div_750k`) and leaves
  `CLKDIV = 0`, so the bus runs at 375 kHz during enumeration.  The DW
  divider is not what sets the low init clock here.
- **CRU registers have a write mask in the high half.**  Plain writes
  to `SDMMC1_CON0/CON1` are ignored; use the hiword-mask form.
- **RK356x iomux packs a bank into 0x20 bytes.**  Groups are 8 bytes
  apart (`A_L, A_H, B_L, ...`), banks 1..4 live in 0x00-0x7f, pulls
  start at 0x80 and drive strength at 0x200.  The old `rk356x.h` macro
  used a 0x100 bank stride and 0x10 group stride, so every pinmux write
  above bank 0 landed in the wrong register; `gpiofs` and the GMAC reset
  were affected too.  Fixed in `sys/include/rockchip/rk356x.h`.
- Pulls: `GRF 0x80 + (bank-1)*0x10 + (pin/8)*4`, two bits per pin,
  1 = pull-up.  Drive: `GRF 0x200 + (bank-1)*0x40 + (pin/2)*4`.
- IO domain: `vccio3` (SDMMC1) is selected by `PMU_GRF_IO_VSEL0` bit 3
  = 1.8 V and `VSEL1` bit 3 = 3.3 V.  U-Boot leaves it at 3.3 V, which
  is what Linux enumerates with before switching to 1.8 V for SDR104.

## Current state

`sdprobe` brings the host up cleanly:

```
sdprobe: controller reset, verid 5342270a hcon 04c434c1 clkdiv 0
sdprobe: clksel30 00005520 ckgate15 00000000 pwren 1 rstn 1 clkena 00010001
sdprobe: cmd5 try 0: e=2 resp=00000000 rintsts=00000100 status=00000176
```

`e=2` is a response timeout: the controller transmits CMD5 but the card
does not answer.  Verified so far: WL_REG_ON is high, the pins are muxed
with pull-ups, `vccio3` is 3.3 V, the CRU gates are open, and the host
command path runs (a bad clock-update previously produced no completion
at all; that is fixed).

Next steps, in order:

1. Capture Linux's GRF/CRU/DW register values after enumeration and
   diff them against ours.  The initramfs has no `devmem`, so the plan
   is to inject a tiny static aarch64 register dumper into the Armbian
   initrd (or use `dd` on `/dev/mem` with a helper).
2. Check the DW `UHS_REG`/`ENABLE_SHIFT`/`CDTHRCTL` registers and the
   phase registers written with the CRU hiword mask (drive 90 degrees,
   sample 0).
3. Only then move on to the firmware download stage.
