# UWE5622 WiFi/BT under LKL (Orange Pi 3B v1.1)

The v1.1 board carries a Unisoc UWE5622 (`CDW-20U5622`) module: WiFi and
Bluetooth both live behind one SDIO function on `sdmmc1` (DesignWare MSHC at
`0xfe2c0000`, GIC SPI 99), plus a TTY-over-SDIO Bluetooth channel.

LKL owns the bus, the SDIO stack and the driver.  The native side keeps the
board wiring: CRU gates and the card-clock mux, the SDIO pinmux, the module's
regulator-enable lines and the 32 kHz clock.

## Native handoff (`setwcnpower`, `sys/src/cmd/lkl/lklboot.c`)

| What | Register | Value |
| --- | --- | --- |
| `hclk_sdmmc1` / `clk_sdmmc1` gates | CRU `CLKGATE_CON(15)` = `0x33c` | `0x000c0000` (0 enables) |
| `sclk_sdmmc1` mux = `cpll_100m` | CRU `CLKSEL_CON(30)` = `0x178` | `0x70003000` (hiword mask `7`<<16, field 3) |
| SDIO pins `GPIO2_A3..A7`, `B0` | SYS_GRF `0x20/0x24/0x28` | function 1, full-field masks |
| `WL_REG_ON` = `GPIO0_D3` (pin 27) | PMU_GRF `0x18`, GPIO0 `0x0c`/`0x04` | mux 0 (GPIO), DDR_H bit 11, DR_H bit 11 |
| `BT_REG_ON` = `GPIO2_B7` (pin 15) | SYS_GRF `0x2c`, GPIO2 `0x08`/`0x00` | mux 0, DDR_L bit 15, DR_L bit 15 |
| 32 kHz to the module | RK809 `CLK32KOUT2` | `pmicfs -c` started by drvman |

Two register conventions bit us and are worth remembering:

* CRU/GRF/iomux uses a **write mask in the upper 16 bits** (`mask<<16 | value`).
  `CLKSEL_CON(30)` is `0x70003000`, not `0x00073000`.
* GPIO v2 writes take **value and write-enable in the same word**:
  `BIT(pin%16) | BIT(pin%16+16)`, so `WL_REG_ON` high is `0x08000800` at
  `DDR_H`/`DR_H`, and low is `0x08000000`.

The module's 32 kHz sleep clock is the RK809 `CLK32KOUT2` output, enabled
once at boot by `pmicfs -c` (drvman passes the flag); the driver does not
touch the PMIC.

`WL_REG_ON` is held low, then driven high, then the driver's 200 ms
post-power-on delay applies.  Without a real output direction the module never
answers CMD5 and `sdiohal_scan_card` times out after 6 s.

## Guest side

* `util/arm64/lkl-wifi.dts` is a trimmed tree: GIC, `mmc@fe2c0000` with fixed
  `biu`/`ciu` clocks at 100 MHz, fixed rails, `non-removable`, no card detect.
* `util/arm64/lkl-wifi.config` enables `CONFIG_MMC_DW`/`MMC_DW_ROCKCHIP`
  (the Kconfig dependencies were widened to include `LKL`), cfg80211/mac80211,
  the UWE5622 driver and both firmware blobs:
  `CONFIG_EXTRA_FIRMWARE="uwe5622/wcnmodem.bin uwe5622/wifi_2355b001_1ant.ini"`.
* The SDIO card enumerates at 50 MHz:
  `mmc0: new high speed SDIO card at address 8800`, chip id `0x2355b001`.
* Firmware version reported by CP2:
  `MARLIN3_19B_W21.05.3~sc2355_marlin3_lite_ott~12-15-2021`, MAC
  `e0:51:d8:67:51:92`, `wlan0` registered.

## LKL integration problems that had to be fixed

