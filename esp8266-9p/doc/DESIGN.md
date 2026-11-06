# Plan9ized ESP8266 firmware - design

Status: IMPLEMENTED AND RUNNING. This document remains the
design record; where reality diverged, the ../re/B*.md reports
and amended sections below say so. The firmware boots on the
device, joins Wi-Fi from the /wifi/creds file, and serves the
section 5 namespace over TCP 564 (bench-verified, B19/B26/B28)
and improved IL (engine host-tested, listener on device, B29;
Harvey interop not yet tested). The p9 substrate replaced the
FreeRTOS kernel API under the blobs (PLAN9-OS Phases A-C);
replacing the blob driver libraries is under way (esf_buf
done, B24; RX stall fixed, B28). The board runs stock ESP-AT
between test rounds only because we restore it after each
round; a verified full backup is in `../backup/` (see
`../backup/BACKUP.md`).

## 1. What "plan9ized" means here

Goal (user decision): **replace the OS on the board with a Plan 9 one.**
The stock ESP-AT / NONOS firmware is removed entirely. What replaces it
is a small Plan 9 system that is the board's whole OS:

- Everything is a file. The device serves one 9P namespace.
- Control is text written to a `ctl` file, not binary ioctls or AT verbs.
- State is read back as text from small files.
- The host needs no vendor tool after flashing: it mounts the device.
- Internal work is split into processes (tasks) talking to file servers,
  in Plan 9 style, not one monolithic AT interpreter.

Hard limits, stated plainly so the design stays honest:

- A literal Harvey/Plan 9 kernel port is impossible on this silicon.
  The ESP8266EX has no MMU and about 50-80 KB of usable RAM. Harvey has
  386/aarch64/amd64/riscv ports and no Xtensa port (see `sys/src/9/`).
  So this is a Plan 9 system written for the chip, not a recompile of
  the Harvey kernel.
- Wi-Fi starts out driven through Espressif's closed blobs
  (libnet80211/libphy/libpp linked by the NONOS/RTOS SDKs). Per user
  decision, **those blobs will be reverse engineered and replaced**
  (section 6.1); until that work lands they are an interim substrate,
  not a permanent layer. The mask-ROM code inside the chip cannot be
  replaced, only understood and called - it is treated like hardware.

Chosen transport (user decision): **9P over the improved IL first, TCP
as fallback**, both on port 564 (section 4).

## 2. Hardware facts (measured, not assumed)

- Chip: ESP8266EX, MAC 48:3f:da:7e:39:68, chip ID 0x007e3968
- Flash: 4 MB (0x5e / 0x4016), crystal 26 MHz
- Host link for flashing/logs: CP2102 UART on /dev/ttyUSB1, 115200 8N1
- DTR/RTS are wired to GPIO0/EN: an ESP-style reset works from the host,
  and esptool syncs without touching the board.
- Module form factor is unknown (ESP-01 vs ESP-12 etc.), so the design
  must not assume which GPIOs are broken out. GPIO support is optional
  and off by default until the module is identified.

## 3. The bootstrap problem and its answer

9P over Wi-Fi has a chicken-and-egg problem: the host can only mount the
device after the device has joined a network. Design:

1. On first boot (no stored credentials) the firmware starts its own
   open SoftAP named `ESP9-3968` (last two MAC bytes) and serves 9P at
   `192.168.4.1!564`. Current stock firmware already boots in SoftAP
   mode, so this matches existing behaviour.
2. The host joins that AP, mounts the namespace, and writes the real
   network credentials to `/wifi/ctl` (section 5).
3. The firmware joins the target network as a station, keeps the SoftAP
   up until the station link has an IP, then drops the SoftAP (write
   `ap off` to `/ctl` to force it earlier, or keep it: see open
   questions).
4. Later boots go straight to station mode if credentials are stored;
   holding no credentials (or a `factoryreset` via `/ctl`) returns to
   step 1. The UART console always offers the same text controls as a
   wired fallback, so a Wi-Fi mistake cannot brick access.

Credentials are write-only: they can be set through `/wifi/ctl` but are
never readable back from any served file.

## 4. Transport: improved IL primary, TCP fallback

Both transports carry the same 9P byte stream on **port 564**. The host
chooses: Harvey dials `il!<ip>!564`, anything else dials
`tcp!<ip>!564`. The firmware listens on both from first boot, including
in SoftAP bootstrap mode (section 3).

### 4.1 Improved IL (primary)

