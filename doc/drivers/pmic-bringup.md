# RK809 PMIC service (rk809-pmic)

Status: **partial** — regulators, RTC (read and write), the CLKOUT2
32 kHz clock, interrupt status, the power key and raw register access
work on the Orange Pi 3B v1.1.  A long press on the power key powers
the board off.  Thermal shutdown is not acted on.

## Levels

| Level | Component |
| --- | --- |
| L2 bus | `i2cfs` owns the RK356x I2C controllers and serves `/srv/i2c` |
| L3 device | `pmicfs` drives the RK809 at `0x20` on `i2c0` |
| L4 service | `/srv/pmic/{status,regs,rtc,ctl}` |

`pmicfs` never touches `#P` or MMIO.  It takes a private namespace
(`rfork(RFNAMEG)`), mounts `/srv/i2c` at `/n`, and speaks the i2cfs
text protocol:

```text
r <addr> <reg> <count>     read count registers
w <addr> <reg> <b0> ...    write bytes starting at reg
```

That keeps the bus engine in one place and makes the PMIC driver a pure
device driver.

## Device

```text
compatible  rockchip,rk809
i2c bus     i2c0, address 0x20
IRQ         GPIO0_A3, level low (not used by this driver)
clocks      MCLK in, CLKOUT1/CLKOUT2 out
```

The RK809 owns the SoC rails, the RTC and the 32 kHz clock that the
UWE5622 WiFi module wants.

## Registers used

```text
0x00..0x06   RTC seconds, minutes, hours, days, months, years, weeks (BCD)
0xf0         system status
0xb1..0xb4   power enable 0..3
0xba..0xc5   buck 1..4 config / on-vsel / sleep-vsel
0xb9         power config (bit 6 = buck3 internal feedback)
0xcc..0xdd   LDO 1..9 on-vsel
0xde         buck 5 (vcc_1v8) voltage
0xed/0xee    chip ID (reads 0x80/0x95 on this board = RK809 rev 5)
0xf0         system status
0xf2         SYS_CFG1: CLKOUT2 enable (bit 7), hot-die threshold
             (bits 5:4), thermal shutdown 140/160 C (bit 6)
0xf5/0xf6    on/off source (power key, RTC alarm, ...)
```

Voltage encodings:

- buck 1/2: selector 0..80 = 500 mV + 12.5 mV steps; 81.. = 1600 mV +
  100 mV steps (to 2.4 V);
- buck 3: same table, and the output is only programmable when
  `POWER_CONFIG` bit 6 (internal feedback) is set — on this board it is
  not, so `vcc_ddr` is fixed by external resistors;
- buck 4: same as buck 1/2 but up to 3.4 V;
- buck 5: 8-entry table 1500/1800/2000/2200/2800/3000/3300/3600 mV;
- LDO 1..9: 600 mV + 25 mV steps.

## Board rails

From the v1.1 device tree (`rk3566-orangepi-3b.dtsi`):

| rail | source | limits |
| --- | --- | --- |
| `vdd_logic` | DCDC1 | 500–1350 mV, settable |
| `vdd_gpu` | DCDC2 | 500–1350 mV, settable |
| `vcc_ddr` | DCDC3 | fixed by external feedback |
| `vdd_npu` | DCDC4 | 500–1350 mV, settable |
| `vcc_1v8` | DCDC5 | 1800 mV fixed |
| `vdda0v9_image` | LDO1 | 900 mV fixed |
| `vdda_0v9` | LDO2 | 900 mV fixed |
| `vdda0v9_pmu` | LDO3 | 900 mV fixed |
| `vccio_acodec` | LDO4 | 3300 mV fixed |
| `vccio_sd` | LDO5 | 1800–3300 mV, settable |
| `vcc3v3_pmu` | LDO6 | 3300 mV fixed |
| `vcca_1v8` | LDO7 | 1800 mV fixed |
| `vcca1v8_pmu` | LDO8 | 1800 mV fixed |
| `vcca1v8_image` | LDO9 | 1800 mV fixed |
| `vcc_3v3` | SWITCH1 | switch, no soft control |
| `vcc3v3_pcie` | SWITCH2 | switch, no soft control |

CLKOUT2 is the 32.768 kHz clock.  The board's `sdio_pwrseq` consumes it
as `ext_clock` (`clocks = <&rk809 1>`), i.e. it is the UWE5622 WiFi/BT
low-power clock.  It is **on at reset** (`0xf2 = 0x80`) and the driver
can toggle it; turning it off while the WiFi module is powered would
stop that clock.

