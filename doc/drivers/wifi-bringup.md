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

`sdprobe` now completes SDIO enumeration on the v1.1 board:

```
sdprobe: cmd5 try 0: e=0 resp=91ffff00 rintsts=00000004 status=0001f9e6 cmd=20000045 cdetect=00000001
sdprobe: CMD5 resp 91ffff00
sdprobe: CMD5 ready, OCR 91ffff00
sdprobe: CMD3 RCA 8800 (resp 88000000)
sdprobe: CMD7 selected, resp 00001e00
sdprobe: CCCR[00] = 43
sdprobe: CCCR[01] = 03
sdprobe: CCCR[02] = 00
sdprobe: common CIS pointer 001000
sdprobe: cis manufacturer id 0000
sdprobe: cis product id 000c
sdprobe: function 1 enabled, IOReady 02
sdprobe: function 1 CIS pointer 002000
sdprobe: function 1 block size 0
sdprobe: block size set to 200
sdprobe: DT read AON_APB: 00000000 00000000 00043014 000014e0
sdprobe: DT write/read CP SRAM: ok (512/512)
sdprobe: firmware /boot/wcnmodem.bin 947120 bytes
sdprobe: firmware written to 0x40500000
sdprobe: CP_RESET_REG 00000001
sdprobe: CP reset released
sdprobe: sync 9eedc767
sdprobe: sync f0f0f0f1
...
sdprobe: CP boot finished
sdprobe: DONE
```

This confirms the module is the Unisoc part (`manufacturer id 0000`,
product `000c`) and that WL_REG_ON, the pinmux, the 3.3 V IO domain,
the CRU gates and the DW command path are all correct.  The earlier
CMD5 timeout was a host-side clock/pin issue, not a dead card.

Function 1 starts with block size zero.  `sdprobe` now sets it to 512
(`0x200`) through FBR `0x110/0x111`, writes the DT system address
`0x4083C000` to FBR `0x15c..0x15f`, and reads one block from function 1
address `0x0f`.  The read returns:

```
00000000 00000000 00043014 000014e0
```

That confirms the vendor DT-mode path works: the CP address window is
reachable over CMD52/CMD53 block transfers.  A 512-byte pattern written
to CP SRAM reads back correctly.

`sdprobe firmware` then loads `/boot/wcnmodem.bin` (947120 bytes) to
`CP_START_ADDR` (`0x40500000`), clears the reset bit in
`CP_RESET_REG` (`0x40088288`), and polls `SYNC_ADDR` (`0x405E73B0`).
The CP starts and reaches `SYNC_ALL_FINISHED` (`0xF0F0F0FF`).

## Reference source

The out-of-tree driver is available at:

```
https://github.com/MichaIng/uwe5622
```

Relevant files:

- `unisocwcn/platform/wcn_boot.c` — `start_marlin`, firmware load/parse,
  CP boot.
- `unisocwcn/sdio/sdiohal_main.c` — SDIO function 1, block size 512,
  DT/PK mode CMD53 transfers, FBR system-address registers.
- `unisocwcn/sdio/sdiohal.h` — `SDIOHAL_DT_MODE_ADDR` (`0x0f`),
  `SDIOHAL_FBR_SYSADDR0..3` (`0x15c..0x15f`), block size 512.
- `unisocwcn/include/uwe5622_glb.h` — chip ID `0x2355B001` and the
  AON/APB register map.

## Next steps

1. Port the `sprdwl` WLAN command path now that the CP is running.
2. Bring up the firmware command channel, chip configuration and
   scan/join.
3. Add the WPA2 four-way handshake.
4. Expose the data path to `/net`.

## PK-mode command probe

After CP boot, `sdprobe pk` wakes the CP (`REG_CP_SLP_CTL` = 0) and sends
a `WIFI_CMD_GET_INFO` packet on SDIO PK channel 8:

```
sdprobe: PK GET_INFO send
sdprobe: PK rx off 0 hdr 0302c83f type 0 subtype 3 len 1424
sdprobe: PK rx off 1428 hdr ffffffff type 15 subtype 15 len 65535
sdprobe: PK no command reply
```

