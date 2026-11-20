# Moto G Power (2021) / borneo support matrix

Device: Motorola moto g power (2021), codename `borneo`, SoC Qualcomm
SM6115 (Bengal, Snapdragon 662), bootloader unlocked
(`ro.boot.verifiedbootstate=orange`), stock Android 11 / kernel
4.19.157 on slot `_b`, eMMC storage (`mmcblk0`).

Vendor reference (oracle): `/home/scott/Repo/motorola-bengal/`

- `kernel/` and `dts-repo/` — Motorola 4.19 source trees
- `firmware/unpacked/borneo.dtb` — stock DTB (432 KiB, 19395 lines
  decompiled, 279 distinct `compatible` strings)
- `firmware/boot.img` — stock boot image; `firmware/harvey-boot.img`
  — Harvey-packed boot image (see `util/arm64/pack_boot.py`)
- `dts/bengal-borneo-display.dtsi`,
  `dts/bengal-borneo-touchscreen-overlay.dtsi` — panel/touch overlays

Status values follow [support-matrix.md](support-matrix.md):
`supported` (tested on hardware), `partial`, `experimental`
(code exists, not board-qualified), `unsupported` (no driver yet).

Harvey paths: **native** = Harvey kernel/userspace driver,
**LKL** = real Linux driver inside an LKL guest
(`util/arm64/lkl-families.json`: `bengal_usb` main guest,
`bengal_ctl` console/recovery guest), **none** = not present.

## Core / SoC

| Function | Vendor HW | Harvey path | Status |
| --- | --- | --- | --- |
| CPUs, GICv3, timer, PSCI | Kryo 260, `arm,gic-v3` | native | experimental |
| GCC clocks/resets/GDSC | `qcom,bengal-gcc` | LKL `SM_GCC_6115` | experimental |
| TLMM pinctrl/GPIO (114) | `qcom,bengal-pinctrl` | LKL `PINCTRL_SM6115` | experimental |
| SPMI PMIC arbiter | `qcom,spmi-pmic-arb` | LKL, both guests | experimental |
| PMIC PM6125+PMI632 GPIO | `qcom,spmi-gpio` | LKL | experimental |
| PON / power key / reset | `qcom,qpnp-power-on` | LKL `bengal_ctl` | experimental |
| PMIC RTC | `qcom,pm8941-rtc` | LKL `RTC_DRV_PM8XXX` | experimental |
| ADC5 / temp alarm | `qcom,spmi-adc5` | LKL | experimental |
| TSENS thermal | `qcom,tsens24xx` | LKL `QCOM_TSENS` | experimental |
| PRNG | `qcom,msm-rng` | LKL `QCOM_RNG` | experimental |
| QFPROM / nvmem | `qcom,qfprom` | LKL | experimental |
| Watchdog | `qcom,msm-watchdog` | none | unsupported |
| RPM / SMP2P / SMEM / GLINK | `qcom,rpm-smd` etc. | none | unsupported |

## Buses, storage, USB

| Function | Vendor HW | Harvey path | Status |
| --- | --- | --- | --- |
| QUPv3 GENI SE | `qcom,qupv3-geni-se` | LKL `QCOM_GENI_SE` | experimental |
| GENI I2C (3 buses) | `qcom,i2c-geni` | LKL, i2c0/i2c1 in DTS | partial |
| GENI SPI (4 buses) | `qcom,spi-geni` | LKL, spi2 in DTS | experimental |
| GENI UART console | `qcom,msm-geni-serial-hs` | none in Bengal DTS | unsupported |
| eMMC (sdhc1, HS400) | `qcom,sdhci-msm-v5` @4744000 | LKL `MMC_SDHCI_MSM` | partial |
| SD card (sdhc2) | `qcom,sdhci-msm-v5` @4784000 | LKL, in DTS | experimental |
| UFS | `qcom,ufshc` (not populated) | config disabled | unsupported |
| USB DWC3 gadget | `snps,dwc3` @4e00000 | LKL `bengal_ctl` | experimental |
| USB QUSB2 PHY | `qcom,qusb2phy` | config disabled (bisect) | unsupported |
| USB host mode | DWC3 + QUSB2 | none | unsupported |

