# GMAC bring-up on the Orange Pi 3B (rk3566)

Controller: `ethernet@fe010000`, `rockchip,rk3568-gmac` /
`snps,dwmac-4.20a`.  PHY: Motorcomm **YT8531** (`4f51:e91b`) behind
MDIO, RGMII, reset on GPIO3_C2 (bank 3, pin 18, active low).
Drivers: `sys/src/9/aarch64/ethergmac.c` (kernel `Ether`, the product
path) and `sys/src/cmd/gmac/gmac.c` (`/boot/gmac`, the user-space
`#P` driver used for bring-up and raw-frame experiments).

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

## Working 10 Mbps service

`gmacfs` served `/srv/gmac` during bring-up (it is built but no longer
started by init, see the last section).  Its ARP and ICMP code is gone:
it is now a raw-frame client.

```
$ cat /mnt/gmac/ctl
gmac rk3566
link 10 mbps
mac 02:48:41:52:56:45
rx 166 tx 79
dma 0000000e 00000445 rxcur ef811160 txcur ef811070
ring ef811100 idx 6 desc c1000000 loops 11941 empty 11941
```

`/data` carries whole Ethernet frames: read returns the next received
frame, write transmits one.  At 10 Mbps the service answered ping from
the host with 50/50 replies while it still had the ARP/ICMP handlers.

Three lessons came from the vendor/Armbian driver:

- `DMA_SYS_BUS_MODE` is the AXI configuration register, not just a
  burst-mode bit.  The board's `snps,axi-config` asks for 8 read / 4
  write outstanding requests and 4/8/16-byte bursts; leaving the limits
  at zero stalls the DMA under load.
- The ring-length register holds the **last index**, not the entry
  count (`stmmac_set_rx_ring_len(dma_rx_size - 1)`).
- DMA rings are power-of-two sized (stmmac uses 256).

The bug that actually broke reception was ours: the transmit buffer sat
at DMA offset `0x200`, inside the 32-entry RX descriptor ring
(`0x100-0x2ff`), so every reply overwrote RX descriptors 16-31.  The
symptom was `RU` (receive buffer unavailable) in `DMA_CH0_STATUS` and a
ring that stopped after a few frames.  The rings and buffers now live
in disjoint ranges.

Also, `#P/dma` maintenance files cannot be cached across the lib9p
fork: a descriptor opened before `postmountsrv` is not valid in the
server child, so they are opened per call and all card access takes one
lock shared by both processes.

## Kernel Ether and `/net`

The card is also a kernel `Ether` (`sys/src/9/aarch64/ethergmac.c`):
it registers with `addethercard("gmac", gmacreset)` and appears as
`#l/ether0`.  It reuses the working init (RGMII, MDIO, 10 Mbps, ADMA
rings) but maps the MAC/GRF/CRU with `KADDR()` and takes the top
128 KiB of the reserved `#P/dma` window for descriptors and buffers.
This kernel does not deliver device interrupts to kernel handlers, so a
kernel process polls the RX ring and calls `etheriq()`; the TX path is
`etherwrite()` → `oq` → `ether->transmit()`.

The IP stack (`sys/src/9/ip`) is compiled in with the `ipprotoinit[]`
table from `config.c`, so `#I` provides `tcp`, `udp`, `rudp`, `ipifc`,
`icmp`, `icmpv6`, `gre`, `ipmux`, `il`, `arp`, `iproute` and friends.
Init unions the two devices at `/net` (`#I0` replacing, `#l` after) and
configures the interface the standard way:

```
open /net/ipifc/clone            # fd stays on the new interface's ctl
write "bind ether /net/ether0"   # medium and ether device
write "add 192.168.2.248 255.255.255.0"
```

After that `cat /net/ipifc/0/status`, `/net/iproute`, `/net/arp` and
`/net/ether0/{addr,stats,mtu}` all work.  Init sends one echo from the
board to 192.168.2.1 in a child process; the `NET-PING PASS` line means
route lookup, ARP and transmit all worked, and a ping from the LAN
proves receive and reply.

`/boot/ping` is the client for day-to-day use:

