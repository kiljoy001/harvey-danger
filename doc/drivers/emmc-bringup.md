# eMMC bring-up on the Orange Pi 3B (rk3566)

Controller: `mmc@fe310000`, `rockchip,rk3568-dwcmshc`, SDHCI-compatible.
Driver: `sys/src/cmd/emmc/emmc.c` (`/boot/emmc`), user space over the
`#P` grant at `fe200000 + 0x110000` (the grant covers the containing
2 MiB block, so the driver adds the in-block offset).

Status: controller fully usable, **card never answers**.  The same card
reads and writes from U-Boot in the same power cycle.

## Comparison with the vendor kernel (`9rock/linux-rockchip`)

### Power

`mmc@fe310000` in the board DTS:

| Supply | Node | Phandle | Flags |
| --- | --- | --- | --- |
| `vmmc-supply` | `SWITCH_REG1` ("vcc_3v3") | 0x1c | always-on, boot-on |
| `vqmmc-supply` | `DCDC_REG5` ("vcc_1v8") | 0x1a | always-on, boot-on |

Both rails are always on, so the card is powered regardless of driver
state.  PMU `0xa0` reads `0x3` in U-Boot and in Harvey, so no power
domain differs.  Setting `PMU+0xa0` bit 8 wedges the board, which means
that bit is a power-*down*; `usbpower` clearing it is correct.

### Clocks (CRU `fdd20000`)

| Register | U-Boot | Harvey |
| --- | --- | --- |
| `CLKGATE_CON(9)` `0x324` (ACLK/HCLK/BCLK/CCLK/TCLK EMMC, bits 5-9) | `0` (all enabled) | `0` |
| `CLKSEL_CON(28)` `0x170` (BCLK bits 8-9, CCLK bits 12-14) | `0x1030` | `0x1030` |
| `EMMC_CON0` `0x598` (drive clock) | `0x4` | untouched |
| `EMMC_CON1` `0x59c` (sample clock) | `0x0` | untouched |

`CLKSEL_CON(28)` bit 12 set selects `gpll_200m` for `cclk_emmc`, matching
the DTS `assigned-clock-rates = <200000000, 24000000>`.

### SDHCI programming

`CAPABILITIES = 0x226dc881`: base clock 200 MHz, **no programmable clock
mode** (bit 20 clear), so `sdhci_calc_clk()` takes the SDHCI 3.00 path:
divisors are even, `real_div = div`, register field `div/2`.  For 400 kHz
that is `div = 500`, field `250` — what the driver writes.

Vendor specifics replicated from `sdhci-of-dwcmshc.c`:

- `vendor_specific_area1 = readl(0xe8) & 0xfff` → `0x500`;
- `MISC_INTCLK_EN` (bit 1 of `0x81C`) set after reset
  (`rk35xx_sdhci_reset()` does exactly this);
- command conflict check cleared at `area1 + 0x8` (`dwcmshc_rk35xx_init`);
- DLL bypass for `clock <= 52 MHz`: `DLL_CTRL = 0` then
  `BYPASS|START`, `DLL_RXCLK = BIT(31)`, `DLL_TXCLK = 0`,
  `DLL_CMDOUT = 0`, `DLL_STRBIN = BIT(27)|BIT(26)|0x16<<16`;
- the vendor `set_clock` order is clock first, then output clock off,
  DLL config, output clock on — the driver now matches.

`sdhci_enable_clk()` sets `INT_EN`, waits for `INT_STABLE`, then sets
`CARD_EN`; the driver does the same.

### What the comparison rules out

- card power (always-on rails, identical PMU state);
- the CRU clock gates and mux (identical values);
- the divider calculation (matches `sdhci_calc_clk` for spec 3.00);
- the vendor init sequence (all registers replicated);
- the display bring-up (`ARM64_NO_DISPLAY=1` changes nothing).

## Observed failure

