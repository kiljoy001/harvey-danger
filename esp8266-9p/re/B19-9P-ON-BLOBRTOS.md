# B19 - The 9P server on blobrtos: frozen namespace over Wi-Fi

Status: DONE. The B18 blobrtos image now also runs the
rtossrv-lineage 9P2000 server (srv9p core + lwIP sockets)
over the joined station link, TCP 564. `bench9p.py` from the
host passes end-to-end (three consecutive full runs).
Board restored to stock AT afterward (verified).

## Verified on device (ESP8266EX, /dev/ttyUSB1)

- `MILESTONE 9p-listening tcp 564`, `client connected`,
  `client disconnected` (clean, no fault)
- `bench9p.py 192.168.1.146 564`:
  - setup version+attach ~30-50 ms
  - Tread /status (open fid): med ~17-21 ms, ~50-60 ops/s
  - readall /status: med ~36 ms, ~27 ops/s
  - walk+open+read+clunk /version: med ~86-99 ms, ~11 ops/s
  - Twrite 'scan' /wifi/ctl: med ~24 ms, ~42 ops/s
  - `BENCH9P: DONE`
- The Twalk-triggered SCAN_DONE refreshes /wifi/scan via the
  event handler; /mem serves memwatch samples (sampled from
  the alive loop and connect/disconnect events).
- Default (unprovisioned) build verified: boots, scan-ok,
  `9p-listening`, no join attempt.
- Host tests: test_kernel, test_shim, test_memwatch all pass.

The namespace is the DESIGN section 5 frozen set, station
flavour: /, /ctl (reboot), /status, /version, /wifi,
/wifi/ctl (scan, disconnect, connect <ssid> <key>),
/wifi/status, /wifi/scan, /net, /net/status, /console,
/mem. SoftAP verbs (ap on/off, factoryreset) were dropped
for now: SoftAP-on-shim is unproven and the SoftAP-after-join
policy is still an open question.

## What it took

1. **DRAM: the server did not fit next to uniform stacks.**
   First server build wedged inside `tcpip_adapter_init`
   (uiT waits forever for the tcpip task's init-done signal)
   and WDT-reset every ~28 s: BSS had grown to 85.6 KB
   (6-8 procs x 7.1 KB with inline 6144 B stacks, plus
   membuf/arena/server statics), leaving ~10-44 KB heap,
   and lwIP init starved. The substrate now allocates proc
   stacks from a static pool, sized by task name as the SDK
   does (ppT 8192 incl. level-1 handler frames on its stack,
   sys_evt 6144 because it runs esp_wifi_connect/scan_done,
   tiT/uiT/srv9p 4096, others 3072; pool 34816 B) in
   [p9_os_xtensa.c](/home/scott/Repo/harvey/esp8266-9p/firmware/kernel/p9_os_xtensa.c).
   Measured high-waters in service: all slots <= ~2.4 KB.
   A `p9_stack_blown` guard in run_one checks each proc's
   bottom stack word so the next overflow names its slot
   instead of wedging silently. MEMWATCH_CAP 128 -> 64 and
   the /mem render buffer 8192 -> 6144 also came out of BSS.

2. **errno was a NULL pointer.** After the first full bench
   pass, the srv9p proc faulted in `lwip_recv_tcp` on every
   client disconnect: `s32i` through `__errno()`, which
   returned 0. The SDK newlib `__errno` is
   `(int *)pvTaskGetThreadLocalStorageBufferPointer(NULL,
   ERRNO_TLS_INDEX)` - it stores errno *in the task's TLS
   pointer slot*, so the function must return the slot's
   address. Our shim stub returned NULL (and had the wrong
   signature). Fixed in
   [freertos_shim.c](/home/scott/Repo/harvey/esp8266-9p/firmware/kernel/freertos_shim.c)
   to return `&t->tls[idx]`, with a static fallback slot for
   no-task context. Disconnects are clean since.

3. **sys_evt stack overflow in scan_done.** A ctl 'scan'
   write wedged the machine silently right after SCAN_DONE:
   scan_done() put `wifi_ap_record_t rec[16]` (~1.7 KB) on
   the sys_evt proc's 3072 B stack, on top of
   esp_wifi_scan_get_ap_records' own chain, and the overflow
   corrupted the neighbouring pool stack. The array is
   static now, and sys_evt got 6144 B (see 1).

4. **Do not accept() during join.** With the server task
   started before connect (as rtossrv does on the real
   kernel), roughly half of joins wedged after the second
   disconnect/retry - once caught as a fault in blob
   esf_rx_buf_alloc -> vPortETSIntrLock (VADDR fff00080)
   in ppT, usually silent. Starting the server task after
   join-ok (still before the alive loop; in the default
   image it starts right after the scan) made joins and the
   bench reliable across repeated runs. Root cause inside
   the lwip accept path under join load is not pinned down;
   the reorder is also semantically fine (no address to
   serve on before DHCP anyway). Recorded as a shim
   limitation, first suspect for the next debugging round.

Temporary provisioning was used for the provisioned test
builds and removed afterward; the committed
[blob_config.h](/home/scott/Repo/harvey/esp8266-9p/firmware/blobrtos/main/blob_config.h)
has template comments only (grep confirms no credential in
source). Per-message server tracing and step prints used
during bring-up were removed; the stack guard (1) stays.

## Recovery verified

Stock backup reflashed and verified: `AT` -> OK;
`AT+GMR` -> AT version 1.3.0.0, SDK version 2.0.0(5a875ba).

## Known gaps / next

- Join-phase frame loss (reasons 202/203/205) still only
  worked around by retries; see B18. The accept-during-join
  instability (4) may share that root.
- /wifi/ctl 'connect' makes the ctl-join path real, but
  without SoftAP bootstrap the unprovisioned image has no
  way to receive its first credentials over the air yet
  (serial provisioning or SoftAP-on-shim, policy open).
- Improved IL transport (DESIGN: IL primary, TCP fallback)
  is now the next transport item: the srv9p core is
  transport-agnostic (Pp9Transport), so IL slots in beside
  the TCP server task.
- Throughput is bench-limited by cooperative scheduling
  latency (~17 ms median RPC); fine for ctl/status class
  traffic, worth revisiting if bulk reads matter.