IL is Plan 9's own reliable datagram protocol, and the reason to prefer
it here: it was designed for 9P - message-oriented, no byte-stream
head-of-line blocking, tiny header. Harvey's `sys/src/9/ip/il.c` is the
reference implementation and carries one extension over classic IL,
which this firmware must implement to interoperate at full rate:

- IL runs directly over IP, **protocol 40** (`IP_ILPROTO` in il.c).
  It is not UDP. On the ESP8266 this means a raw-IP lwIP PCB
  (`IP_PROTO` 40), not a UDP socket.
- Header is 18 bytes (`IL_HDRSIZE`): checksum, length, type, spec,
  source/dest ports, sequence id, ack id. Packet types: `Ilsync,
  Ildata, Ildataquery, Ilack, Ilquery, Ilstate, Ilclose`.
- Connection by SYNC exchange (server side: `Ilsyncee` - accept SYNC,
  reply SYNC, established on ACK/DATA), with retransmit, delayed ACKs
  (100 ms), queries to probe a silent peer, and close handshake.
  Timers in il.c: 50 ms tick, death after repeated unanswered queries.
- **"Improved" = the `Ilflow` credit extension** (il.c:80-81, 803-828):
  if both SYNCs set `ilspec & 0x80`, then ACK and STATE packets carry
  an extra 8-byte cumulative count of bytes the receiver has consumed.
  A sender that honours it never overruns the peer's receive queue
  (`Maxrq` 64 KB on Harvey). The ESP8266 must set `Ilflow` on its
  SYNC and append/accept this credit field; its advertised credit is
  small (its whole receive queue is a few KB), which is exactly the
  case the extension exists for. A classic-IL-only peer still works:
  no `Ilflow` bit, no credit bytes, window rules revert to classic IL.
- IL preserves message boundaries: one IL data packet carries exactly
  one 9P message (fragmenting never needed because `msize` stays well
  under the 1500-byte path MTU - see 4.3).
- One IL connection in v1. Harvey's side is a full multi-connection
  stack; the ESP side is a deliberate minimal subset: states Closed,
  Syncee, Established, Closing, plus retransmit of the single unacked
  head block in the spirit of il.c's `ilrexmit`.

Risk note: Linux has no IL stack, so IL cannot be tested from the
development workstation. That is what the TCP fallback is for. First
IL interop test target is a real Harvey host dialing the device.

### 4.2 TCP (fallback)

Plain lwIP TCP listen on 564. It exists for Linux/9pfuse development,
debugging, and any client without IL. Same fid limits and `Rerror`
behaviour as IL. TCP may be disabled at build time once IL is proven,
to reclaim RAM; it is not disabled by a runtime ctl in v1.

### 4.3 9P subset (both transports)

- 9P2000 (not .u, not .L) to stay close to Plan 9: `Tversion, Tattach,
  Twalk, Topen, Tread, Twrite, Tstat, Twstat (rename rejected), Tclunk,
  Tflush, Tremove (rejected except for session files if any)`.
- `Tauth`: v1 answers "no authentication required" (`Rattach` directly).
  This is a trusted-LAN device; see section 8.
- `msize`: 2048 bytes default, negotiated down safely. Static buffers;
  no per-request heap growth. 2048 also keeps every 9P message inside
  one IL packet on a 1500-byte MTU after IP+IL headers.
- Limits sized for the chip: 1 client per transport in v1 (a second
  connect gets a clean refusal), 16 fids, 4 open files. Everything
  fails with a Plan 9 style `Rerror` string, never a hang.
- Little-endian wire format, one request at a time per connection;
  `Tflush` honoured for the in-flight tag.

## 5. Namespace (v1) - FROZEN

Frozen by Phase 1: the host mock (esp8266-9p/mock9p, Go) serves
exactly this namespace and passed an independent Python 9P2000
client test (version, attach, walk, open/read, stat, ctl write
with bad-verb rejection, nonexistent-file walk rejection).
Changes from here need a design note, not just code. Phase A
firmware implements this list; `/gpio` stays out until pinout
is known (section 2).

All files are synthetic, generated on read. Sizes in `Tstat` are the
exact byte counts a read would return at that moment, or 0 for `ctl`
files where Plan 9 convention allows it.