- `CMD0` completes (`NIS.CC`), so the command path itself works.
- `CMD1` (both OCR windows, 150 polls), `CMD2`, and `CMD13` for RCA 1-4
  all end with `NIS = 0x8001`, `EIS = 0x1` — *command timeout*: the
  controller transmitted and waited for a response that never came.
- Same at 400 kHz and at U-Boot's 200 MHz with the DLL state untouched.
- A full `SWRST_ALL` makes even `CMD0` stop completing, so the driver
  deliberately leaves the controller as the bootloader configured it.

Since commands are transmitted (`PRESENT` shows the CMD line active) and
the card is powered, the card is not receiving the command.  That leaves
the pin path or the card's own state, neither of which the vendor source
comparison can settle.

## Faithful port of the vendor sequence

The driver now follows the kernel call order rather than an approximation:

| Kernel function | What the driver does |
| --- | --- |
| `dwcmshc_probe()` -> `dwcmshc_rk35xx_init()` | `area1 = readl(0xE8) & 0xfff` (0x500); `writel(0, area1+0x8)`; `writel(0, DLL_TXCLK)`; `writel(0, DLL_STRBIN)` |
| `mmc_power_off()` -> `sdhci_reinit()` -> `sdhci_reset(ALL)` | `writeb(4, SWRST)`, poll up to 100 ms |
| `rk35xx_sdhci_reset()` | `writel(MISC_INTCLK_EN | extra, MISC_CON)` with `extra` read *before* the reset |
| `mmc_set_initial_signal_voltage()` -> `sdhci_voltage_switch(1.8V)` | `HOST_CONTROL2 |= VDD_180`, then 5 ms |
| `sdhci_set_ios()` -> `sdhci_set_clock()` | divider 250, `INT_EN`, wait `INT_STABLE`, DLL bypass (<= 52 MHz), `CARD_EN` |
| `sdhci_set_power_noreg()` | `POWER = 0`, then `pwr | SDHCI_POWER_ON` |
| `sdhci_set_bus_width(1)` | clear 4-bit and 8-bit select in `HOST_CONTROL` |
| `sdhci_send_command()` | `ARG`, then `CMD` with response type 2 / CRC / index check |

`SDHCI_QUIRK_BROKEN_TIMEOUT_VAL` is honoured by not programming
`TIMEOUT_CONTROL`.

VCC comes from `vmmc` (`vcc_3v3`) and IO signalling from `vqmmc`
(`vcc_1v8`), so the correct combination is `POWER = 3.3 V` with
`HOST_CONTROL2.VDD_180 = 1`.  All four combinations were tried in one
boot (`/boot/emmc matrix`): each gives `CMD0` complete, then `CMD1`
never completes and leaves `PRESENT.CMD_INHIBIT` set.

Result: the register-level port now matches the vendor driver, and the
card still does not answer.  The remaining difference is not in the
SDHCI register set.

## Two non-obvious details from the vendor code

1. `SDHCI_QUIRK2_CLOCK_DIV_ZERO_BROKEN` is set in
   `sdhci_dwcmshc_rk35xx_pdata`.  With the core clock at 375 kHz,
   `sdhci_calc_clk()` computes `div = 0`, and the quirk forces it to 1.
   Writing 0 to the divider field leaves the card without a usable clock.
2. `sdhci_setup_host()` calls `sdhci_set_clock(host, host->max_clk)` at
   probe, i.e. **200 MHz through the vendor DLL branch** (DLL reset,
   start, wait for lock, `ATCTRL`, RX/TX taps) before any card command.
   Identify mode later bypasses the DLL, but the PHY has been through
   that initialisation.  The driver now does both
   (`cru_emmc_200m()` + `clock200()` + `dllstart()`, then 375 kHz).

Both are now implemented and the DLL locks (`dll lock 0x78`,
`rxclk 0x8a007800`), and the card still does not answer.

## State comparison with U-Boot (same power cycle)