1. **Initcall ordering.**  `lkl_start_kernel()` returns only after every
   initcall has run (it waits for the guest to exec `/init`).  The WiFi probe
   waits seconds for the SDIO card, so if it runs from an initcall the host's
   interrupt forwarders do not exist yet and the card never answers.  The
   platform device is therefore registered from a delayed work on a dedicated
   workqueue (`unisoc_wlan`, not `events`, which the firmware download uses).
2. **Interrupt forwarding.**  `watchfamily()` starts one host thread per
   device IRQ; it reads `#P/<node>/wait` and calls `lkl_trigger_irq(131)`.
   It runs immediately after `lkl_start_kernel()` returns, and MMC card
   detection plus the deferred WiFi probe happen later, so the forwarders
   exist before either needs them.
3. **DMA coherency.**  The `dma` arena is mapped cacheable; drivers that use
   `dmam_alloc_coherent` (the IDMAC descriptor ring) assume hardware
   coherency.  `lkl_dma_alloc()` registers coherent ranges with the host and
   `hiomem_access()` cleans them before every device register write.  Without
   this the IDMAC never sees the descriptors and every CMD53 hangs.
4. **`readl()`/`writel()` on normal memory.**  LKL routes those through
   `iomem_access`; the DW MMC descriptor poll reads normal RAM.  The host
   falls back to a plain volatile access when the address is not a device
   window.
5. **`vmalloc` and DMA.**  The firmware image is copied out of the built-in
   blob with `vzalloc`; `hmmap` now allocates from the DMA arena so that image
   has a device-reachable physical address.
6. **No `CONFIG_PM_SLEEP`.**  `wakeup_source_register()` returns `NULL`; the
   driver treated that as fatal and left `cmd->init_ok = 0`, which silently
   dropped *every* command to the firmware (the sync-version reply then read
   uninitialised stack and the API map stayed all zero).  The driver now
   continues without a wakeup source.
7. **Firmware without INI download.**  This blob reports API version 0 for
   `WIFI_CMD_DOWNLOAD_INI`; the driver now skips the command and keeps the
   parsed board config on the host side.
8. **Built-in firmware is not heap memory.**  `release_firmware()` must not be
   called on the stack `struct firmware` used for `firmware_request_builtin()`
   (it `kfree()`s the pointer).

## Deploy

`lklboot` is ~19 MB stripped, too big for the main boot archive; it rides in
the second archive U-Boot loads at `0x50000000`:

```
tftpboot 0x40080000 aspera-arm64.img
tftpboot 0x50000000 lklboot.arch
tftpboot 0x48000000 orangepi-3b-v1.1.dtb
booti 0x40080000 - 0x48000000
```

The board DTB must have `mmc@fe2c0000` enabled so `#P` exposes the window.

Then `/boot/lklboot -b -f wifi`.  `-b` detaches with
`rfork(RFPROC|RFNOTEG)` *before* any LKL state exists, so the shell gets its
prompt back and the detached process owns the LKL CPU lock.  The service keeps
the process (and the guest) alive; a datapath from the guest netdev to Harvey
and BT HCI are next.

## Diagnostics and the console

The console is not part of the driver path:

* `lklboot` opens `/dev/lkllog` (a ring-buffer file) and sends its own
  messages there, and LKL's `print` op is redirected to the same fd, so guest
  `printk` never touches the UART.
* Host-side setup messages in `harvey-host.c` (`say`/`lkl_printf`) go to that
  fd as well.
* The kernel's old `mmuput dma ...` trace is gone; a normal start produces no
  console output at all.
* The only console output left is a genuine Harvey fault/panic dump, which
  means the guest process died.

Read the log with `cat /dev/lkllog`.  Interrupt forwarding uses `pread` at
offset 0 on `#P/<node>/wait`: a plain `read` advances the file offset, hits
EOF after the first interrupt and forces a reopen, which flaps the GIC line
and intermittently breaks SDIO enumeration.  `pnpclose` now releases the line
and a second opener is refused, so a leaked watcher can no longer corrupt the
wait queue.