```
% ping -n 4 192.168.2.1
PING 192.168.2.1: 56 data bytes
88 bytes from 192.168.2.1: seq 1 time 1.426 ms
...
4 packets transmitted, 4 received, 0% packet loss
```

It connects a `/net/icmp` conversation and writes 20-byte IP header +
8-byte ICMP header + 56 bytes of data, letting the kernel fill in the
addresses and checksum.  There is no `alarm()` to interrupt a blocked
read on this kernel (notes are not delivered to user handlers, and
`notejmp` is still a stub), so after each send it polls the `EchoReply`
counter in `/net/icmp/stats` — tightly for the first few milliseconds,
then with `sleep()` — and only reads the reply once the counter moves.
That gives a working `-t timeout` without depending on signals.

Two bugs surfaced while wiring this up:

- `sendarp()` throttled with `NOW - a->ctime < 1000`, but a fresh
  `Arpent` has `ctime == 0`.  If the first packet was sent during the
  first second of uptime the request was dropped and, because v4
  entries are not on the retransmit queue, it was lost for good.  The
  throttle now only applies when `ctime != 0`.  Symptom: the board's
  ARP entry for the router stayed `WAIT` while the router had already
  learned the board's MAC (it saw the gratuitous ARP instead).
- v4 masks are stored v4-mapped with the 96-bit prefix set, so the
  `%M` formatter in `sys/src/libip/eipfmt.c` described a /24 as
  `/120`.  `maskstr()` in `ip/ipaux.c` now prints dotted quads for v4
  and is used by the ipifc status line and the route table.

`gmacfs` is no longer started by init: the kernel owns the card, and IP
lives in `/net`.  It is still built for raw-frame work, but refuses to
start when `/net/ether0` exists (the kernel driver is running) unless
given `-f`.


## Handoff from U-Boot (measured 2026-11-01)

The kernel probe now prints `gmac: no phy`: every MDIO read at
address 0..7 returns 0 or `0xffff`, and `MAC_CONFIG` reads zero, so the
block is not answering at all.  What the loader leaves behind, read from
the U-Boot prompt with `md.l`:

```
fe010000: 00000000 00000000 00000000 00000000   MAC registers
fdd20340 .. fdd20354: all zero                  CLKGATE_CON(16..20)
fdd20438: 00001000                              CRU SOFT_RST_CON(14)
```

`0x1000` is bit 12, `SRST_A_GMAC1` (reset id 236, `SOFTRST_CON(14)`
bit 12; bit 13 is `SRST_GMAC1_TIMESTAMP`).  Writing
`mw.l 0xfdd20438 0x10000000` (data 0, write-mask bit 28) deasserts it,
and the write is visible on read-back, but `0xfe010000` still reads
zero and does not accept writes.

U-Boot's own TFTP and DHCP work in that same state, so this is not a
dead PHY.  A block that ignores writes with its gate register reading
"ungated" points at the clock **parent**, not the gate: on RK3568
`aclk_gmac1` is parented to `aclk_usb` and `pclk_gmac1` to `pclk_usb`,
both in the clock-tree group labelled **PD_USB** (`clk-rk3568.c`).  That
label is not a separate RK3568 PM domain: the actual domain is
**PD_PIPE** (PMU_PWR_DWN_CON bit 8, PMU_BUS_IDLE_CON bit 11).  If that
domain is off, GMAC1 has no bus clock — and the same domain explains why
the xHCI side is still only partially alive.

So the next step is not in the Ethernet driver: it is to power PD_PIPE
(PMU power-domain registers) during device init, before `addethercard`.
Until then the board has no `/net/ether0`, and `NET-ETH FAIL no free
devices` is the expected result rather than a driver bug.

## PD_PIPE result (2026-11-02)

The kernel now powers PD_PIPE before `ethergmacinit()` and reports:

```text
HARVEY-ARM64 PD_PIPE PASS
gmac: no phy
```

So PD_PIPE is necessary but not sufficient.  Forcing the GMAC gates
(`CLKGATE_CON(17)` bits 2-6,10) and deasserting both GMAC resets
(`SOFTRST_CON(14)` bits 12/13) before the PHY probe **deadlocks the
kernel** before `gmac: no phy`, with a `lock ... held by qmalloc`
panic.  Clearing the MAC interrupt (`INT_EN = 0`) and releasing only
`SRST_A_GMAC1` also deadlocks.  The reset release itself is the trigger;
this is a bus/interrupt-path problem, not a missing clock gate.

