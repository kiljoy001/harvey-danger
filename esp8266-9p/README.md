# esp8266-9p

Plan9ized firmware for the ESP8266EX behind the CP2102 on /dev/ttyUSB1
(MAC 48:3f:da:7e:39:68). The stock OS is replaced by a small Plan 9
system: it serves a 9P namespace over the improved IL (primary) and TCP
(fallback), port 564. The intent is that a Harvey (or any 9P) host
mounts it and controls it by reading and writing text files; see
"Interop" below - no external client has mounted it yet. It is not a
Harvey kernel port - the chip has no MMU - but everything above the
Wi-Fi blobs is Plan 9-shaped.

Status: WORKING SYSTEM, flashed and verified many times
(board is normally restored to stock AT between rounds;
restore procedure in backup/BACKUP.md). Our substrate OS
boots on the chip today - see "What boots today" below.
The remaining closed code is the Wi-Fi driver layer:
of libpp's nine objects one (esf_buf) is already ours;
the re/B*.md reports are the blow-by-blow record.

## Provenance

Parts of this tree were written with a language model in the loop, and the
first comment on any repository like this is "AI slop". The useful answer
is not to deny it, it is to point at the board: the image flashes, the
namespace serves, the stock firmware is backed up and restorable, and the
reverse-engineering reports are tied to byte offsets. **Slop that works is
still a working system**; slop that only compiles is not, and the only
claim made here is the one the hardware can check.

- Design: doc/DESIGN.md (system design; status there too)
- Kernel plan: doc/PLAN9-OS.md (p9_os, OSI seam, milestones)
- Frame/DMA spec: doc/SPEC-WDEV-DMA.md
- Reverse engineering: re/B0..B29 reports, re/inventory/
- Firmware source: firmware/ (kernel/, esp9/, srv9p/,
  libpp/ clean-room pieces)

## What boots today

ROM -> SDK second-stage bootloader -> our esp9 image.
In the image, the FreeRTOS kernel API is implemented by
our p9 substrate (firmware/kernel: p9_os scheduler over
Xtensa setjmp coroutines, freertos_shim, osi_wifi), so
the closed Wi-Fi blobs (libpp/libnet80211/libphy) run
as drivers on *our* OS, not on Espressif's. On top:
the Plan 9 procs srv9p (9P over TCP 564), il9p (9P over
improved IL, IP protocol 40 - host-tested engine,
listener verified on device, Harvey interop pending),
and cons (UART provisioning). Credentials live in the
/wifi/creds file (NVS-backed), loaded on boot; nothing
is compiled in. It is not a Harvey kernel port - the
chip has no MMU - and the blob-free link (replacing
the remaining driver libraries) is the open campaign.

## Scope: two projects, not one

This project is (a) a reverse-engineering artifact -
proving exactly what the closed Xtensa/Wi-Fi code does
until it can be replaced - and (b) a Wi-Fi 9P node: a
server appliance with one served namespace. It has no
per-process namespaces and needs none; the mounting
host's kernel owns mount tables.

The separate thesis - a Plan 9-style MCU *operating
system* with per-process userspace namespaces, slim
Chan/Fid structures, and MPU/TrustZone isolation (the
2-syscall kernel) - lives outside this tree (lux9) and
wants different silicon (the ESP8266 has neither MMU
nor MPU). Ideas cross between the two (the srv9p core
already embodies the slim fid/flash-table model), but
neither blocks the other.

## Quick facts

- Chip: ESP8266EX, 4 MB flash, stock ESP-AT 1.3.0.0 / SDK 2.0.0
- Backup: backup/esp8266-at-stock-483fda7e3968.bin
  SHA-256 8664e475c46bfc4cc0695759987b3cfcc0b9adbf6cc2b6e31bcc2d9c9e06d5a6
- Flashing/logging cable: /dev/ttyUSB1 at 115200 8N1

## Tools

    uv venv .venv
    uv pip install --python .venv/bin/python esptool
    .venv/bin/python -m esptool --port /dev/ttyUSB1 --baud 115200 chip-id


## Scope: two projects, deliberately separate

This tree is **esp9**: a reverse-engineering artifact and a 9P node. It
serves one namespace, owned by the server. The client owns the mount
table - on Harvey that is `Pgrp`/`Mhead`/`Mount` in the kernel.

It is **not** the 2-syscall, namespace-dynamic kernel. That thesis
(`sys_call`/`sys_serve`, userspace namespaces, per-process mount state)
belongs to the lux9 tree and needs silicon this chip does not have. The
LX106 has no MMU and **no MPU**: there is no hardware isolation of any
kind here. The eight procs share one address space, and a blown stack is
caught by a guard word after the fact rather than prevented.

Keeping the two separate matters. Adding a local namespace to esp9 would
add machinery for zero users, and citing esp9 as evidence for the
separation thesis would be a category error.

## Interop: pending, and the next milestone

Every 9P conversation so far has been our server against our own client
(`firmware/srv9p/test/bench9p.py`, `mock9p/`). A client and server we
wrote agreeing with each other proves the two agree with each other.

An external client is the test that counts:

    tools/interop9p.sh 'tcp!192.168.4.1!564'

or mount it:

    9pfuse -a 'tcp!192.168.4.1!564' /tmp/esp
    ls -l /tmp/esp/dev
    fusermount -u /tmp/esp

Until that passes, this is "serves 9P", not "is a Plan 9 fileserver".
The difference is the external client, and it is an afternoon.
