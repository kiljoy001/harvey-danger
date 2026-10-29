# GMAC bring-up on the Orange Pi 3B (rk3566)

Controller: `ethernet@fe010000`, `rockchip,rk3568-gmac` /
`snps,dwmac-4.20a`.  PHY: Motorcomm **YT8531** (`4f51:e91b`) behind
MDIO, RGMII, reset on GPIO3_C2 (bank 3, pin 18, active low).
Driver: `sys/src/cmd/gmac/gmac.c` (`/boot/gmac`), user space over the
`#P` grant for the MAC, GRF and CRU.

## Register path (working)

- MAC registers answer with the clocks the bootloader leaves on:
  `CONFIG 0x0030a000`, `HW_FEATURE 0x160171e7 0x010c01c8`.
- GRF `GMAC1_CON0/CON1` (`0x388/0x38c`, hiword-masked): CON1 bit 4
  selects RGMII, bits 0/1 enable the MAC TX/RX clock delays.  The board
  boots with CON0 `0x264f` (tx 0x4f, rx 0x26) and CON1 `0x13`.
- Clock: `CLKSEL_CON(33)` (`0x184`) selects the RGMII speed divider in
  bits 5:4: 0 = /1 (125 MHz), 3 = /5 (25 MHz), 2 = /50 (2.5 MHz).  The
  bootloader leaves `0x20` (/50) because its TFTP runs at 10 Mbps.
- MDIO is clause 22 through `MAC_MDIO_ADDRESS 0x200` /
  `MAC_MDIO_DATA 0x204`: busy bit 0, PHY address bits 25:21, register
  bits 20:16, CSR clock range bits 11:8, op bits 3:2 (3 = read,
  1 = write).  CSR 1 for a 100-150 MHz CSR clock.

The probe resets the PHY through gpiofs (`/srv/gpio`, `gpio3/ctl`
"18 out 0/1", 50/200 ms) and then:

```
gmac: phy 0 id 4f51:e91b
gmac: phy 0 bmcr 1140 bmsr 796d link 1 aneg 1
gmac: phy status bc40 link 1 speed 1000 duplex 1
```

## DMA path

Descriptor rings live in the `#P/dma` region.  The controller is a
DesignWare GMAC4:

| Register | Offset |
| --- | --- |
| `DMA_BUS_MODE` / `DMA_SYS_BUS_MODE` | `0x1000` / `0x1004` |
| `MTL_TXQ0_OP_MODE` / `MTL_RXQ0_OP_MODE` | `0xd00` / `0xd30` |
| `DMA_CH0_*` | `0x1100` + 0x80*chan |
| TX base / tail / ring length | `0x1114` / `0x1120` / `0x112c` |
| RX base / tail / ring length | `0x111c` / `0x1128` / `0x1130` |

Descriptors are 16 bytes: `des0/des1` buffer address, `des2` buffer1
size (13:0) plus interrupt-on-completion (31), `des3` OWN (31), FD (29),
LD (28) and the packet length (14:0) for TX; RX sets OWN|BUF1_VALID (24)
and interrupt-on-completion (30).  The tail pointer is the descriptor
*after* the last valid one, and it has to advance for every frame or the
DMA never sees new work.

Verified on the wire at 10 Mbps: the host's bridge counted our frames by
source MAC (`02:00:00:00:00:01`) after `gmac tx`; the MAC's own MMC
counters agree (`0x714` = frames, `0x710` = bytes).

## Open: 100 and 1000 Mbps

- 10 Mbps, keeping the PHY in the bootloader's state (no reset):
  frames arrive, TX and RX both work.
- 100 Mbps (advertise-only autoneg, MAC delays on, /5 divider): the MAC
  transmits and the counters move, but nothing reaches the switch.
- 1000 Mbps (/1 divider): the MAC wedges; register/DMA accesses stop
  responding until reset.
