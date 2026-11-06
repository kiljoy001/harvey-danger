# B21 - The load stall, localized: RX posts stop, scheduler lives

Status: DONE (localization; root cause below the pp queue
is next round). The intermittent bench stall from B19/B20
is now confirmed to the pp-queue boundary with on-device
telemetry. Board restored to stock AT (verified).

## Instrumentation added (kept as telemetry)

- `g_pp_posts` in
  [freertos_shim.c](/home/scott/Repo/harvey/esp8266-9p/firmware/kernel/freertos_shim.c):
  counts sig-5 posts to the blob pp queue (created 75x8)
  on the ISR send path - i.e. received frames that reached
  the pp task's queue. (Path: blob `MacIsrSigPostDefHdl`
  -> SDK `esp_wifi_os_adapter` -> shim
  `xQueueGenericSendFromISR`.)
- Scheduler heartbeat in
  [p9_os_xtensa.c](/home/scott/Repo/harvey/esp8266-9p/firmware/kernel/p9_os_xtensa.c):
  every 10 s, raw UART (no locks, no heap, works when
  every proc is blocked): `HB rx=<hex> last=<slot>
  runs=<per-slot run counts mod 10>`.
- The app's alive line no longer calls the blocking
  `esp_wifi_get_ap_rssi()`; it prints
  `alive rx=<n> heap=<n> cli=<0|1>`.

## Evidence

Provisioned runs under `bench9p.py` (~50-70 RPC/s):

- Passing runs still occur (full `BENCH9P: DONE`), but
  roughly half of runs stall at a random RPC (observed:
  Tread, readall, Twalk of later iterations).
- In stalled runs the heartbeat keeps printing with the
  scheduler cycling procs, while `rx` freezes exactly
  (e.g. `HB rx=00000210` unchanged 10 s apart) and the
  alive line stops (uiT blocked downstream of the dead
  RX path). No PANIC, no STACKBLOW, no reboot (>70 s).
- Conclusion: frames stop reaching the pp queue while
  CPU and scheduler remain healthy. Everything above
  (ppT, net80211, lwIP, srv9p, uiT's queries) then blocks
  forever. This is B18's auth/assoc frame loss in its
  severe, sustained-load form.

## Theories tested and eliminated this round

- **Runtime heap charging in pvPortMalloc** (B20 WiP):
  stalled with and without it. Reverted regardless;
  allocator path stays clean.
- **Malloc-from-NMI heap re-entry**: blob esf allocation
  from the WDEV FIQ re-entering newlib malloc mid-op was
  a strong fit. Implemented an NMI fixed-block pool
  (6x1664 B, carved from heap at first use, byte-flag
  state) so the FIQ never touches the heap
  ([shim_xtensa.c](/home/scott/Repo/harvey/esp8266-9p/firmware/blobrtos/components/freertos/shim_xtensa.c)).
  Stalls recur identically with it, so heap re-entry is
  not (the only) cause. The pool is kept anyway: NMI
  calling malloc remains improper on this substrate,
  and the blob provably tolerates esf NULL (frame drop).
- **sys_evt stack, ppT stack, srv9p stack**: sized by
  measurement in B19/B20; the stack guard has never
  fired during a stall.

## Next round (below the pp queue)

The chain is FIQ -> blob ring -> PendSV soft interrupt
-> deferred dispatch -> `MacIsrSigPostDefHdl` -> pp
queue. `g_pp_posts` proves output stopped; which link
stops is unknown. Cheapest next probes, in order:

1. Count ring insertions vs drain calls (wrap
   `pp_post_from_isr` / `MacIsrSigPostDefHdl` again,
   temporarily): separates FIQ-side death from
   drain-side death.
2. If the drain stops: audit the PendSV/deferred-dispatch
   interaction (INTSET while the soft source is masked
   pending dispatch) under FIQ preemption of the drain.
3. If the FIQ stops: audit `INT_ENA_WDEV` narrowing
   windows (`vPortETSIntrLock`) for a path where the
   WDEV interrupt enable is left narrowed.

## Recovery verified

Stock backup reflashed: `AT` -> OK; `AT+GMR` -> AT
1.3.0.0 / SDK 2.0.0. Provisioning removed (grep-clean),
host tests pass, default image boots/scans/listens
with the memory gate green (headroom 27296 B).

## Addendum

Renamed `blobrtos` -> `esp9` (directory, build script,
project/image name, log tag, /version string) at the
user's direction: blobrtos named the bring-up vehicle,
esp9 names the system. B18-B21 keep the old name where
they describe the builds of their day.