Known blocker: **GENI I2C first-write wedge** — `ti,lm3697@37` on
i2c1 is `status="disabled"` in `util/arm64/lkl-bengal-usb.dts`
with `lm3697,stage-limit` kept as the debug gate. Backlight cannot
be enabled until this is fixed. Vendor notes (2026-11-18):
Motorola drives LM3697 via MFD `ti-lmu`
(`kernel/drivers/mfd/ti-lmu.c`, node `lm3697-bl@37` with a
`ti,lm3697-backlight` child in `bengal-leds-lm3697.dtsi`);
the LKL kernel uses the mainline `leds-lm3697` with an
`led@0` child, which our DTS follows. Stock DT enables both
LM3697 @37 and AW99703 @36 (different SKUs); which chip is
populated on this djnboe unit is unproven — a probe NACK is
possible and must not wedge the bus. The LKL GENI driver is
the mainline one plus a 1 ms delayed-work ISR poller
(`geni_i2c_poll_work`) for host IRQ-forwarding latency;
Motorola's is the CAF variant with GPI/DMA and ipc logging.

## Observed on hardware, 2026-11-19, branch `pdma` (RAM-boot, no flash)

`pdma` (DMA chunks, claims as open files, `regs`) packed with
`pack_boot.py --families bengal_ctl,bengal_usb`.

- Both guests come up under the claim rules: the console gadget
  enumerates 30 s after `fastboot boot`; eMMC p1-p77 and the
  `/fw/{modem,bt,dsp}` mounts as before; touch and WCN3990 firmware
  staged.  `#P/*/owner`: `qcom,gcc@1400000`, `pinctrl@400000` and
  `qcom,spmi@1c40000` are `lkld shared` (the last held by both
  guests); dwc3, ssusb, sdhci x2, GENI, i2c x2, spi, qrng, tsens and
  icnss are `lkld`.
- From rc, against held devices: an exclusive or shared claim of
  `sdhci@4744000` and an exclusive claim of the clock unit fail with
  `device already claimed`; a shared claim of the clock unit succeeds
  and ends with its writer; `wait` and `regs` without a claim fail
  with `device not claimed`.  On a free device (`ufshc@4804000`): a
  claim held open on fd 3 makes `owner` read its name, lets a child
  open `regs`, keeps a second claim out, and ends when the block
  closes fd 3, after which the device can be claimed again.
- The "ARM64 repeat fault" lines at the end of earlier boot logs were
  a false alarm: the detector compared consecutive faults on any
  process, and each of `lklboot`'s threads (separate processes with
  their own page tables) takes its own first fault on a shared heap
  page, at the same address and pc.  It now keys on CPU and process;
  no report since.
- Cleaned up the same day, verified on the phone: the boot `echo`
  writes its line in one write and reports a refused write (it wrote
  each word separately, so `echo x shared` reached a claim file as
  three messages); `wifi@c800000`, the guest's name for the WCN3990,
  left the `bengal_usb` list (`qcom,icnss@C800000` maps the same
  window); `drvman` starts a child only when its parent service is up,
  so it no longer waits out `npufs` and init's deadline, and writes
  each status line whole; init skips `fanctl` without TSADC and PWM,
  `emmcfs` when the build has none, and the IP interface without
  Ethernet, each with the reason.  The console now comes up 15 s after
  `fastboot boot`, against 30-40 s.
- The guest's WCN3990 node uses IRQs 358-369; `lklboot` forwards only
  the two `#P` lists for `qcom,icnss@C800000` (390, 391).  WiFi needs
  the rest.

## Observed on hardware, 2026-11-18 (RAM-boot, no flash)

`fastboot boot firmware/harvey-boot.img` (fastboot 34.0.4),
console via serial MCP on the DWC3 gadget (`/dev/ttyACM1`,
`Harvey_ARM64_USB harvey01`). Boot log is `/dev/prevlog` on the
phone (`HARVEY BOOT 1`). Observed, not inferred:

- Native kernel up: CPU timers, `DISPLAY-BIND`, LKL arena,
  `bengal_ctl` guest (DWC3 console) and `bengal_usb` guest both up.
- eMMC (LKL): `mmc0: new HS400 MMC card`, `mmcblk0: G1J9R8
  58.2 GiB` p1–p77, boot0/boot1/RPMB. **Working.**
- SD (LKL): `mmc1` SDHCI probed, `Got CD GPIO`; no card inserted.
- Touch (LKL): FocalTech FT8009 probe stages 1–10 complete,
  `chip id = 0x8009`, `input1` registered, IRQ 1029, firmware
  `focaltech-djnboe-ft8009-07-000-borneo.bin` (180224 B) loaded.
  Probe **working**; coordinate reports not yet exercised.
  The firmware is a vendor blob and is not in the repository: copy it
  from the phone's stock firmware into `util/arm64/fw/` (ignored by git)
  before `build_lkl.py`, which embeds whatever that directory holds.
