# RK3566 OTP (eFuse) notes

The SoC OTP block is `otp@fe38c000`, compatible `rockchip,rk3568-otp`. It is
128 bytes (`0x80`), read 16 bits at a time through a small state machine with
ECC. It holds the chip identity and the per-die calibration values; it is
write-once and read-only from software.

## Cell map (vendor rk356x.dtsi)

```
0x02  cpu-code (2 B)          0x2e  cpu_tsadc_trim_l
0x07  serial (5 bits)         0x2f  cpu_tsadc_trim_h    (bits 0-3)
0x08  cpu-version (3 bits)    0x30  gpu_tsadc_trim_l
0x0a  id (16 B)               0x31  gpu_tsadc_trim_h    (bits 0-3)
0x1a  cpu-leakage             0x31  tsadc_trim_base_frac (bits 4-7)
0x1b  log-leakage             0x32  tsadc_trim_base
0x1c  npu-leakage
0x1d  gpu-leakage
0x2a  core-pvtm (2 B)
```

## Reading it from Linux (oracle)

Mainline 6.18.21 does not ship the RK3568 OTP entry, and Armbian's
`6.18.21-current-rockchip64` build gates the match table on vendor
`CONFIG_CPU_*` symbols, so the stock kernel cannot bind the driver. The
following was used to get a trusted read:

1. Build vanilla 6.18.21 with the Armbian config
   (`/boot/config-6.18.21-current-rockchip64`), `make ARCH=arm64 LLVM=1 Image`.
2. Apply two upstream patches, in order:
   - `6c403594354d59fb288978c5a23008da89ba2741` nvmem: rockchip-otp: handle
     internal word_size in main reg_read op
   - `902fa931a2095566de6be12c8153f7fa9b31ab5f` nvmem: rockchip-otp: add
     support for RK3568
3. Add the node to the board DTB (the deployed DTB has none):

```
otp@fe38c000 {
        compatible = "rockchip,rk3568-otp";
        reg = <0x00 0xfe38c000 0x00 0x4000>;
        clocks = <&cru CLK_OTPC_NS_USR>, <&cru PCLK_OTPC_NS>,
                 <&cru PCLK_OTPPHY>, <&cru CLK_OTPC_NS_SBPI>;
        clock-names = "otp", "apb_pclk", "phy", "sbpi";
        resets = <&cru SRST_OTPPHY>;
        reset-names = "otp_phy";
        status = "okay";
};
```

Note the clock *names* are the driver's, not the older vendor DTS names
(`usr`/`sbpi`/`apb`/`phy`); a mismatch makes `devm_clk_bulk_get` fail and the
probe never creates the nvmem device.

4. Boot with `rdinit=/init` and a tiny init that mounts sysfs and prints
   `/sys/bus/nvmem/devices/rockchip-otp0/nvmem`.

## Board result (Orange Pi 3B v1.1, RK3566)

```
00: 52 4b 35 66 02 00 fe 21 18 01 4d 32 59 34 30 31
10: 00 00 00 00 00 00 00 15 24 07 1a 2f 03 07 00 00
20: 00 00 00 00 00 00 00 00 00 01 e7 22 03 07 00 00
30: 00 00 00 00 27 00 00 00 00 00 00 00 00 00 00 00
40: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff 00
50: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
60: 00 00 00 00 00 00 00 01 52 4b 35 66 fe 21 18 01
70: 4d 32 59 34 30 31 00 00 00 00 00 00 00 00 15 24 07
```

- `0x00` = `"RK5f"`, `0x0a` = `"M2Y401"` — matches the documented `id@0x0a`,
  so the word/byte mapping is right.
- The tail from `0x68` repeats the header (minus the two `cpu-code` bytes);
  this looks like a redundant copy in the array, outside the calibration area.
- **All TSADC trim cells are zero**: `trim_l`, `trim_h`, `trim_base_frac` and
  `trim_base` are unprogrammed. The vendor driver treats that as "no trim"
  (`if (!trim_l && !trim_h) return 0`), so the correct temperature on this
  board is the untrimmed table conversion.

The read succeeded with ECC enabled and returned no ECC error, i.e. Rockchip's
check accepted every word.

## Consequence

`tsadcfs` needs no trim on this board. The remaining difference against the
Linux reference (a few degrees) is thermal state, not calibration. An `otpfs`
would still be useful for the chip identity and serial, but not for the
temperature.

The test artifacts (`linux-Image-otp`, `linux-opi3b-otp.dtb`,
`otp-initramfs.img`) are published on the TFTP server for repeat runs.
