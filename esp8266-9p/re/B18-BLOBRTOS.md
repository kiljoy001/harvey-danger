# B18 - blobrtos: RTOS-generation Wi-Fi blobs on the Plan 9 shim, joined

Status: DONE. The `blobrtos` image (RTOS-generation
libpp/libnet80211/libphy over the Plan 9 kernel + FreeRTOS-API
shim, no RTOS kernel code) boots, scans, joins the provisioned
AP, gets a DHCP lease, and answers ICMP echo from the host.
Board restored to stock AT afterward (verified, see below).

## Verified on device (ESP8266EX, /dev/ttyUSB1, 4 MB flash)

Clean provisioned build, one boot:

- `MILESTONE init-returns heap=52432`
- `MILESTONE pp-task heap=51312` (blob pp task runs as a p9 proc)
- `MILESTONE scan-ok` (5 APs; Linksys01314 ch1 rssi -53..-42)
- reconnect retries exercised: EVT reason 203 (ASSOC_FAIL),
  then 205, then EVT id=4 (STA_CONNECTED)
- `MILESTONE join-ok rssi=-42`
- `tcpip_adapter: sta ip: 192.168.1.146, mask: 255.255.255.0,
  gw: 192.168.1.1` (same lease the SDK reference build gets)
- `alive rssi=-40 heap=50780` every 5 s, indefinitely
- Host `ping 192.168.1.146`: 4/4 replies, 0% loss
  (rtt avg ~50-110 ms; cooperative scheduling + modem sleep)

Default (unprovisioned) build also verified: boots, pp-task,
scan-ok, `MILESTONE done`, no join attempt.

Host tests: `test_kernel`, `test_shim`, `test_memwatch` all
pass (`all tests passed`) after every change below.

## What made it work (this stage's fixes)

1. **Soft-interrupt RX drain (PendSV).** Blob `pp_post()` puts
   sig>=4 work (RX is sig 5) into a lock-free ring and calls
   `PendSV(2)`, expecting the soft ISR to move ring entries
   into the pp queue via `MacIsrSigPostDefHdl()`. Our PendSV
   was a no-op, so the pp task never saw received frames:
   scan returned 0 APs and promiscuous RX starved. Implemented
   faithfully in
   [shim_xtensa.c](/home/scott/Repo/harvey/esp8266-9p/firmware/blobrtos/components/freertos/shim_xtensa.c):
   `PendSV(1|2)` raises `ETS_SOFT_INUM` (7) via INTSET, and a
   `SoftIsrHdl` attached for that interrupt calls
   `MacIsrSigPostDefHdl()`. Deferred to scheduler context by
   the existing level-1 deferral, like every other handler.
   Scan immediately found 5 APs.

2. **NULL-payload queue items (semaphore gives).** Right after
   STA_CONNECTED the machine faulted in ROM `memcpy` called
   from `q_put` ([p9.c](/home/scott/Repo/harvey/esp8266-9p/firmware/kernel/p9.c)):
   a binary semaphore give passes a NULL item (FreeRTOS
   uxItemSize==0 queues skip the copy), but the shim coerced
   item_size 0 to 1 and copied a byte from address 0.
   `q_put`/`p9_msgq_recv`/`p9_msgq_recv_isr` now skip the copy
   when the item pointer is NULL. Found via a temporary
   `p9_qput_bad` UART hook (removed after the fix; the guard
   on NULL storage/zero capacity remains, with a weak no-op
   hook).