- PMIC (LKL): SPMI arbiter v5, RTC registered, `pm8941_pwrkey`
  as `input0`. GENI I2C x2 and GENI SPI probed, no wedge at probe.
- WiFi (initial, wrong): guest attempted **Unisoc Marlin SDIO**
  (`sprdwl`) power-on and timed out. Stock DTB corrects this:
  the phone is **Qualcomm WCN3990** (`qca,wcn3990`,
  `qcom,icnss@C800000`, BT slim codec) — see the boot7 section.
  The Unisoc driver is an artifact of the shared LKL kernel
  config (its BSP creates a device without a DT node).
- (Fixed 2026-11-19 on `pdma`, see that section.)  Native Rockchip
  services ran wrongly on Bengal and failed:
  `gpufs` (no PMU at `0xfdd90000`), `tsadcfs` (no GRF),
  `spifs` (no CRU), `gpiofs` (no GPIO banks in `#P`), native
  `emmcfs`, `fanctl`. They need board gating in init/drvman.
- Boot log ends with `ARM64 repeat fault esr 0x92000006
  far 0x1390c950 pc 0x1097f968` after the WiFi probe failures.
- `adrenofs -t` **hangs** the console (>3 min, no output, ^C
  ineffective; kernel/USB gadget stay alive). Stage prints and
  a timeout are needed before the next attempt; run `-i` first.

## Observed on hardware, 2026-11-18 boot7 (RAM-boot, no flash)

