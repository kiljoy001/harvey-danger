# B13 - /mem: memwatch lifted into the Phase A 9P firmware

Status: verified over the air on a shared AP (see section 5).
Board restored to stock AT.

## 1. Namespace change (design note)

`/mem` is added to the frozen namespace as a read-only debug
file, recorded in `doc/DESIGN.md` section 5. It has no write
side and no ctl semantics. Content is the memwatch ring dump:

    # memwatch samples N total T minfree F maxarena A maxstack S
    # seq tick ev free arena arenahigh q stack
    <one line per sample>

Event markers: 0 = idle, 1 = 9P client connected. The q and
stack columns are 0 on this NONOS substrate (no p9 procs); the
kerntest firmware fills them on the Phase C substrate.

## 2. Implementation

- `firmware/sdkbuild/esp9srv/user/esp9_mem.{c,h}`: a NONOS
  `ETSTimer` fires every 100 ms and only appends a sample
  (system heap free, the 9P server arena used/high, event
  marker). It never prints from timer context. Rendering into an
  8 KB static buffer happens on 9P read via `esp9_mem_read`.
  The sampler start is tied to server start in `user_main.c`;
  client connect/disconnect flip the event marker.
- `esp9_files.c` gains `{ "/mem", 12, 0, 0, rd_mem, NULL }`.
- `memwatch.{c,h}` are linked from `firmware/kernel/` unchanged,
  so the Phase A firmware and the Phase C kerntest share one
  ring implementation.
- `build-esp9srv.sh` gets the stale-`eagle.app.flash.bin` fix
  from B12 (`gen_appbin.py` appends; rebuilds had been silently
  doubling this image too).

The ring holds 128 samples at 100 ms, i.e. a 12.8 s window.
That is a deliberate in-action capture window, not a history log.

## 3. Verification

Host: `test_esp9_mem: all tests passed` — header, minfree and
maxarena reductions, and exact sample-line rendering against a
real pp_arena. The shared ring tests from B12 still pass.

On-device image `user1-phaseA.bin`, 232196 bytes, SHA-256
`2de7553f1da0398bba829500dfd3caf5104d9c9b1fc77a36e506dfc1b0d84599`,
first header bytes `ea 04`. `.bss` grows from 35536 to 47872
bytes (memwatch ring plus the 8 KB render buffer); that cost is
now measured, which is the point of the file.

Flashed at `0x1000`, UART capture:

    ESP9: phaseA boot sdk=2.0.0(656edbf)
    mode : sta(48:3f:da:7e:39:68) + softAP(4a:3f:da:7e:39:68)
    dhcp server start:(ip:192.168.4.1,...)
    ESP9: 9P server listening on tcp 564

The firmware boots, starts SoftAP+station, listens on 564, and
survives the 12 s capture with the sampler timer running (no
watchdog resets).

That first flash was boot-verified only. The over-air read is
in section 5.

## 4. Restore

Full 4 MB stock backup rewritten at `0x0` (hash verified).
`AT` -> `OK`, `AT+GMR` -> original 1.3.0.0 / SDK 2.0.0 /
Mar 13 2018. Board state: stock AT.

## 5. Same-AP attempt (follow-up)

The host can stay on its network if the ESP's station side joins
the same AP. Support for that is now in the firmware: optional
build-time provisioning (`ESP9_PROVISION_SSID` /
`ESP9_PROVISION_KEY` in `esp9srv/include/user_config.h`, off by
default) plus a Wi-Fi event handler that logs the station IP to
UART on GOT_IP.

Tested against the host's active AP, `xfinitywifi`: the ESP
boots, tries to join, and reports `station disconnected reason
201` (NO_AP_FOUND) repeatedly. Root cause is band, not code:
both visible `xfinitywifi` APs are on channel 157 (5 GHz), and
the ESP8266 is 2.4 GHz-only. The board was restored to stock and
the test provisioning removed; the default Phase A image was
rebuilt (SHA-256
`b879c3be8b15d0c2c0296677553ef00564efcdb557f44216e3f677135bc3c0f0`).

Provisioned instead with `Linksys01314` (2.4 GHz WPA2, key
supplied by the user for this test and removed afterwards).
The ESP joined, logged `station ip 192.168.1.146`, and the host
reached `192.168.1.146:564` over the LAN. A Python 9P2000
client then read `/version`, `/status`, and `/mem` in full.
Samples taken while the client was connected show `ev=1` and
free heap 21832 against 22232 idle: the connection itself
costs about 400 bytes, visible in the file it was read from.
A slow client and three consecutive fast-client runs all
completed (`CLIENT9P-LAN: OK`).

This test also exposed a real Phase A transport bug. NONOS
espconn permits one outstanding send: srv9p writes a reply as
a 4-byte header then a body, and the second `espconn_send`
failed with `-7 ESPCONN_MAXNUM`, so the device accepted
connections and immediately dropped them. (Phase A had never
actually served a client on-device before; B9 was blocked
earlier.) The fix in `user_main.c` queues reply bytes, waits
1 ms to coalesce header and body, sends one chunk, and pumps
queued continuations from the sent callback. An earlier
variant that pumped from a posted task/timer stalled
intermittently under a fast client; the sent-callback pump
passed 3/3 fast runs. The build scripts now also fail loudly
on a stale ELF instead of packaging one after a compile error.

Default Phase A image after key removal: 232804 bytes,
SHA-256
`a82e36ffddbbb59af43d6ea8864348e0eb5bfeb03be87f4e6695d8edfa8f0726`.

## 6. Next steps

1. Exercise `/wifi/ctl connect` over this LAN path now that
   serving a client on-device is proven, instead of build-time
   provisioning.
2. REGDUMP from the exception vector (B12 step 3), where
   `epc1`/`exccause` are meaningful.
3. Add `/mem` to the host mock and srv9p harness tables so the
   Python client test covers it before the next flash.