| Item | U-Boot | Harvey |
| --- | --- | --- |
| GRF pinmux `fdc60000` (first 0x34 bytes) | `...1111 1111 2111 1111 1111 0111` | identical |
| `PRESENT_STATE` at handover | - | `0x3f700f0` |
| `CLOCK_CONTROL` / `POWER` / `HOST_CONTROL2` | 7 / 0xb / 0x9b | same |
| RSA sweep after U-Boot selected the card (`mmc read`) | - | **no RCA 1-255 answers** |

That last row is the strongest evidence: immediately after U-Boot read
the card, Harvey sends CMD13 for every possible RCA and gets no response.
The controller transmits (CMD line active) but the card never replies, so
the card is not receiving Harvey's commands.

## PMIC lead (unconfirmed)

RK809 regulator enables use a bit + write-mask layout:
`ENABLE_MASK(id) = BIT(id) | BIT(4+id)`.

`POWER_EN0` (`0xb1`) reads `0x0f`: DCDC1-DCDC4 enabled.  The eMMC's
`vqmmc` is `DCDC_REG5` ("vcc_1v8"), whose enable is
`RK817_POWER_EN_REG(3)` = **`0xb4`**, mask `BIT(1)|BIT(5)` (write `0x22`).

Reading `0xb4` over i2c fails ("read failed") — the RK809 exposes some
registers through a page register, so the state of `vcc_1v8` is still
unconfirmed.  If it is off, the eMMC IO rail is unpowered and that alone
explains the silence.

## Boot isolation tests

| Build | Result |
| --- | --- |
| `ARM64_NO_DISPLAY=1` (no `vop2init`) | unchanged: CMD0 completes, CMD1 silent |
| `ARM64_NO_USBPOWER=1` (stub `usbpower`, no PMU/CRU writes) | unchanged |

So neither the display bring-up nor `usbpower`'s PMU/CRU programming is
responsible.

## Findings from the Armbian source

`9rock/linux-rockchip` is `armbian/linux-rockchip` on branch `rk-6.1-rkr5.1`,
but the booted image is **mainline 6.18.21** (`current-rockchip64`).  Three
things exist only in the newer driver or in the creator's board DTS:

1. **`dwcmshc_phy_init()`** (6.18 only), installed as the `voltage_switch`
   op: deasserts `PHY_CNFG_RSTN` and configures the PHY pads, delay line and
   DLL at `DWC_MSHC_PTR_PHY_R = 0x300`.  Implemented; on this controller
   every register in that block reads back 0 (U-Boot too), so it is a no-op
   here.
2. **`SDHCI_QUIRK2_CLOCK_DIV_ZERO_BROKEN`** — identify mode gives div 0,
   the quirk forces 1.  Implemented (`CLOCK_CONTROL = 0x107`).
3. **The creator's board DTS gives the eMMC no `vmmc`/`vqmmc` and no
   `mmc-hs200-1_8v`**, so there is no 1.8 V voltage switch: the card is
   identified with 3.3 V signalling.  Our v1.1 DTB declares both, which is
   why we had been switching to 1.8 V.  Tried both; no change.
4. **Five controller resets** (`SRST_A/H/B/C/T_EMMC`, ids 117-121, all in
   CRU `SOFTRST_CON(7)` = `0x41c` bits 5-9).  Our DTB has no `resets`
   property at all.  They read back 0 (deasserted) in U-Boot and Harvey;
   pulsing them makes even CMD0 stop completing, so they are left alone.
5. The 375 kHz parent `clk_osc0_div_375k` is `clk_osc0_div_750k / 2`, and
   the 750 kHz divider lives in `CLKSEL_CON(82)` bits 8-13.  Programmed to
   /32 (`0x1f09`) on the assumption the framework would do it; no change.

## Next steps

1. Read `0xb4` through the RK809 page mechanism (`rk808.c` regmap
   config) and confirm `vcc_1v8`/DCDC5; enable it with a `0x22` write if
   it is off.