3. **CCOUNT wrap wedged the whole machine at ~26.8 s.**
   Deadliest bug of the stage: every run silently froze
   2-3 s after `esp_wifi_connect()` (later shown to be
   boot-relative, not connect-relative; a delayed-connect
   experiment proved it). Freeze matched 2^32 CCOUNT cycles
   exactly: 26.84 s at 160 MHz, 53.7 s after switching the
   build to 80 MHz. Root cause: the real port's
   `xPortSysTickHandle` resets CCOUNT every tick and
   accumulates elapsed microseconds into `g_esp_os_us`, so
   SDK time (`g_esp_os_us + ccount / g_esp_ticks_per_us`,
   esp_timer.c) never wraps. Our substrate has no tick reset
   and never advanced `g_esp_os_us`, so system time wrapped
   to zero at 2^32 cycles and every deadline built on it
   wedged. Fix in
   [p9_os_xtensa.c](/home/scott/Repo/harvey/esp8266-9p/firmware/kernel/p9_os_xtensa.c)
   `p9_os_now_ms()`: count CCOUNT wraps (the function is
   called at least once per millisecond from the delay/wait
   paths) and keep `g_esp_os_us` in step, mirroring port.c.
   After the fix the 160 MHz build runs through multiple
   wraps (observed >98 s, alive loop printing, ping OK).

4. **Reconnect on disconnect.** The auth/assoc exchange over
   the shim still loses a frame occasionally (observed
   reasons 202 AUTH_FAIL, 203 ASSOC_FAIL, 205). The event
   handler now retries `esp_wifi_connect()` up to 10 times
   on STA_DISCONNECTED, as the SDK station example does;
   joins succeed within 1-3 attempts.

Earlier blobrtos fixes this stage built on (from B17's
abandoned mixed image and the first blobrtos boots): SDK
`xtensa_vectors.S`/`os_cpu_a.S` own `_xt_isr_handler`
equivalents with level-1 deferral, per-proc saved/restored
PS.INTLEVEL, ISR yield guards, queue capacity by byte
product, pp queue (75, 8) at priority 13, faithful
`vPortETSIntrLock` narrowing to `WDEV_TSF0_REACH_INT`.

## Cleanup done for this milestone

- Removed all bring-up instrumentation: `--wrap` counters
  (component.mk LDFLAGS and `__wrap_*` bodies), pp-queue
  histogram/signal counters in freertos_shim.c, the
  `p9_qput_bad` UART hang hook, the scheduler heartbeat
  trace (run counts / alive stage / stack watermark / clock)
  in p9_os_xtensa.c, and the promiscuous/TX-probe/dump
  scaffolding plus delay-tick register dumps in blob_main.c.
  [blob_main.c](/home/scott/Repo/harvey/esp8266-9p/firmware/blobrtos/main/blob_main.c)
  is now just: init, start, scan, provisioned connect with
  retry, join-ok, alive log.
- Temporary AP provisioning removed from
  [blob_config.h](/home/scott/Repo/harvey/esp8266-9p/firmware/blobrtos/main/blob_config.h)
  (template comments only; a repo-wide grep confirms the
  credential is gone). Default image rebuilt and verified.
- The panic/fault reporter (`panicHandler` with ISR/proc
  state dump) stays: it is the only crash channel on this
  substrate.

## Recovery verified

Stock backup reflashed
(`backup/esp8266-at-stock-483fda7e3968.bin`) and verified:
`AT` -> OK; `AT+GMR` -> AT version 1.3.0.0 (Jul 14 2016),
SDK version 2.0.0(5a875ba).

## Known gaps / next

- Auth/assoc frame loss over the shim is worked around with
  retries, not root-caused; RX-path loss (ring overwrite in
  the blob, esf allocation, deferred dispatch latency) is the
  first suspect when it matters.
- Cooperative-shim approximations remain: cross-task
  suspend/delete, timer daemon fidelity, critical sections
  are not preemptive.
- `blob_support.c` crypto stubs return errors; WPA2/CCMP
  worked here through the blob's own path, but the stubbed
  helpers bound what else can run.
- RAM NVS shim is non-persistent; PHY calibration behavior
  across resets is uncharacterized.
- Next stage per DESIGN.md: run the 9P server on this
  substrate (rtossrv lineage) now that the blob link is up,
  then improved IL / devip.