`/tmp/harvey-boot7.img`: rebuilt LKL kernel (#134) with the
Unisoc WiFi gate, Harvey kernel repeat-fault kill, instrumented
`adrenofs`, LM3697 still `status="disabled"`. Observed via
serial MCP (`/dev/serial/by-id/usb-Harvey_Harvey_ARM64_USB_harvey01-if00`):

- Boots to a Harvey `%` console; `echo BOOT7-ALIVE` answered.
  Native log has `DRVMAN gpio/gpu/i2c/rng/tsadc/spi/usb SKIP
  incompatible` (Rockchip services correctly not launched).
- WiFi gate works as a gate: guest log still has the Marlin BSP
  `marlin_init`/`sdiohal_init ok`, but no `sprdwl` SDIO scan
  timeouts and a 45 s console capture was silent. WiFi itself
  remains **unsupported** (no power sequencing / association).
- `adrenofs -i` passes again: exact `qcom,kgsl-3d0@5900000`,
  GPUCC `qcom,gpucc@5990000`, IOMMU `idr0=0x4c017e04`,
  `powered 1`, `why=ok`. `adrenofs -t` (SQE ring) still not
  proven; finer stage prints (`cache sqe/mem done`, `gbif done`,
  `hwcg done`, `uche/cp done`, `wait done`) are now in the tree.
- eMMC/SD as in boot1: HS400 `G1J9R8 58.2 GiB` p1–p77,
  SDHCI at 4784000 with CD GPIO. `/srv/lkl` mounts at
  `/mnt/lkl` (guest root, `/proc`, `/sys`, `/dev/input/event0`,
  `event1` visible). Filesystem-level eMMC proof is still
  pending: the guest only hand-makes input nodes. Block nodes
  (`/dev/mmcblk0*` from `/sys/class/block/*/dev`) are now
  exported by `lkl9p` `mkblocknodes` and need a boot8 test.
- Console caveat: a backgrounded (`&`) blocking `cat` of
  `event1` wedged the rc shell (input echoes, `^C`/DEL do not
  interrupt; gadget/kernel stay enumerated). Do not background
  blocking device reads on this console; recovery is a
  physical power hold back to stock Android, then RAM-boot.

## Observed on hardware, 2026-11-18 boot8 (RAM-boot, no flash)

`/tmp/harvey-boot8.img`: boot7 plus `lkl9p mkblocknodes` and
finer `adrenofs` stage prints. LM3697 still disabled.

- Block export works: after `mkdir /mnt/lkl; mount /srv/lkl
  /mnt/lkl`, guest `/dev` has `mmcblk0`, `mmcblk0boot0/1` and
  `mmcblk0p1`–`p77`. Guest sysfs sizes read back
  (`mmcblk0` = 122142720 sectors).
- eMMC block **read proven**: `cat /mnt/lkl/dev/mmcblk0p1`
  (5 MiB) and `mmcblk0boot0` (4 MiB) to `/dev/null` completed
  and the prompt returned. No filesystem mount yet, and no
  eMMC write attempted (would risk stock Android data).
- Touch: guest `/proc/bus/input/devices` lists `fts_ts`
  (`input1`, handlers `event1`, ABS) and `pm8941_pwrkey`
  (`event0`). Coordinate events still not captured; guest
  `/proc/interrupts` reads empty over 9P.
- `adrenofs -i` passes again (`idr0=0x4c017e04`, powered).
- WiFi corrected (user catch, 2026-11-18): the guest was
  "using the wrong thing" — the shared LKL kernel merged
  `lkl-wifi.config` (Unisoc UWE5622 for Orange Pi/Samsung)
  into the Bengal build. boot11 instead enables the real
  borneo stack in `lkl-bengal.config` (`ATH10K=y`,
  `ATH10K_SNOC=y`, `QRTR`, `RPMSG_QCOM_GLINK_SMEM`,
  `QCOM_SMEM`, `QCOM_SMP2P`, `QMI_HELPERS`, hwspinlock/APCS —
  all verified `=y` after rebuild) and adds
  `wifi@c800000` (`qcom,wcn3990-wifi`, IRQs 358–369,
  `wlan_msa_region@51900000`) to `lkl-bengal-usb.dts` and to
  the `bengal_usb` family device list. Unisoc stays compiled
  for the OPi family but is DT-gated off Bengal. Expected
  first result is ath10k probe + WLFW QMI timeout: the modem
  (which hosts the WLFW service) is not up under Harvey yet,
  and the WCN3990 firmware (`firmware-5.bin`/`board-2.bin`)
  must still be pulled from the phone's `/vendor/firmware`
  via ADB and staged into the guest rootfs.
## Observed on hardware, 2026-11-18 boot13 (RAM-boot, no flash)

`/tmp/harvey-boot13.img`: boot12 plus generic WCN3990
`board-2.bin` (from linux-firmware; `firmware-5.bin` is not
in linux-firmware and phone `/vendor/firmware` needs root —
extraction still pending). Generic
`adb pull` failed (`Permission denied`, production build).

- Firmware staging **works**: guest
  `/lib/firmware/ath10k/WCN3990/hw1.0/board-2.bin` (893528 B)
  present after mount of `/srv/lkl`.
- `adrenofs -d` **DMA PASS**: alloc (pa 0x79c00000), 320 MiB
  arena `segattach`, touch, cache clean, free — all clean,
  prompt returned. The `#P/dma` path itself is healthy.
- `adrenofs -t` then wedged the console exactly at
  `drvdma: map segattach` (arena base 0x69c00000, size
  0x14000000) — i.e. the same attach that `-d` had just done,
  but now in a process that had already run `power_on`
  (GPUCC GDSC/clock register writes). boot14 reorders
  `sqe_test` to attach all DMA **before** `power_on` to test
  this ordering hypothesis.
- ath10k probe result not captured before the wedge; check
  the guest log early on the next boot.

boot14 runbook (built, awaiting power cycle): boot14 =
boot13 + `sqe_test` DMA-before-power reorder. On boot:
(1) `cat /dev/lkllog` early — capture ath10k probe lines;
(2) run `adrenofs -t` FIRST (no `-d`/`-i` before it, to keep
the process history clean). If `map segattach done` prints,
ordering was the wedge; proceed to hw_init stages. If it
still wedges at `map segattach` as the first adrenofs
process, both the power-ordering and prior-process theories
are dead: the remaining suspect is kernel `segattach` state
left by the two LKL guests, and kernel tracing is next.

- `adrenofs -t` **wedged the console at `sqe stage dma`** —
  before the first DMA allocation returned, and this time
  with no input echo at all. This refutes the earlier
  "hw_init wedges" theory: the failure is in the `#P/dma`
  alloc / 512 MiB arena `segattach` path (`drvkit drvdma`),
  or in whatever that path contends with. boot9 adds an
  `adrenofs -d` DMA-only probe (alloc/map/touch/cache/free,
  no GPU power) plus per-allocation prints in `-t`; run `-d`
  first on the next boot.

## Display, GPU, input

| Function | Vendor HW | Harvey path | Status |
| --- | --- | --- | --- |
| MDP / SDE display | `qcom,sde-kms` | none | unsupported |
| DSI ctrl/PHY/PLL | `qcom,dsi-ctrl-hw-v2.4` | none | unsupported |
| Panel ILI9881x (Tianma) | `dsi-panel-moto-ili9881x` | none | unsupported |
| Panel NT36672a (DJN) | `dsi-panel-moto-nt36672a` | none | unsupported |
| Panel FT8009 (DJN BOE) | `dsi-panel-moto-ft8009` | none (name only) | unsupported |
| Backlight LM3697 | `ti,lm3697` @37 i2c1 | LKL, DTS disabled | unsupported |
| Backlight AW99703 | `awinic,aw99703-bl` @36 | none | unsupported |
| GPU Adreno 610 | `qcom,kgsl-3d0` | native `adrenofs` /srv/adreno | experimental |
| Touch FocalTech FT8009 | `focaltech,fts` SPI2 | LKL `FTS_0FLASH_MMI` | experimental |
| Touch Ilitek | `tchip,ilitek` SPI2 | LKL `ILITEK_0FLASH_MMI` | experimental |
| Touch Novatek NT36672 | `novatek,NVT-ts-spi` SPI2 | config disabled | unsupported |
| GPIO keys (vol/pwr) | `gpio-keys` | none | unsupported |
| Fingerprint FPC1020 | `fpc,fpc1020` | none | unsupported |
| Fingerprint Egis ET516 | `egistec,et516` | none | unsupported |

Panel is selected at runtime by `/chosen/mmi,panel_name`
(`tm_ili9881x_video_display`, `djn_nt36672a_video_display`, or
`djnboe_ft8009_video_display`); the touch chip follows the panel.
Display is the largest missing native/LKL family: SDE+DSI in LKL
(DRM MSM) or a native fixed-mode DSI driver are the two routes —
undecided, see repo plan.

## Connectivity, audio, sensors, power

| Function | Vendor HW | Harvey path | Status |
| --- | --- | --- | --- |
| WiFi/BT WCN3990 | `qca,wcn3990` + `qcom,icnss` (stock DT) | LKL `ath10k_snoc` stack enabled, not yet probed | experimental |
| WiFi wrong-driver gate | Unisoc UWE5622 (OPi/Samsung, in shared lkl.o) | DT marker gate in guest | supported (gate) |
| Modem / IPA / RMNET | `qcom,ipa`, PIL, GLINK | none | unsupported |
| GPS (in modem) | modem subsystem | none | unsupported |
| NFC | `nfc` pinctrl nodes | none confirmed | unsupported |
| Audio DSP (Q6/Bolero) | `qcom,bolero-codec` | none | unsupported |
| Audio codec WCD937x | `qcom,wcd937x-codec` | none | unsupported |
| Smart PA AW882xx | `awinic,aw882xx_smartpa` @34 | none | unsupported |
| Sensors (accel/gyro/etc.) | I2C/SPI + `wake-capable-sensor` | none enumerated | unsupported |
| SAR sensor | `abov,abov_sar_mmi_overlay` | none | unsupported |
| Camera CAMSS/CSI/CCI | `qcom,cam-*`, csiphy, cci | none | unsupported |
| Camera flash | `qcom,camera-flash` | none | unsupported |
| Charger SMB5 (PMI632) | `qcom,qpnp-smb5` | none | unsupported |
| Fuel gauge QG | `qcom,qpnp-qg` | none | unsupported |
| Vibrator | `qcom,qpnp-vibrator-ldo` | none | unsupported |
| Tri-LED / LPG | `qcom,tri-led`, `qcom,pwm-lpg` | config disabled | unsupported |
| Video encode/decode | `qcom,msm-vidc` (Venus) | none | unsupported |
| Crypto QCE / QSEECom | `qcom,qcrypto`, `qseecom` | none | unsupported |

## What "all drivers working" requires, in order

1. Boot Harvey on the phone and capture the console log
   (flash `harvey-boot.img` to the inactive slot; keep stock
   `boot.img` for recovery). No Bengal driver can be called
   `supported` without an on-hardware log.
2. Fix the GENI I2C first-write wedge; re-enable LM3697.
3. Prove eMMC read/write + SD detect in `bengal_usb` LKL guest.
4. Prove DWC3 gadget console in `bengal_ctl` (recovery path).
5. Touch: confirm panel/touch pairing on this unit, prove
   FocalTech or Ilitek reports coordinates.
6. Display: choose LKL DRM/MSM vs native fixed-mode DSI, then
   panel init + LM3697/AW99703 backlight.
7. Adreno: extend `adrenofs` from register probe/SQE test to
   job submission usable by a compositor.
8. Then, each as its own family: WiFi (ICNSS/WCN3990), audio
   (Bolero/WCD937x/AW882xx), sensors, fingerprint, charging,
   camera, modem. Modem and camera are subsystem ports, not
   single drivers, and are last.