2. Toggle `vcc_3v3`/`vcc_1v8` through `i2cfs` (RK809 `POWER_EN` at
   `0xb1`) to power-cycle the card, which is the one thing Linux does in
   `mmc_rescan` that a warm handover from U-Boot never does.
3. If the card still ignores `CMD0`, compare the eMMC pinmux
   (`GRF_GPIO4A/B/C_IOMUX`) against U-Boot's settings; `gpiofs` only
   writes the pin data register, but a stray pinmux write would explain a
   silent card exactly.
4. Cross-check with `bcm64/sdhc.c` from 9front (a working SDHCI driver on
   ARM) rather than continuing to derive the sequence from SDHCI alone.

## Live capture from the working 6.18 kernel

Booting the Armbian kernel (6.18.21) under this same U-Boot and sampling
the controller with a small static `/dev/mem` tool gives the reference
identify state:

| register | working value |
|---|---|
| `CLKSEL_CON(28)` (`0x170`) | `0x00005030` (cclk_emmc mux 5) |
| `CLKSEL_CON(82)` (`0x248`) | `0x00001f09` (750 kHz parent) |
| `CLKGATE_CON(9)` (`0x324`) | `0x0000001f` |
| `CLKGATE_CON(35)` (`0x38c`) | `0x00002000` |
| `SDHCI_CLOCK_CONTROL` | `0x00000b47` (divider 267) |
| `SDHCI_HOST_CONTROL2` | `0x0008` (1.8 V signalling, UHS 0) |
| `SDHCI_POWER_CONTROL` | `0x01` (power on, no voltage field) |
| `EMMC_CON0/1` | `4` / `0` |
| `PLL0` | `2044 1441 0 7` |
| `GRF` mux/pull/drive, GPIO, PMU | identical to U-Boot |

The card is identified with **1.8 V signalling and the 1.8 V OCR bit**;
the SDHCI divider is 267 (702 Hz), not 1.  Note `dwcmshc_phy_init()` is
only installed in the TH1520 ops table, not RK3568, so the PHY block
(which reads zero) is not part of this path.

## Reproducing the sequence on the working kernel

A second static tool (`emul`) replays the Harvey sequence from
userspace under the Armbian kernel with its driver still bound:

```
emul: state clock=00000b47 pwr=00000001 hc2=00000008 sel28=00005030
cmd1_18=00000000 ocr18=00000900
```

So the register sequence itself is correct on this hardware.

## Result in Harvey