So the PK TX path is accepted and the CP is sending RX packets, but the
reply is not on the expected command channel.  The vendor driver maps
RX command port 22 to subtype 10 (`subtype + SDIO_CHN_TX_NUM`, where
`SDIO_CHN_TX_NUM` is 12); this packet is subtype 3, which the WLAN
driver would not hand to its command handler.  Before a scan can work,
the RX channel mapping and the WLAN initialization sequence
(`WIFI_CMD_OPEN`, ini download, version sync) must be matched to the
vendor driver.

The next milestone is a scan result, not a full association.

## CP firmware notes

`wcnmodem.bin` is a raw ARM Cortex-M4 image, not encrypted or
compressed:

- Strings include `cm4_region`, `Hardfault`, `Mpufault`, `Busfault`,
  `Usagefault`, `WCN_VER`, `flush_wcn_log`, `loopcheck_ack` and
  `pseudo_atc_cfg`.
- The vector table at the start has an initial stack pointer of
  `0x1a0f50` and a reset vector of `0x100319` (Thumb).
- A literal at file offset `0x380` contains `0x100000`, so the image is
  linked at base `0x100000`.

Ghidra analysis at base `0x0` resolves the data references (the
`0x100000` value in the vector table is an alias).  The key findings:

- The command-name table starts at file offset `0x7c348` with 48
  entries (`WIFI_CMD_MAX_RAM`): `WIFI_CMD_GET_INFO`,
  `WIFI_CMD_OPEN_DEV`, `WIFI_CMD_SCAN`, ...
- The firmware contains `WIFI_CMD_DOWNLOAD_INI`,
  `rf_board_config_section_1_download`,
  `rf_board_config_section_3_download`, `INI_MAGIC_WORD` and
  `sizeof(INI_MAC_CFG_T) == len`.
- The 1424-byte type-0/subtype-3 packet contains code-like words
  (`f7 f7 09 d5`, `00 00 83 d5`, ...) and an incrementing sequence
  number at offset `0x0c`.  It is a CP log/dump, not a command reply.
- Sending `WIFI_CMD_OPEN` without the ini/config produces the same
  log stream; the CP is asserting before the WLAN subsystem is
  configured.

So the next required step is the ini/config download, not more channel
probing.  The vendor driver parses
`/lib/firmware/uwe5622/wifi_2355b001_1ant.ini` into a `wifi_conf_t`
and sends three sections with `WIFI_CMD_DOWNLOAD_INI` (command 76):

- section 1: version, board config, TPC, frequency compensation,
  power tables, reg domain, band-edge offsets;
- section 2: TX scale, misc, debug registers, coexistence;
- section 3: RF data.

The `wifi_conf_t` generator now works:

```
sizeof conf=3296 sec1=328 sec2=1464 rf_len=14
```

It parses `wifi_2355b001_1ant.ini` with the vendor's table and emits
`sec1.bin`, `sec2.bin`, `sec3.bin` plus C arrays.  `sdprobe pk` sends
the three sections with `WIFI_CMD_DOWNLOAD_INI` (command 76), section
number and CRC16, then sends `WIFI_CMD_OPEN`.

## CP boot handshake

`downloadfirmware()` originally treated `SYNC_ADDR = 0x405E73B0` reading
`0xF0F0F0F1` as success.  That value is not success: it is
`SYNC_CALI_WAITING`, the CP asking the host for its SDIO configuration
and RF calibration data.  The vendor sequence (`wcn_boot.c`) is:

1. write `wcnmodem.bin` to `0x40500000`, release `CP_RESET_REG` bit 0;
2. poll `SYNC_ADDR` until it reads `SYNC_CALI_WAITING` (`0xF0F0F0F1`);
3. write `sdio_config` at `SYNC_ADDR + 0x1c`;
4. write `SYNC_CALI_WRITE_DONE` (`0xF0F0F0F2`) to `SYNC_ADDR`;
5. poll until `SYNC_ALL_FINISHED` (`0xF0F0F0FF`).

