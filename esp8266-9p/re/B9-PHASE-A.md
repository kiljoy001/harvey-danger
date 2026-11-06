# B9 - Phase A firmware built and boot-verified; Wi-Fi client test blocked

Continues B8. The frozen namespace now runs in firmware.
Board state at the end of this stage: **stock AT, restored and
verified**, as after every stage. Details below.

## 1. What was built

`firmware/sdkbuild/esp9srv/` (repeatable via
`firmware/sdkbuild/build-esp9srv.sh`):

- `user/esp9_files.c` - the Pp9File table for the frozen
  namespace with **real** callbacks: /status and /wifi/status
  from wifi/system calls (mode, IPs, RSSI, uptime, heap,
  clients), /version from the SDK version, /net/status from
  SoftAP IP/MAC. /wifi/ctl verbs act: `scan` (results land in
  /wifi/scan), `connect <ssid> <key>`, `disconnect`. /ctl:
  `reboot`, `ap on|off`, `factoryreset`. Unknown verbs return
  the frozen Rerror strings.
- `user/user_main.c` - NONOS app: UART 115200, on init-done set
  STATIONAP with open SoftAP `ESP9-3968` ch 1, start an espconn
  TCP server on **port 564**. One client; replies via
  espconn_send.
- `srv9p` gained an event-driven entry `pp9_input` (incremental
  header/body assembly) plus `pp9_reset`, because NONOS offers
  only async espconn, not blocking sockets. The blocking
  `pp9_serve` and the core are unchanged, so TCP/IL transports
  and the host harness all share one implementation.
- A layout fix with a lesson: functions needed IROM placement
  (`PP9_ICACHE` / ICACHE_FLASH_ATTR); the first link overflowed
  the 32 KB IRAM .text. srv9p.h uses `__has_include` so the core
  also compiles copied into an app tree.
- One self-inflicted incident, recorded: a scripted annotation
  pass mangled function names in esp9_files.c/user_main.c. Both
  files were rewritten cleanly from scratch and the build then
  passed with no warnings-as-errors.

Image: bin/user1-phaseA.bin, 231,140 bytes, SDK gen format
(first header EA 04 00 01), SHA-256
4c6df7f064263aeff7f1ad3c765df04121e0e874142ab84b0ab6c24acadfb9a4.

## 2. Verified

- Build: full SDK link clean under -Werror; image in the exact
  format that boots boot 1.6 (B7 lesson).
- Boot (serial capture, /dev/ttyUSB1 at 115200):
  `ESP9: phaseA boot sdk=2.0.0`, `mode : sta(...) + softAP(...)`,
  `dhcp server start:(ip:192.168.4.1,...)`, and
  `ESP9: 9P server listening on tcp 564`. The firmware runs,
  the AP comes up, the server starts.
- Protocol: the identical srv9p core passed the full
  independent Python 9P client suite on the host (previous
  stage), over both the blocking and (by construction) the
  shared message handler used by pp9_input.

## 3. Not verified: the over-Wi-Fi client test

The workstation's wlan is a managed connection (currently on
`xfinitywifi`); joining ESP9-3968 needs wireless admin rights
- `iwconfig ... essid` returns Operation not permitted,
  wpa_cli control is root-owned, and there is no sudo on this
  machine by design. So no 9P client has yet spoken to the
  device over the air. The espconn send/recv glue specifically
  (as opposed to the core) is therefore **unverified**, and for
  that reason the stock firmware was restored (hash-verified
  write; AT probe returns the original answers) instead of
  leaving Phase A flashed.

To close this row, any one of: connect a phone/laptop to
ESP9-3968 and run a 9P client against 192.168.4.1:564 after
flashing bin/user1-phaseA.bin (plus stock boot/init at the B7
offsets); or give this host a second, unmanaged Wi-Fi adapter.

## 4. Next

- Close section 3 (first over-air mount; on Harvey that is also
  the first IL-path rehearsal, though IL itself is Phase B).
- Phase B (PLAN9-OS section 4): Proc/Rendez/QLock + OSI
  adapter over them, IL in devip, FreeRTOS linked out. That is
  a substrate project of its own; Phase A here ran on NONOS
  blob init as the pragmatic substrate, a deviation from
  PLAN9-OS's RTOS-first Phase A recorded in B8/B9 - the srv9p
  core and file table carry over unchanged either way.

