# B20 - Design corrections from CMRX/lux9 review

Status: DONE. Two external designs were reviewed for
transferable lessons (CMRX, a Cortex-M MPU microkernel;
lux9-kernel's Pebble token-budget system) and the
corrections that fit a 96 KB no-MMU target were applied
to the design and the blobrtos image. Board restored to
stock AT (verified).

## What was adopted

1. **Build-time memory budget gate** (CMRX derives its
   layout from the linker; lux9 conserves a global token
   bank). `build-blobrtos.sh` now measures `_bss_end`
   after linking and FAILS the build if heap headroom
   (0x40000000 - `_bss_end`) drops below 24 KB
   (`ESP9_MIN_HEAP` overrides). Verified both directions:
   current build reports `bss=68416 bytes, heap
   headroom=27312 bytes`; a forced 99999999 floor fails
   the build. This converts B19's on-device DRAM wedge
   into a build error. DESIGN 6.2 records the rule and a
   named budget table (stack pool, ppT stack, arena,
   /mem buffer, memwatch ring, heap floor).

2. **Fault containment at reboot granularity** (CMRX
   terminates the faulting process; with no MPU, shared
   state cannot be trusted after a fault here).
   `panicHandler` and the stack-blow guard now dump
   state, flush UART, and `esp_restart()` instead of
   hanging for the watchdog (`fatal_reboot` in
   [shim_xtensa.c](/home/scott/Repo/harvey/esp8266-9p/firmware/blobrtos/components/freertos/shim_xtensa.c)).
   Below that line, procs are disposable: the 9P server
   runs under a supervisor wrapper that recreates the
   listener if it ever exits. DESIGN 6.3 records the
   policy.

3. **OSI contract pinned by host tests** (CMRX's hosted
   testing discipline). `test_shim` gained `shim-tls-errno`:
   `pvTaskGetThreadLocalStorageBufferPointer` must return
   the per-task TLS slot address (never NULL, including
   no-task context), and slots are per-proc. All host
   tests pass (test_kernel, test_shim, test_memwatch).

## What was assessed and not adopted

- **lux9 Pebble tokens**: per-process memory budgets
  charged per page (1 token = 8 B, denial on exhaustion,
  conserved global bank, COW token colours). The
  mechanism needs paging/MMU, fork, and up to 4096 token
  records - none of which exist or fit here. The
  principle (charge allocations to an owner, deny when
  out) survives only in our arenas/pools, which already
  deny and report high-waters.
- **Runtime heap charging in `pvPortMalloc`** was
  implemented (gross per-proc charge + low-water
  sentinel) and then reverted: heap-size sampling from
  the allocator path, including blob/NMI contexts, risks
  spinning on the heap lock, and two bench runs with it
  stalled; with it removed the same stalls recur, so it
  was not the (only) cause - but the allocator path
  stays clean on principle. DESIGN 6.2 says so.
  (Also: allocation headers are impossible here -
  `free()` on a `pvPortMalloc` pointer is legal in this
  SDK, so interposing offsets would corrupt callers.)
- **CMRX MPU isolation / per-process termination**:
  no MPU on ESP8266; see containment above.

## On-device verification

- Provisioned build (with gate): full `bench9p.py` pass
  (`BENCH9P: DONE`), clean `client disconnected`, no
  PANIC/STACKBLOW/HEAPLOW in the log.
- Default build: boots, scan-ok, `9p-listening`, gate
  reports headroom 27312 B.
- Stock restored: `AT` -> OK, `AT+GMR` -> AT 1.3.0.0 /
  SDK 2.0.0. Temporary provisioning removed; grep
  confirms no credential in source.

## Open: RX-path death under sustained load

The bench still occasionally stalls mid-run (roughly
1 in 3 runs) at a random RPC, after which the device is
silent but does NOT reboot (observed >70 s): scheduler
and watchdog stay alive, nothing prints. Best-fit
theory, consistent with all captures: the blob RX path
stops delivering frames (the severe form of B18's
auth/assoc frame loss), TCP stalls, and uiT blocks
inside its `esp_wifi_get_ap_rssi()` alive-loop query so
even liveness logging stops. Not proven - no wedged-CPU
snapshot channel exists on this substrate. This is now
the top known instability. First suspects for the next
round: RX descriptor replenishment racing our deferred
FIQ/soft-interrupt handling under load; an RX-progress
counter surfaced via /mem would confirm the theory
cheaply (frames stop, CPU counters continue).