Next steps, in order:

1. Mask the GMAC SPI at the GIC and verify the ARM64 SError/IRQ path
   before releasing `SRST_A_GMAC1`; `INT_EN = 0` alone is not enough.
2. Instrument the reset write: print the CRU reset/status before and
   after, with the GMAC SPI masked, to separate a bus hang from a
   spurious interrupt.
3. Only after a clean reset release, verify the `CLK_MAC1_2TOP`
   parent/divider (`CLKSEL_CON(33)`) and the PHY `CLK_OUT` path.
4. Add the gates one at a time, starting with `ACLK_GMAC1`/`PCLK_GMAC1`.
5. Then retry 100/1000 Mbps.

Do not apply the GMAC reset/gate writes until that path is understood;
the PD_PIPE-only kernel boots and leaves the rest of the board usable.

## U-Boot register experiment (2026-11-02)

At the U-Boot prompt after a successful TFTP transfer:

```text
PMU_PWR_DWN_ST        0x00000003   PD_PIPE bit 8 = on
SOFTRST_CON(14)       0x00001000   SRST_A_GMAC1 asserted
CLKGATE_CON(17)       0x00000000   GMAC gates on
CLKSEL_CON(33)        0x00000000   speed/parent state cleared
GMAC MAC/DMA regs     all zero
```

Doing the release sequence at U-Boot is safe but still does not wake the
MAC:

```text
mw.l 0xfdd20438 0x30000000     deassert both GMAC resets
mw.l 0xfdd20344 0x047c0000     enable ACLK/PCLK and MAC clocks
mw.l 0xfdd20184 0x00300020     10 Mbps RGMII divider
md.l 0xfe010000 4               still 00000000 00000000 00000000 00000000
```

So reset and gates are not sufficient.  The missing piece is the GMAC
clock parent/rate setup that Linux's `rk_gmac_set_to_rgmii()` does:
`mac_clk_rx`, `mac_clk_tx`, `clk_mac_speed`, `clk_mac_refout`,
`clk_gmac1_ptp_ref`, and the `CLK_MAC1_2TOP`/`SCLK_GMAC1` muxes.
Harvey's `setspeed()` only writes the RGMII divider bits 5:4, and it runs
after the PHY probe.  Port the full clock setup before MDIO, and keep
the U-Boot reset-release experiment separate from the kernel deadlock.

The U-Boot environment also still boots `aspera-arm64.img`; the working
image is `aspera-usb.img`, loaded by hand:

```
tftpboot 0x40080000 aspera-usb.img
tftpboot 0x50000000 userland.img
tftpboot 0x48000000 orangepi-3b-v1.1.dtb
booti 0x40080000 - 0x48000000
```

The boot archive at `0x50000000` is a fixed address (`BootarchiveAddr`
in `sys/src/9/aarch64/main.c`), not an initrd: the third `booti`
argument stays `-`.

## U-Boot clock capture (2026-11-02)

After `mii info` at the U-Boot prompt, the GMAC clock tree is already
programmed:

```text
CLKSEL_CON(80)   0x00000f07   cpll_125m divider /8
CLKGATE_CON(35)  0x00000000   cpll_125m gate on
CLKSEL_CON(33)   0x00000020   10 Mbps RGMII divider
CLKGATE_CON(17)  0x00000000   GMAC gates on
mii info         PHY 0x00/0x01 detected
fe010000         all zero
```

`mii info` proves U-Boot can reach the PHY through MDIO, but the GMAC
register block still reads zero, and the reset release inside Harvey
locks the kernel.  This is no longer a clock-tree gap: the v1.1 MAC
block is only usable while U-Boot's live network state is present, and
the kernel cannot reproduce that state safely.

Treat v1.1 Ethernet as a board-level limitation until the live U-Boot
GMAC state can be captured during TFTP or a Linux `devmem` reference is
taken on the vendor image.  Do not spend more time on the generic
RK3568 clock setup; it is already correct.