```
/                       directory
/ctl                    write: reboot | ap on | ap off | factoryreset
                               (factoryreset erases /wifi/creds, B25)
                        read:  same text as /status (convenience)
/status                 read:  mode, ssid, ip, rssi, uptime, heap, clients
/version                read:  firmware name, build, chip, mac, sdk
/wifi/                  directory
/wifi/ctl               write: connect <ssid> <key> | disconnect |
                               scan | forget
/wifi/creds             read:  ssid <ssid>, key set|unset (never the
                               key itself). write: <ssid> <key> =
                               store-and-join, same as ctl connect.
                               Added by design note B25: credentials
                               live in this file (NVS-backed), are
                               loaded on boot, and are never compiled
                               into the firmware.
/wifi/status            read:  state, ssid, ip, rssi, channel
/wifi/scan              read:  triggers/returns last scan, one AP per
                               line: ssid<TAB>channel<TAB>rssi<TAB>auth
/net/                   directory
/net/status             read:  ip, netmask, gateway, dns, mac
/console                read/write: UART0 log + command line (wired
                        fallback mirror of ctl semantics)
/mem                    read: memwatch ring dump (added by design
                        note, B13): header plus one line per sample
                        `seq tick ev free arena arenahigh q stack`;
                        ev 0 idle, 1 client connected. Debug only,
                        no write side, no ctl semantics.
/gpio/                  only if module pinout is confirmed (v2)
/gpio/<n>/ctl           write: in | out | pullup
/gpio/<n>/data          read/write: 0 | 1
```

Control grammar is deliberately Plan 9-flavoured: short verbs, space
separated, one command per write, errors as `Rerror` text such as
`bad verb` or `wifi: not connected`. Reading a `ctl` file is not required
for control to work.

## 6. OS replacement: implementation

The firmware *is* the OS. There is no AT interpreter and no shell; the
system is a set of Plan 9-style processes (tasks) around file servers:

Recommended base: **ESP8266 RTOS SDK (C, FreeRTOS + lwIP)** - used only
as the driver substrate (Wi-Fi blobs, lwIP raw IP for IL, timers). The
RTOS scheduler stays, because the Wi-Fi blobs need it; every visible
system behaviour above it is Plan 9-shaped and ours.
How the RTOS itself gets replaced layer by layer (the
OS-adapter seam, Phase A-D) is in `PLAN9-OS.md`, which supersedes
this section's substrate discussion.

| Option | Fit | Problem |
| --- | --- | --- |
| ESP8266 RTOS SDK | Sockets, raw IP, maintained, small | More build setup |
| ESP8266 NONOS SDK | Closest to stock AT firmware | Callback style fights a 9P/IL server loop |
| Arduino core | Fastest Wi-Fi bring-up | Hides the stack; least Plan 9 in spirit |

Processes (FreeRTOS tasks, Plan 9 roles):

- `init` - boot, flash config, starts everything else, owns reboot.
- `wifi` - station/SoftAP state machine; serves `/wifi` files.
- `il` - improved-IL endpoint (IP protocol 40). One connection; feeds
  complete 9P messages to `srv9p`.
- `tcp` - fallback listener doing the same over a byte stream.
- `srv9p` - the 9P codec + fid table (section 4.3). Must compile on the
  host unchanged so it can be unit-tested off-device.
- file servers - `files.c` synthetic file table: name, read fn, write
  fn, mode. Adding a file is a table row, not new protocol code.
- `cons` - UART0 console plus the fallback ctl parser (same verb table
  as `/ctl`), so the cable gives the same system as the mount.

Inter-process traffic uses queues carrying whole messages, echoing
Plan 9's channel discipline at this scale. No dynamic fid lifetimes
beyond Tclunk; all protocol buffers are static.

### 6.1 Wi-Fi blob reverse engineering (parallel workstream)

Goal: replace libnet80211, libphy and libpp with our own source, one
library at a time, each swap proven on hardware before the next starts.
This is the highest-risk part of the whole project and is not on the
critical path for phases 1-4: the system works on the blobs while the
RE proceeds underneath it.

Evidence already in hand (from `../backup/`, strings over the stock
image): 2nd boot 1.6, SDK 2.0.0 app image containing the blob source
filenames `ieee80211.c`, `ieee80211_hostap.c`, `ieee80211_input.c`,
`ieee80211_output.c`, `ieee80211_phy.c`, `ieee80211_scan.c`,
`ieee80211_sta.c`, and runtime prints for phy/pp versions. The linked
app image in flash is therefore itself an RE input, alongside the
matching SDK 2.0.0 `.a` libraries (which retain object-level structure
the stripped flash image lacks).