`set` refuses anything outside the table's range and refuses fixed
rails, so a typo cannot move `vdd_logic` outside the SoC spec.

## Interface

```text
/srv/pmic/status   rail table with mV, on/off and selector; RTC; sys status
/srv/pmic/regs     hex dump of 0x00..0xff
/srv/pmic/rtc      "YYYY-MM-DD HH:MM:SS" (read or write)
/srv/pmic/ctl      commands, last result readable from the same file
```

`/ctl` commands (a command may arrive in several writes; it runs when
the newline arrives):

```text
r <reg> [count]        read registers
w <reg> <byte> ...     write bytes
on <rail>              set the enable bit
off <rail>             clear the enable bit
set <rail> <mV>        programme the selector, range-checked
clk32k [on|off]        read or set the CLKOUT2 32 kHz enable
```

`pmicfs -c` enables CLKOUT2 before serving; `pmicfs -p` probes and
prints the status without posting the service.

Both `/ctl` and `/rtc` buffer writes until a newline arrives: the shell's
`echo` sends each argument as its own Twrite, and a command has to run
once, not once per fragment.

Example session:

```text
% /boot/pmicfs &
pmicfs: rk809 on i2c0
% mount /srv/pmic /n
% cat /n/rtc
2017-08-04 09:02:29
% echo 'r 0xba 12' >/n/ctl ; cat /n/ctl
r 0xba: 64 20 20 64 20 20 64 00 00 64 00 00
% echo 'set vdd_npu 800' >/n/ctl ; cat /n/ctl
vdd_npu -> 800 mV (vsel 0x18)
% echo 'set vdd_npu 2000' >/n/ctl ; cat /n/ctl
vdd_npu range is 500..1350 mV
% echo 'clk32k off' >/n/ctl ; cat /n/ctl
clk32k off
% echo 'r 0xf2 1' >/n/ctl ; cat /n/ctl
r 0xf2: 00
% echo 'clk32k on' >/n/ctl ; cat /n/ctl
clk32k on
% echo 'r 0xf2 1' >/n/ctl ; cat /n/ctl
r 0xf2: 80
% echo 2026-10-31 12:34:56 >/n/rtc
% cat /n/rtc
2026-10-31 12:34:58
```

## RTC details

Reading pulses `RTC_CTRL` (0x0d) bit 6 (`GET_TIME`) to latch the shadowed
registers, then clears it; writing stops the counter (bit 0), writes
0x00..0x06, and restarts it.  The RK8xx calendar gives November 31 days,
so Linux stores dates shifted by one day per year since 2016 plus one in
December; `pmicfs` applies the same transform, which keeps the RTC
consistent when the same board also boots Linux.  Writing the raw
registers while the counter is running does not work — only the date
part sticks — so the stop/start sequence is required.

## Measured on the board (v1.1)

```text
rk809 addr 0x20
vdd_logic         900 mV on (vsel 0x20)
vdd_gpu           900 mV on (vsel 0x20)
vcc_ddr          fixed (external fb) on (vsel 0x00)
vdd_npu           800 mV off (vsel 0x18)     after 'set vdd_npu 800'
vcc_1v8          1800 mV on (vsel 0x09)
vdda0v9_image     600 mV off (vsel 0x00)
vdda_0v9          900 mV on (vsel 0x0c)
vdda0v9_pmu       900 mV on (vsel 0x0c)
vccio_acodec     3300 mV on (vsel 0x6c)
vccio_sd         3300 mV on (vsel 0x6c)
vcc3v3_pmu       3300 mV on (vsel 0x6c)
vcca_1v8         1800 mV on (vsel 0x30)
vcca1v8_pmu      1800 mV on (vsel 0x30)
vcca1v8_image     600 mV off (vsel 0x00)
vcc_3v3          switch on
vcc3v3_pcie      switch on
rtc 2017-08-05 09:02:09
sys status 0x82
chip id 8095
clkout2 32kHz on, hotdie 85 C, tsd 140 C
power source on 0x40 off 0x00
latched irq: plug-in plug-out gate-gpio classd-mute chg-bat-hi
```

