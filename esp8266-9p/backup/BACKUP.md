# Stock firmware backup

Read-only dump taken before any flashing. Restoring this file returns the
board to the stock ESP-AT firmware.

## Device

- Chip: ESP8266EX, 160 MHz, 26 MHz crystal, Wi-Fi
- MAC: 48:3f:da:7e:39:68
- Chip ID: 0x007e3968
- Flash: 4 MB, manufacturer 0x5e, device 0x4016
- Port: /dev/ttyUSB1 (Silicon Labs CP2102, USB serial 0001)
- Stock firmware: AT version 1.3.0.0 (Jul 14 2016), SDK 2.0.0, v1.0.0.3,
  Mar 13 2018 09:37:06
- Stock mode at backup time: AT+CWMODE = 2 (SoftAP)

## File

- Name: esp8266-at-stock-483fda7e3968.bin
- Size: 4194304 bytes (full 0x0-0x400000 flash range)
- SHA-256: 8664e475c46bfc4cc0695759987b3cfcc0b9adbf6cc2b6e31bcc2d9c9e06d5a6
- Verification: a second full read was byte-identical (cmp clean, same
  SHA-256). esptool image-info: ESP8266 image v1, entry 0x40100438,
  flash 4 MB / 40 MHz / QIO, checksum valid.
- After the dump the board was hard-reset and answered AT / AT+GMR again,
  so the stock firmware is still running.

## Restore (only run this deliberately)

From the repo root:

    esp8266-9p/.venv/bin/python -m esptool --port /dev/ttyUSB1 \
      --baud 460800 write-flash 0x0 \
      esp8266-9p/backup/esp8266-at-stock-483fda7e3968.bin

Note: esptool command spelling varies by version. In esptool 5.x the
hyphenated forms above (chip-id, flash-id, read-flash, write-flash,
image-info) are correct; older 4.x releases use underscores.

## Tool environment

    cd esp8266-9p
    uv venv .venv
    uv pip install --python .venv/bin/python esptool
    .venv/bin/python -m esptool version