The same sequence (and even U-Boot's untouched state) gets no response:
`CMD0` completes, but `CMD1`, `CMD2` and `CMD13` all end in
`eint=0x0001` (command timeout) with no response.  This is true:

- with the untouched U-Boot state (no reset, no clock change),
- with the full 6.18 sequence (`slow8`),
- after a real PMIC rail power-cycle (`POWER_EN3 0x0f -> 0x09 -> 0x0f`),
- with `ARM64_NO_DISPLAY=1` and `ARM64_NO_USBPOWER=1`,
- and from the init program **before any userland service starts**.

Every software-visible block compared against U-Boot/Linux is identical:
SDHCI, DLL, vendor area, CRU/PLLs, `EMMC_CON`, GRF (mux, pull, drive),
GPIO, PMU, and the PMIC enable state.  The remaining difference is
outside that state: the card clock/pad or the card's reset line.

## Bugs fixed along the way

- `i2cfs` never cleared `CON.START` when issuing `CON.STOP`; PMIC writes
  failed and left the bus wedged.  With that fixed, `rtc poweroff` and
  the RK809 rail writes work, and `POWER_EN3` reads back `0x0f`.
- The driver enabled the interface clock before writing the power
  register; that clears `CLOCK_CARD_EN`.  Power, width, 1.8 V signalling
  and clock now follow `sdhci_set_ios()` order.
- RK3568 GRF registers are hiword-masked; writes without the mask are
  silently ignored.
- `initcard()` now uses the captured 6.18 identify values.

## Remaining next step

Instrument the kernel before `#P` initialisation, or scope `SDMMC0_CLK`
and `SDMMC0_CMD` during a Harvey boot.  The card answers U-Boot and the
Armbian kernel in the same power cycle, and stops answering somewhere in
Harvey's early boot despite the identical register state.

## Root cause: the SDHCI command and transfer encoding

The card was working the whole time; every command was being addressed to
the wrong index.

1. **Command register**: the command index is bits 8-15 and the flags are
   bits 0-5 (`SDHCI_MAKE_CMD`).  We wrote `idx | flags`, so CMD1 went out
   as CMD4, CMD2 as CMD2 with a 48-bit response, CMD13 as CMD5, and so
   on.  Fix: `(idx << 8) | flags`, with `CmdData=1<<5`, `CmdIdx=1<<4`,
   `CmdCrc=1<<3`, `Rsp136/Rsp48/Rsp48b = 1/2/3`.
2. **Transfer mode**: the data direction is bit 4 (`SDHCI_TRNS_READ`),
   not bit 3 (that is Auto CMD23).  `readblock` set `0x08|0x02`, which the
   controller treated as a write, so the card waited for data while the
   host waited for data.  Fix: read `1<<4`, write `0`, clear bits 2-3.
3. **PIO waits**: poll Present State bit 10 (buffer write enable) and
   bit 11 (buffer read enable), as Linux does, rather than the NIS
   Bwr/Brr bits.

Also restore the working init order (power, width, 1.8 V, clock), set
`Timeout=0x0e` after reset, poll CMD1 until OCR bit 31 (ready), and use
the 1.8 V OCR.

Verified on the board:

```
emmc: cid 9010f0ba 363411fc 41334135 00d62903   (serial fc9010f0)
emmc: csd ef964000 ffffffff 329f5903 00d0ffff
emmc: wrote lba 1000
emmc: cmd17 ok r1 00000900
emmc: readback MATCH
```

Transfers still run at the 702 Hz identify clock (~6 s per 512-byte
block).  Raising the clock needs the DLL/PHY sampling work: 25 MHz gives
a Data End Bit Error and 5 MHz a Command End Bit Error with the bypassed
DLL.

## Working driver: 50 MHz, 8-bit

After the command/transfer encoding fixes the vendor clock model is what
gets the card to full speed: `cclk_emmc` is set to the target rate by the
CRU and the SDHCI divider stays **0**, so the card sees `cclk_emmc`
directly (the vendor driver does `sdhci_enable_clk(host, 0)` at the end
of `set_clock`, which overwrites the computed divider with 0).

Sequence after CMD16:

1. CMD6 `0x03B90100` — HS_TIMING = 1.
2. `CLKSEL_CON(28)` mux -> `cpll_50m` (4), divider 0, gate 9 bit 8.
   `xin24m` (mux 0, 24 MHz) is the fallback.
3. CMD6 `0x03B70200` — BUS_WIDTH = 8, then HC1 bit 5.

Verified on the board:

```
emmc: 50m sel 4030 clock 7
emmc: hostctl 24
emmc: wrote lba 4000
emmc: cmd17 ok r1 00000900
emmc: readback MATCH
```

That is 50 MHz x 8-bit, about 50 MB/s theoretical.

## HS200

After the 50 MHz / 8-bit switch:

1. CMD6 `0x03B90200` — HS_TIMING = 2.
2. `CLKSEL_CON(28)` mux -> `gpll_200m` (1), SDHCI divider 0.
3. DWCMSHC DLL init as in the 6.18 driver: reset, `0x800 = 0x00050201`,
   wait for `0x840` lock, `ATCTRL (0x540) = 0x001f0000`,
   `RXCLK = 0xaa007800` (DLYENA|ORI_GATE|NO_INVERTER|TAP_SEL|lock<<8),
   `TXCLK = 0x29000010`, `STRBIN = 0x09000004`.
4. `HC2 = SDR104 | 1.8 V` (0x0b).
5. SDHCI execute tuning: set `EXEC_TUNING`, issue CMD21 with a 128-byte
   tuning block until the hardware clears `EXEC_TUNING` and sets
   `TUNED_CLK` (HC2 becomes 0x8b).

Verified:

```
emmc: hs200 dll 13c rx aa007800 tx 29000010 hc2 b
emmc: tuning ok hc2 8b
emmc: wrote lba 5000
emmc: cmd17 ok r1 00000900
emmc: readback MATCH
```

That is 200 MHz x 8-bit HS200, the card's full rated mode.

## ADMA2

PIO through the buffer data port is about 4 MB/s: each 4-byte access to
the Device-mapped port costs about a microsecond.  Data now moves by
ADMA2 out of the `#P/dma` region, which is physically contiguous.

The register layout came from the working Linux driver, booted from the
same U-Boot (Armbian 6.18.21, `break=premount`) and read live:

| Register | Linux | Meaning |
| --- | --- | --- |
| `HC1` `0x28` | `0x34` | ADMA2 32-bit + high speed + 8-bit |
| `0x00` | `0` idle, block count during a transfer | **not** the DMA address |
| `0x58` | `0x02cc0208` (a descriptor address) | ADMA system address |
| `HC2` `0x3e` | `0x8b` | SDR104 + 1.8 V |

The controller is in SDHCI v4 mode even though `HC2` bit 12 reads back
clear: `0x00` behaves as Argument 2 / the 32-bit block count, and the
ADMA address lives at **`0x58`**.  Writing the descriptor address to
`0x00` (the physical-spec-3.0 location) makes the DMA engine never start:
the command completes, `DMA End` (NIS bit 3) may even be set, but
`Transfer Complete` (NIS bit 1) never arrives and the data path stays
active.  `dmesg` on the same board confirms `sdhci ... using ADMA`.

Descriptor table entries are the standard 8-byte ADMA2 32-bit form:

```
u16 cmd   0x21 = Valid|Transfer, | 0x02 = End on the last entry
u16 len   bytes, up to 32 KiB per entry in this driver
u32 addr  physical address
```

Completion is `NIS` bit 1 (Transfer Complete); the transfers are short
enough that the loop should busy-poll, because one `sleep(1)` costs 10 ms
and dominates a 64 KiB transfer.

The `#P/dma` physseg is mapped **cached**.  Driver buffers are touched by
the CPU, so the mapping has to be normal write-back, and an uncached
64 KiB `memmove` alone costs 3.5 ms.  The driver maintains it explicitly:

- write: `memmove` into the buffer, `#P/dma/clean`, then DMA;
- read: `#P/dma/cleaninval`, DMA, `#P/dma/inval`, then `memmove`;
- the descriptor table is also cleaned before the transfer (a cached
  descriptor is invisible to the DMA engine).

Verified on the board (64 KiB, 128 blocks):

```
emmc: wrote lba 9000 count 128
emmc: readback MATCH
emmc: read 128 blocks in 822500 ns: 77811 KiB/s
emmc: write 128 blocks in 563791 ns: 113517 KiB/s
```

About 78 MB/s read and 114 MB/s write, against 4 MB/s with PIO.

## 9P: emmcfs

`sys/src/cmd/emmcfs/emmcfs.c` serves the card over 9P.  The driver core
is `../emmc/emmc.c`, built a second time with `-DEMMCFS_LIB` so the CLI
and the server share one implementation.  The server posts `/srv/emmc`
and is started by init:

```
$ cat /mnt/emmc/ctl
emmc rk3566
mode hs200 200MHz 8bit
blocks 488554496
blocksize 512
bytes 250139901952
```

| File | Mode | Meaning |
| --- | --- | --- |
| `ctl` | 0444 | negotiated mode, geometry and I/O counters |
| `data` | 0666 | the raw device, byte addressable |
| `flush` | 0222 | any write runs EXT_CSD FLUSH_CACHE (CMD6, R1b) |

The controller has one command queue, so all transfers are serialised by
a single `QLock`.  A request that is not block aligned becomes a
read-modify-write through a 64 KiB bounce buffer; aligned requests go
straight from the client's buffer.

Init runs a self-test before the shell: mount, read `/ctl`, write and
read back eight blocks at LBA 18000, then overwrite 300 bytes inside
block 17000 and check that the bytes on both sides are preserved.

```
emmcfs: hs200 200MHz 8bit, 488554496 blocks
HARVEY-ARM64 EMMCFS PASS
```

The server advertises `msize = 128 KiB`, the kernel's `MAXRPC`, so one
9P message can carry 64 KiB of payload.  Measured on the board with
`/boot/emmc 9pbench`:

```
emmc: 9p write 4 MiB in 147253458 ns: 27815 KiB/s
emmc: 9p read   4 MiB in  89715792 ns: 45655 KiB/s
emmc: 9p read  32 MiB in 604194791 ns: 54234 KiB/s     (read-only)
```

Sustained writes beyond a few MiB drop to a few MB/s: that is the card's
own SLC cache running out and garbage collection taking over, not the
transport (read-only runs stay above 50 MB/s).

The server owns the controller while it runs, so the `/boot/emmc` CLI
must not be used at the same time (it re-initialises the card).

## Partitions and the ext2 root

The card is 233 GiB.  Its user area carries a Rockchip IDB at sector
`0x40` (`RKNS`) and no DOS partition table, so the master boot sector
was free.  The layout now in use:

```
sector 0        MBR, one partition, type 0x83
sector 0x40     Rockchip IDB (untouched)
sector 0x8000   partition 1: 128 MiB ext2, label harvey-root
```

`emmcfs` reads the MBR once at startup and exports each used entry as
`/partN` next to `/data`, so a filesystem server never has to know
about offsets:

```
emmcfs: hs200 200MHz 8bit, 488554496 blocks, 1 partitions
emmcfs: part1 type 0x83 offset 16777216 bytes 134217728
HARVEY-ARM64 EMMCFS PASS
HARVEY-ARM64 EMMC-ROOT PASS
```

`ext2srv` is built for ARM64 and init starts it with
`-F -f /mnt/emmc/part1 ext2`, then mounts `/srv/ext2` at
`/mnt/emmcroot`.  The `-F` flag is a bring-up allowance: ext2srv clears
`EXT2_VALID_FS` while it has the filesystem open and only restores it
when the last reference goes away, so a board that was power-cycled
without unmounting comes back dirty.

The image itself is built on the host from the boot archive: `mke2fs
-t ext2 -b 1024 -O ^dir_index,^resize_inode,^ext_attr`, then every blob
from `userland.img` is written to `/bin` with `debugfs`.

Note that U-Boot must load **both** the kernel and the boot archive:

```
tftpboot 0x40080000 harvey.img
tftpboot 0x50000000 userland.img
tftpboot 0x48000000 orangepi-3b-v1.1.dtb
booti 0x40080000 - 0x48000000
```

Without the archive at `0x50000000` the kernel falls back to the
programs embedded in its image, which is an older and much smaller set:
`/boot/emmcfs` is missing and the eMMC test reports `EXEC FAIL`.

### Writing the images to the card

From U-Boot, with the images on the TFTP server:

```
tftpboot 0x10000000 mbr.bin
mmc dev 0
mmc write 0x10000000 0 1
tftpboot 0x10000000 rootfs.ext2
mmc write 0x10000000 0x8000 0x40000
```

U-Boot parses numbers as hex, and `mmc dev 0` is the eMMC
(`mmc@fe310000`); `mmc dev 1` is the SD slot.