After `irq clear` the last line reads `latched irq: none`.  `0xf5 = 0x40`
is the power-on source latched at reset (the vendor driver only logs the
raw byte; the board powered up from the DC plug).  The four IRQ sources
seen at boot are charger/no-battery artifacts, not power-key events.

`0xb1 = 0x07` matches the enable bits: logic, GPU and DDR on, NPU off.
The RTC is running but has lost its date (the coin cell is absent or
flat), so it reads the reset date.

## Power key

The board's button drives the RK809 PWRON input (not the SoC reset
line).  A press latches these bits in `INT_STS0` (0xf8):

```text
bit 0  pwrkey-fall   key down edge
bit 1  pwrkey-rise   key up edge
bit 2  pwrkey        short-press detect (not seen on this board)
bit 3  pwrkey-long   long-press detect
```

A held press was measured as `fall rise long`; the plain `press` bit
never set.  The bits are read-only status and are acknowledged by
writing 1s to them.

`pmicfs` runs a small watcher process (forked after the 9P service,
with its own namespace and fds).  It first tries the real interrupt
line: `gpiofs` exposes `/gpioN/wait`, which blocks on that bank's GIC
line (`#P/<bank>/wait`, IRQ 65 for gpio0), and the PMIC INT pin is
GPIO0_A3.  The watcher configures `irq 3 low` and sleeps in that read.
If gpiofs is not available it falls back to polling `INT_STS0` every
200 ms.  Either way it **powers the board off on a long press** by
setting `DEV_OFF`:

```text
SYS_CFG3 (0xf4) bit 0 = DEV_OFF   -> PMIC drops the rails
```

Boot log:

```text
pmicfs: rk809 on i2c0
pmic: key on gpio0 irq
```

Safety rules in the watcher:

- it clears any key bits latched before it started, so a warm reset
  does not immediately power the board off;
- it only arms after seeing the long bit clear at least once, so a
  button held across boot cannot power the board off until it is
  released and pressed again;
- it reads the long bit twice, 10 ms apart, before acting.

`pmicfs -n` disables the watcher (useful when debugging over serial).

Measured: writing `0xf4 = 0x21` (0x20 is the sleep polarity bit that is
already set) powered the board off immediately; the watcher performs the
same write.

Confirmed on hardware: a ~2 s press prints `pmic: long press, powering
off` and the board drops power.  The next power-up boots normally and
`/n/pwrkey` reads `none`, so the latch left by the press that powered
the board off does not cause another shutdown.

## Thermal

The PMIC has a hot-die comparator configured by `SYS_CFG1` (0xf2):
bits 5:4 pick 85/95/105/115 C, bit 6 picks the 140/160 C thermal
shutdown.  `hotdie <85|95|105|115>` changes the warning threshold (the
register change itself latches an edge, so the driver clears it).

On a hot-die warning the watcher logs `pmic: hot die warning` and
clears the bit.  It deliberately does **not** power the board off: the
warning threshold is 85 C by default and the hardware thermal shutdown
at 140 C is the protection of last resort.  An earlier build did power
off on the warning bit and shut the board down merely for changing the
threshold — that is why the action is now report-only.

## RTC alarm

`/ctl` commands:

```text
alarm                      show the alarm and whether it is enabled
alarm <Y> <M> <D> <h> <m> <s>   set it (same Rockchip date transform)
alarm off                  disable
```

The alarm uses registers 0x07..0x0c and `RTC_INT` (0x0f) bit 3.  When
it fires the watcher prints `pmic: rtc alarm` and clears `RTC_STATUS`
(0x0e, write 0xfe) and the `rtc-alarm` status bit.  Measured: setting
an alarm six seconds ahead produced the message on time.

## Boot integration

`drvman` starts `i2cfs` for the `i2c` class, then launches
`/boot/pmicfs` as a dependent child once `/srv/i2c` exists, and waits
for `/srv/pmic`:

```text
HARVEY-ARM64 DRVMAN i2c PASS
HARVEY-ARM64 DRVMAN pmic PASS
```

## Not implemented

- power key as a blocking event source: the watcher polls, and the
  GPIO0_A3 interrupt is not wired, so a press is only noticed within
  200 ms;
- thermal warning/shutdown (`TEMP105C`/`TEMP115C` and the hot-die status);
- suspend/restore: Harvey has no suspend path, so there is nothing to
  save the PMIC state for yet;
- RTC alarm as an action (it is reported and cleared, not routed to a
  wake or shutdown).