Method rule: strings and filenames are entry points only. A behaviour
is only claimed when disassembly or on-hardware observation backs it;
every finding is recorded as observed, inferred, or unknown, with the
evidence for it. That discipline is what makes a clean-room rewrite
defensible.

Stages:

- **B0 Inventory.** Extract the boot/app images from the backup,
  identify every linked blob object, its size, entry points, and the
  SDK header contract it implements (`esp_wifi_*` / `wifi_*` APIs).
  Record blob hashes so replacements are diffed against known bytes.
- **B1 Disassembly.** Xtensa LX106 disassembly (Ghidra Xtensa / SDK
  objdump): function map, call graph, and the hardware register,
  DMA-descriptor and ROM-function surface each blob touches. Output is
  a written spec per function, not just listings.
- **B2 Interface capture.** On hardware: register/MMIO access traces
  and frame-level captures (monitor AP + wired mirror) for scan,
  associate, WPA2 handshake, TX/RX, and power-save, so behaviour is
  specified by observation, not only by code reading.
- **B3 Clean-room replacement.** Reimplement from the B1/B2 specs,
  library by library (order: libpp frame plumbing first, libnet80211
  MAC next, libphy/PHY calibration last and hardest). Each replacement
  must pass the same exit test: SoftAP up, station join, IL mount,
  sustained 9P traffic, reboot persistence.
- **B4 Blob-free build.** Link with zero Espressif `.a` inputs; the
  only closed code left is the chip's mask ROM, treated as hardware.

Known hard parts, named up front: undocumented MAC/PHY registers,
RF calibration and TX power behaviour (a wrong rewrite can transmit
out of spec), ROM helper functions the blobs call, and timing-critical
802.11 ACK/block-ack paths. If a stage stalls, the honest fallback is
to stop at the last library that was replaced and ship the rest on
blobs - the phase gates make that a partial success, not a failure of
the OS itself.

Flash layout on the 4 MB part: bootloader + one app image in the normal
SDK layout, one config sector for Wi-Fi credentials, rest unused in v1.
OTA is explicitly out of scope for v1; recovery is the CP2102 cable and
the stock backup.

### 6.2 Memory budget (hard rule)

DRAM is the scarcest resource on this chip (96 KB total,
shared by BSS, proc stacks, and the SDK heap). Two B19
wedges traced to silent BSS growth starving lwIP/Wi-Fi
allocation. Therefore:

- All memory is static: fixed pools and bump arenas
  (PLAN9-OS section 4.5). No unbounded statics; every large
  buffer names its budget here or in its source header.
- Proc stacks come from one static pool, sized per role
  following the SDK's own sizing (pp task largest, event
  task next, servers/tcpip mid, leaf tasks smallest), not
  one uniform size. A stack that outgrows its allocation
  corrupts its neighbour, so each proc's lowest stack word
  is a guard word checked by the scheduler.
- The esp9 build enforces the budget: after linking,
  `_bss_end` is measured and the build FAILS if heap
  headroom (top of DRAM minus `_bss_end`) falls below the
  floor in `build-esp9.sh` (default 24 KB, override
  `ESP9_MIN_HEAP`). A memory regression must break the
  build, not the device.
- Named budgets (auditable constants, lux9-style; change
  the number here when code changes it there):

  | Item | Budget |
  | --- | --- |
  | proc stack pool (all procs) | 38912 B (34816 + 4096 for il9p, B29) |
  | ppT stack (largest proc) | 8192 B |
  | srv9p arena (per-message, mark/release) | 4096 B |
  | /mem render buffer | 6144 B |
  | memwatch ring | 64 samples |
  | SDK heap headroom floor (build gate) | 24576 B |

- Runtime accounting follows lux9's Pebble principle
  only where it is safe on this substrate: our own
  allocations live in named arenas/pools that deny on
  exhaustion and report high-waters. Charging the shared
  SDK heap inside `pvPortMalloc` was tried and reverted:
  the allocator path must not call back into the heap
  (heap-size walks from blob/NMI contexts can spin), and
  blob callers fault on NULL, so runtime denial is not
  available there. The build gate above is the heap's
  accounting; Pebbles's tokens and paging do not fit a
  96 KB no-MMU target and are not adopted.

### 6.3 Fault containment

This hardware has no MMU and no MPU, so the CMRX-style
answer (terminate the faulting process, keep the system)
is not available: after a fault, shared state cannot be
trusted. The containment unit is therefore the reboot.