The `sdio_config` word is built from `union wcn_sdiohal_config`:

```
bit 0      sdio_config_en              1
bit 3:1    vendor_id                   0 (unisoc default)
bit 4      sdio_rx_mode                1 (sdma; 0 would be adma)
bit 7:5    sdio_blk_size               1 (512-byte blocks)
bit 12:11  sdio_irq_type               1 (inband data1 irq)
bit 22:18  wake_host_level_duration    2 (20 ms)
bit 23     wake_host_data_separation   1
=> 0x00880831
```

Without this the CP reaches `SYNC_ALL_FINISHED` on its own but never
initializes the WLAN command path: it accepts PK writes, sends log
packets, and never answers a command.

## SDIO PK framing, TX and RX

The vendor's public header is `struct sdio_puh_t`:

```
bits 5:0   pad (CONFIG_SDIOM sets 7 bits)
bit 6      check_sum (unused for WLAN packets)
bits 22:7  len (payload length, excludes this header)
bit 23     eof
bits 27:24 subtype
bits 31:28 type
```

`sdiohal_channel_to_hwtype()` maps TX channel `c` to `type 0,
subtype c`; RX maps `subtype s` to channel `s + 12`.  WLAN commands go
out on subtype 8 and their replies come back on subtype 10 (channel 22).
Subtype 3 (channel 15) is the CP log ring.

The vendor writes each packet as `sdio_writesb(0x20, buf, ALIGN_BLK(used))`:

- `used = 4 + ALIGN4(payload) + 4`, with an eof header (all zero except
  bit 23) after the payload;
- the buffer is padded up to a 512-byte multiple;
- if the total is over 512 bytes the full 512-byte blocks go out in one
  block-mode CMD53, and the remainder (or the whole packet when it is
  <= 512 bytes) goes out in a byte-mode CMD53.

That last detail matters: a 512-byte total must be sent in *byte* mode,
not as one block.  `pkwrite()` now mirrors this and uses the DW MSHC
IDMAC path for multi-block writes.

RX is an aggregation window: a 4-block (2048-byte) CMD53 read from
0x20 returns one or more packets followed by a trailer with
`valid_len` at 2040 and the pending packet count at 2044.  The card
interrupt is not asserted (`CCCR[05]` stays 0 even with the interrupt
master enabled), so the driver polls the window.

## Working WLAN sequence

`sdcard: sdprobe pk` now reaches a decoded scan:

```
sync 9ee4c763
sync f0f0f0f1
cali handshake, sdio_config 00880831
sync f0f0f0ff
CP boot finished
INI section 1 len 328 crc 9594      -> id 76 status 0
INI section 2 len 1464 crc a057     -> id 76 status 0
INI section 3 len 14 crc a354       -> id 76 status 1
WIFI_CMD_OPEN  (mode 1, mac 02:00:00:00:00:01)
                                    -> id 179 (flow control), id 3 status 0
WIFI_CMD_SCAN  (channels 1..14)     -> id 11 status 0
                                    -> id 131 x N (beacon/probe response)
                                    -> id 130 SCAN_DONE, code 1
scan ap bssid 60:38:e0:cd:5f:f1 ssid [Linksys01314]
scan ap bssid fa:d0:0e:34:01:21 ssid [cozyyConnections]
```

Notes:

- Section 3 answers with status 1 rather than 0; the CP stays up and
  the following commands work, so it is not a failure code in this
  path.  The reply's byte 9 is a rolling sequence number (0, 1, 2, ...),
  not a section index.
- `WIFI_EVENT_MGMT_FRAME` (0x83) carries scan results.  Two payload
  shapes appear: either a 12-byte `struct sprdwl_event_mgmt_frame`
  (type=4 for scan, channel, signal, bssid, frame length) followed by
  the 802.11 frame, or the 802.11 frame directly after the 12-byte
  command header.  `scanframe()` accepts both and decodes the SSID IE.