- Switching the MAC clock to the PHY's `CLK_OUT` (`CLKSEL_CON(33)` bit
  2) also wedges, even after programming the YT8531 clock output
  (`0xa012 = 0xd0`), so the board's `clock_in_out = "input"` path is
  not wired the way the Armbian DT assumes, or the PHY output is not
  enabled.

The pattern points at the RGMII clock/delay setup for the higher
speeds: the /50 path works, /5 sends but corrupts, /1 loses the clock.
Next steps are to sweep the MAC TX/RX delay values at 100 Mbps with the
host counting frames, check the PHY's RGMII delay registers, and
confirm which clock actually reaches `CLK_MAC1_2TOP`.


## Is 1 Gbps real on this board? (v1.1)

Our unit is a **v1.1**: toggling GPIO3_C2 (the v1.1 PHY reset) actually
resets the PHY, while v2.1 moved that line to GPIO4_C4.  Measured on
this unit, with a fresh boot:

| Link | U-Boot ping | our driver |
| --- | --- | --- |
| 10 Mbps | works | works (frames counted at the switch) |
| 100 Mbps | **fails** | fails |
| 1000 Mbps | **fails** | fails |

U-Boot autonegotiates 1000 Mbps full duplex on its own and still cannot
ping; forcing 100 Mbps links but cannot ping either.  Only 10 Mbps
passes traffic.  So the working TFTP recipe is not a workaround for a
Harvey bug - it is the only speed this board does.

This matches what the community reports about the v1.1:

- v1.1 uses a **3.3 V PHY**, v2.1 a 1.8 V PHY; the kernel device trees
  do not distinguish them.
- The DietPi maintainer: "These Ethernet issues were one of the reasons
  for the v2.1", and notes the U-Boot/Linux inconsistency about whether
  the reset delays live in the PHY or the MAC, plus a hardware defect
  that can appear over time.
- A reported workaround is forcing 100 Mbps.

There is also a concrete software mismatch: the YT8531 drive-strength
register `0xa010` is `0xdbcf` for 1.8 V IO and `0xdacf` for 3.3 V IO
(the vendor driver says so in a comment).  U-Boot and the Linux driver
both write `0xdbcf`, i.e. the 1.8 V value, on a v1.1 board.  Setting
`0xdacf` did not fix our unit at 10 or 1000 Mbps, but it is the correct
value for this revision and worth keeping.

The driver is therefore left at: probe/MDIO/PHY identity, DMA rings and
a verified 10 Mbps TX/RX path.  1 Gbps on v1.1 should be treated as
unproven at best, and a hardware limitation at worst.


## What Linux does for v1.1

Mainline carries two device trees, and the difference is exactly the
hardware change:

| | v1.1 | v2.1 |
| --- | --- | --- |
| PHY IO domain (`vccio5`) | `vcc_3v3` (3.3 V) | `vccio_phy1` (1.8 V) |
| PHY reset | `gpio3 RK_PC2` | `gpio4 RK_PC4` |
| MDIO address | 1 | 1 |

The shared `rk3566-orangepi-3b.dtsi` sets `phy-mode = "rgmii-id"` and
no `tx_delay`/`rx_delay`, but `dwmac-rk` then defaults those to
`0x30`/`0x10` anyway, so Linux ends up with **both** the MAC delays and
the PHY delays enabled.  We reproduced that exact combination
(mode 3) and it still does not put frames on the wire on this unit at
1 Gbps, while the same code at 10 Mbps does.

So software support for v1.1 exists and is not the missing piece here:
the DietPi maintainer fixed a device-tree bug, and the remaining
reports are described as a hardware failure that "appears after a
while" on v1.1, with only workarounds (force 100 Mbps, `ethtool -s
eth0 advertise 0x0f`, reflash the SPI bootloader).  Our unit behaves
like that: it links at 1 Gbps but passes no traffic, in U-Boot and in
this driver alike.

The one experiment left that would settle it is booting the official
Orange Pi vendor image (or a full mainline distro, not the initramfs)
on this unit and testing 1 Gbps there.
