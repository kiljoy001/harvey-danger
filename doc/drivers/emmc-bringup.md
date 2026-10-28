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

## Next steps

1. Instrument `PRESENT` bits 24-31 around `CMD0` to confirm the CMD line
   toggles at the pad.
2. Toggle `vcc_3v3`/`vcc_1v8` through `i2cfs` (RK809 `POWER_EN` at
   `0xb1`) to power-cycle the card, which is the one thing Linux does in
   `mmc_rescan` that a warm handover from U-Boot never does.
3. If the card still ignores `CMD0`, compare the eMMC pinmux
   (`GRF_GPIO4A/B/C_IOMUX`) against U-Boot's settings; `gpiofs` only
   writes the pin data register, but a stray pinmux write would explain a
   silent card exactly.
4. Cross-check with `bcm64/sdhc.c` from 9front (a working SDHCI driver on
   ARM) rather than continuing to derive the sequence from SDHCI alone.