- A fault (exception, blown stack guard) dumps full state
  to UART0 (proc slots, ISR state, run counts), flushes,
  and reboots into a fresh server. Clients remount; the
  frozen namespace carries no state worth preserving
  across a fault. Hanging for the watchdog is a bug.
- Individual server procs are disposable below that line:
  the 9P server proc is supervised and recreated if it
  ever exits (e.g. listen failure), rather than serving
  nothing forever.
- OSI/shim semantics that blobs and lwIP silently assume
  are contractual and host-tested, because violating them
  faults far from the cause. Current contract items:
  queue items may be NULL (semaphore gives carry no
  payload); `pvTaskGetThreadLocalStorageBufferPointer`
  returns the address of the per-task TLS slot (newlib
  `__errno` stores through it); the pp task learns about
  RX only via the PendSV soft-interrupt drain
  (`MacIsrSigPostDefHdl`); on the shim substrate the 9P
  listener starts after station join, not before (accept
  during join destabilises the blob link - B19).

## 7. Host side

- Harvey / Plan 9 (primary): dial `il!<ip>!564`, post to `/srv`, mount
  into the namespace as `/n/esp9`. Harvey's `il.c` already speaks the
  `Ilflow` credit extension, so no host changes are needed.
- Linux test client (fallback only): kernel `9p` mount
  (`mount -t 9p -o trans=tcp,port=564`) or `9pfuse`. Linux cannot test
  IL; IL is verified against Harvey.
- A host-side mock comes first in the plan (phase 1): the same namespace
  served by a Go or C program on the workstation over TCP, so the file
  design is validated with real 9P clients before firmware is written.

## 8. Security notes (findings, not blockers for v1)

- v1 has no 9P authentication: anyone on the LAN who can reach port 564
  can read status and write `/wifi/ctl` and `/ctl` (including
  `factoryreset`). Acceptable only on a trusted LAN.
- Mitigations available later, in order: WPA2 is already the transport
  boundary; add a per-device token that must be written to `/auth`
  before ctl writes unlock; add source-IP allowlist in config.
- Wi-Fi keys are stored in flash. Anyone with the CP2102 cable and
  esptool can read flash back (as this backup proves). Do not store a
  high-value key on this device; a dedicated IoT SSID is the right call.

## 9. Phases

0. **Backup (done).** Full 4 MB dump, byte-identical on re-read, still
   booting stock AT firmware. See `../backup/BACKUP.md`.
1. **Host mock.** Implement the namespace + 9P subset as a host program
   over TCP; mount it from Linux and Harvey; freeze the file/verb list.
2. **Read-only firmware.** RTOS SDK build: AP bootstrap, TCP 564,
   `/status`, `/version`, `/wifi/status`, `/net/status`. No ctl writes.
   Flash only after explicit approval; verify over Wi-Fi and UART log.
3. **Control.** `/wifi/ctl` join + persistence, `/ctl` reboot/ap,
   SoftAP-to-station handover, UART fallback.
4. **Improved IL.** Minimal IL server (section 4.1) with the `Ilflow`
   credit extension; Harvey mounts over `il!<ip>!564`. TCP stays as
   fallback. Exit test includes packet capture of the SYNC exchange
   showing both sides offering `Ilflow`.
5. **Extras, only if wanted.** GPIO files once pinout is known, token
   auth, second client, OTA.

Parallel track B, independent of 1-5: **Wi-Fi blob RE** (section 6.1,
stages B0-B4). It starts with inventory of the backup image and the
SDK 2.0.0 libraries, and lands library-by-library. The OS ships on
blobs until B4; no phase above waits for it.

Every phase ends with the same exit test: unplug, replug, device comes
back reachable, and the stock backup restore command still works.

## 10. Open questions for approval

1. SDK: RTOS SDK as recommended, or Arduino for faster bring-up?
2. SoftAP after a successful station join: drop it (recommended) or keep
   it as a permanent fallback?
3. Is a v1 token on ctl writes required, or is trusted-LAN acceptable?
4. IL scope: full credit extension in the first IL release
   (recommended), or classic IL first and credit as a fast follow?

## 11. Restore / undo

At any point, the board returns to exactly today's state with the
restore command in `../backup/BACKUP.md`. Nothing in phases 1-4 changes
the CP2102 side, so the cable path always survives a bad flash.

