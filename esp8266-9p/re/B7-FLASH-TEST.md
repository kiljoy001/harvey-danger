# B7 - flash test: libpp test firmware booted on-device, stock restored

Continues B6. This stage crossed the approval gate you opened
("we have a back up and can recover, right?" -> confirmed ->
"then go ahead"). Exact scope executed: flash the B6 test image,
read its UART output, restore the stock backup in full, verify
AT answers. All four steps are done; the board is stock again.
Prior stage caveat stands: B6's esptool-packaged image and its
SHA-256 (eb3d51cc...) **did not boot** - see section 1. Only the
gen_appbin-format image below was ever booted successfully.
The non-booting file was deleted from testapp/bin/.

## 1. First attempt failed: esptool image does not boot boot-1.6

Flashed SDK boot_v1.6 + esptool elf2image user1 + init/blank.
Serial at 74880 (pyserial; ROM log baud) showed a boot loop:

    2nd boot version : 1.6
      SPI Flash Size & Map: 4Mbit(256KB+256KB)
    jump to run user1 @ 1000
    rf_cal[0] !=0x05,is 0xFF        <- then reset, repeats

Root cause, from comparing stock and esptool headers byte by
byte: the v2 first header's byte 3 is the **user-bin number**
(stock: 0x01; gen_appbin writes `app`), and boot 1.6 derives the
flash map from it. esptool writes its own flash-size code (0x40)
there, so boot 1.6 got map 0 (512 KB), computed the RF-cal sector
inside blank flash, and looped. The second headers differ too
(stock/gen: byte3 = (size_map<<4)|clkdiv = 0x60 for map 6;
esptool: 0x40). Lesson recorded in the build script: SDK-format
packaging is not interchangeable with esptool packaging on this
bootloader, however valid both checksums are.

Fix: ported the SDK's own tools/gen_appbin.py to Python 3
(lib2to3 + manual bytes/str fixes; kept in
`firmware/sdkbuild/gen_appbin_py3.py`, Espressif copyright header
intact) and regenerated. The resulting image matches stock
format byte-for-byte in structure: first header EA 04 00 01,
second E9 03 00 60, 223,732 bytes, SHA-256
81bbe9360a612bfff513131f3a9617c3378e804b61faf539859cd4453a6d1881.
Also used the **stock boot image** (re/images/boot.bin, from the
backup) rather than the SDK's generic boot_v1.6.bin, to match
what this board shipped with.

## 2. Second attempt: success

Flashed stock boot.bin @0x0, user1-gen.bin @0x1000,
esp_init_data_default @0x3fc000, blank @0x3fe000 (all
hash-verified by esptool). UART capture after a DTR/RTS reset:

    2nd boot version : 1.6
      SPI Flash Size & Map: 32Mbit(512KB+512KB)
    jump to run user1 @ 1000
    rf cal sector: 1019
    ESP9-LIBPP-TEST boot sdk=2.0.0(656edbf)
    ESP9-LIBPP-TEST pass desc-chain
    ESP9-LIBPP-TEST pass offer-owner
    ESP9-LIBPP-TEST pass offer-len-fcs
    ESP9-LIBPP-TEST pass offer-not-done
    ESP9-LIBPP-TEST pass reclaim
    ESP9-LIBPP-TEST pass pool-alloc
    ESP9-LIBPP-TEST pass pool-offer-len
    ESP9-LIBPP-TEST pass pool-free
    ESP9-LIBPP-TEST ALL-PASS heap=52328
    mode : softAP(4a:3f:da:7e:39:68)   <- SDK default after user_init

So: the clean-room libpp (pp_lldesc + pp_esf, built in B5/B6
behind blob init) runs correctly on the real ESP8266EX. The
trailing softAP is the NONOS default when user code does not set
a Wi-Fi mode; the plan9ized firmware will set it deliberately.

## 3. Restore: exit test passed

Restored the whole 4 MB stock backup with the single command
from backup/BACKUP.md (write hash-verified by esptool), then
re-ran the AT probe: AT -> OK, AT+GMR -> the original
1.3.0.0 / SDK 2.0.0 / Mar 13 2018 string, MAC
48:3f:da:7e:39:68, CWMODE 2 - byte-for-byte the behaviour from
before the flash. The recovery path is now proven end-to-end on
hardware, not just by cmp of two reads.

## 4. State and next steps

- Board: stock AT firmware, exactly as found.
- `firmware/sdkbuild/build-testapp.sh` now builds AND packages
  correctly (gen_appbin port); delete-and-rebuild is the repeat
  test if you want it rerun.
- B6 is amended by this report; its "Approval gate" section has
  been satisfied for the test image only. Any further flashing -
  DESIGN phase 2 read-only 9P/IL firmware - is a new gate.
- Next, in DESIGN order: phase 1 host 9P mock to freeze the
  namespace, then the phase-2 firmware (improved IL primary,
  TCP fallback, port 564), then blob-free WDEV work per
  SPEC-WDEV-DMA section 5.

