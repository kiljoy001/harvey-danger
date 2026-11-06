# B25 - Credentials live in a file, not the build

Status: DONE (implementation + build + on-device
store/load/forget cycle + stock restore verified).

## Change

The compile-time provisioning path is gone:
blob_config.h deleted (it contained only commented
placeholder defines), all ESP9_PROVISION_* code removed.
Credentials now live in a file and load on boot:

- Backing store: NVS namespace `esp9wifi` (keys ssid/key)
  in the existing NVS flash partition; nvs_flash_init
  already ran at boot. This is the credentials file in
  flash; /wifi/creds is its namespace face.
- New file /wifi/creds (DESIGN section 5 amended):
  read returns `ssid <ssid>` and `key set|unset` - the
  key itself is never readable from any file. Write is
  `<ssid> <key>` and means store-and-join.
- /wifi/ctl connect now stores via the same provision()
  path before joining; new verb `forget` disconnects and
  erases the file; /ctl `factoryreset` erases and reboots
  (previously reboot-only).
- Boot: creds_load() after esp_wifi_init; if a file
  exists, esp_wifi_set_config from it and auto-join
  after start (with disconnect-retry, as before). The
  bench alive loop now keys off have_creds instead of a
  preprocessor define.
- Bootstrap: a `cons` proc reads UART0 lines with the
  same verb grammar (connect/scan/disconnect/forget via
  wr_wifi, reboot/factoryreset via wr_ctl), so a cabled
  device with no network can be provisioned. This is the
  wired half of DESIGN section 3; the SoftAP half is
  still open.

All in firmware/esp9/main/blob_main.c; no new files.

## On-device verification (/dev/ttyUSB1, then restored)

Flashed esp9 (hash verified), UART at 74880:

Boot 1 (fresh):
    creds: none stored; provision via /wifi/creds, ...
    MILESTONE scan-ok / 9p-listening / done

UART `connect TestNet testpass123`:
    console: connect TestNet testpass123 -> ok
    EVT wifi id=5 reason=201 (NO_AP_FOUND - dummy SSID,
    expected) with reconnect retries

Reboot (UART `reboot`):
    creds: loaded ssid TestNet from file
    connect rc=0, auto-join attempts follow

UART `forget`, then `reboot`:
    console: forget -> ok
    creds: none stored; ...

So the file round-trips: write over the wire, reboot,
loaded from flash, cleared on forget. A join against a
real AP was not tested (no credentials available to this
session); reason 201 confirms the stored SSID is what
the blob tried to join. The provisioned bench from B19
remains to be re-run with runtime provisioning - type
the real `connect` over UART once and every later boot
joins on its own.

Build: memory gate green (bss 68640, headroom 27088).
Host tests: test_pp, test_esf_buf, test_kernel pass.
Stock backup reflashed (hash verified); board answers
AT / AT+GMR (AT 1.3.0.0, SDK 2.0.0). No credentials,
dummy or real, remain on the device. In esp9 source the
PROVISION grep is clean (blob_config.h deleted). The
retired rtossrv lineage (firmware/rtossrv, the B14
vehicle superseded by esp9) still carries the same
compile-time pattern in rtos_main.c/rtos_config.h; it
was left untouched this round.

## Correction (from B26)

The unprovisioned boot above did not in fact serve:
the cons proc took the last of 6 proc slots and srv9p
never started (`p9_os: out of procs`). The scan and
creds lines were real; any 9p-listening line was not
verified that round. Fixed in B26 (MAX_XPROCS 8).

## Notes

- Keys sit in plaintext NVS flash, readable by anyone
  with the cable and esptool (DESIGN section 8 already
  says: dedicated IoT SSID).
- The host mock (mock9p) still serves the pre-B25 frozen
  namespace without /wifi/creds; updating it is a
  follow-up, not required for firmware work.