- The scan is slow: the CP walks channels at roughly 40 ms each and
  completes in 2-4 s.  Results only arrive while the host keeps
  draining the RX window.

## Association

`WIFI_CMD_CONNECT` (10) takes `struct sprdwl_cmd_connect` (82 bytes on
arm64, because `WLAN_MAX_KEY_LEN` is 32):

```
0   u32 wpa_versions
4   u8  bssid[6]
10  u8  channel
11  u8  auth_type       0 = open, 1 = shared
12  u8  pairwise_cipher (value | 0x80 SPRDWL_VALID_CONFIG)
13  u8  group_cipher
14  u8  key_mgmt        2 = PSK
15  u8  mfp_enable
16  u8  psk_len         32 (the raw PSK, not the passphrase)
17  u8  ssid_len
18  u8  psk[32]
50  u8  ssid[32]
```

For WPA2 the firmware wants the 32-byte PSK.  `sdprobe` derives it from
the passphrase with PBKDF2-HMAC-SHA1 (4096 rounds, SSID as salt), which
is small enough to carry in the probe.

Measured against `Linksys01314` (WPA2/WPA3 transition mode):

```
WIFI_CMD_CONNECT ssid [Linksys01314] ch 1 bssid 60:38:e0:cd:5f:f1 psk 32
cmd 10 -> status 0
PK reply id 245 plen 23     WIFI_EVENT_STA_LUT_INDEX
sta lut index 6 ctx 0 action 1
PK reply id 128 plen 672    WIFI_EVENT_CONNECT
connect event status 0 bssid 60:38:e0:cd:5f:f1 ch 1 sig -91
```

`WIFI_EVENT_STA_LUT_INDEX` (0xf5) carries
`{ctx_id, action, sta_lut_index, ra[6], ht, vht}`; the index is needed
in every TX data descriptor.

## Data path

TX uses SDIO subtype 10 and RX subtype 12.  The payload starts with
`struct tx_msdu_dscr` (11 bytes):

```
0  common        type=2 (DATA), ctx_id
1  offset        11
2  tx_ctrl       sw_rate is set for ARP/management-style frames
3  pkt_len       802.3 frame length
5  buffer_info   msdu_tid (0 = BE)
6  sta_lut_index from event 0xf5
7  color/rsvd
9  tcp_udp checksum offset
```

followed by the 802.3 frame.  RX data starts with `struct rx_msdu_desc`;
`msdu_offset` (byte 1) and `msdu_len` (bytes 2..3) locate the frame.

The firmware also sends `WIFI_EVENT_SDIO_FLOWCON` (0xb3) with four
credit bytes (VO, VI, BE, BK), 16 each after connect.  Sending without
credit makes the CP log `TXC_FAIL_Q ... FC: 0`.

### Current blocker: the link is too weak

With the board where it is, `Linksys01314` is heard at -91/-92 dBm
while the host sees the same AP at 70 %.  Association and the WPA2
handshake complete, and the probe's DHCP DISCOVER and ARP requests are
accepted by the CP (`datatx e=0`, BE credit decrements, no `TXC_FAIL_Q`
on the broadcast), but no frame ever comes back: no DHCP offer and not
a single subtype-12 packet in 4 s.  The CP itself logs
`send_notify_cqm_to_host rssi_low_count=61` and starts roaming.

That is a link-budget problem, not a framing one.  The next test needs
the antenna attached (the v1.1 board has a U.FL connector) or the board
next to the AP, and then `sdprobe connect <ssid> <passphrase> [channel]`
should reach DHCP.

## Next

1. Retest association + DHCP with the board next to the AP; then send
   `WIFI_CMD_NOTIFY_IP_ACQUIRED` (26) and keep the lease.
2. Turn the one-shot probe into a long-running driver that owns the
   data channels and exposes a `/net` interface.
3. WPA2 key install (`WIFI_CMD_KEY`, 14) only matters if the firmware
   does not keep doing the handshake itself; with a PSK in CONNECT it
   does.
