# B26 - Provisioned bench on the creds file + esf pool probe

Status: DONE (join + bench + stall reproduction with the
B22 probe live). Board restored to stock AT (verified).
Credentials were provisioned at runtime over UART
(connect verb); they are in the device file only, never
in source, and are not recorded here.

## Provisioning findings

- First join attempt failed with reason 201
  (NO_AP_FOUND): the SSID as supplied differed in case
  from the broadcast SSID, and SSIDs are case-sensitive.
  With the exact broadcast name the device joins
  (join-ok rssi=-49..-50, DHCP lease 192.168.1.146).
- Boot with stored creds occasionally takes ~25 s to
  become reachable and the join loop at boot sometimes
  misses; the disconnect-retry path (10 retries) carries
  it. Reconnect cycles with reason 4 and blob log
  `no buf for probe, ie len 0` were seen during one
  unstable period, then a clean join. This matches the
  B18/B21 assoc-frame-loss flakiness, now on a real AP.

## Bug found by provisioning: out of procs

With credentials stored, `MILESTONE 9p-listening` never
printed and TCP 564 stayed closed: `p9_os: out of
procs`. The B25 cons proc took the last of MAX_XPROCS=6
slots, so srv9p silently never started (this also means
the B25 unprovisioned boot had no listener either; its
report overstated that line). Fix: MAX_XPROCS 6 -> 8 in
p9_os_xtensa.c (slots are cheap; the stack pool is the
real budget, and it had room). After the fix: join-ok,
9p-listening, alive all present. Memory gate green
(headroom 26944).

## Bench results (bench9p.py, device at 192.168.1.146)

- 30 iterations: BENCH9P: DONE (Tread med 18.4 ms,
  readall 34.7 ms, walk+open+read+clunk 72.0 ms,
  Twalk scan write 23.9 ms - in line with B19).
- 120 iterations: all read phases pass (Tread med
  14.9 ms over 120, ~240 RPCs total), then stalls in
  the final Twake/scan-write phase.
- 300 iterations (twice) and 60-90 iterations (twice):
  stall at a random point after roughly 80-250 RPCs.
  So: moderate load is clean on our esf_buf; sustained
  load still hits the B21 stall.

## The B22 probe, live (decisive)

The alive line now prints esfrx/esfgen (pool levels,
firmware/esp9/main/blob_main.c). During the 120-iter
run, held open on UART while the bench ran:

    alive rx=116 heap=31056 cli=0 esfrx=8 esfgen=6
    alive rx=381 heap=27416 cli=1 esfrx=8 esfgen=4
    alive rx=858 heap=30468 cli=1 esfrx=8 esfgen=5
    (stall) HB rx=00000360 ... (frozen), alive stops

Findings at the stall:

- esfrx = 8 the whole time: the RX esf pool is FULL,
  not drained. The B22 pool-exhaustion theory is dead
  for this stall, and the clean-room esf_buf is
  exonerated - buffers are being recycled correctly
  under sustained load (esfgen oscillates 4-6 with
  TX in flight, as expected).
- g_pp_posts freezes (0x360) while HB heartbeat
  continues and per-slot run counts keep changing:
  scheduler alive, frames stop reaching the pp queue.
- No WIFI_EVENT_STA_DISCONNECTED around the stall and
  the blob state machine stays in run state: the
  association is up, data delivery is dead. Same
  signature as B21, now with the esf layer proven out.

So the break remains in the B21 chain: FIQ -> blob
ring -> PendSV -> deferred dispatch ->
MacIsrSigPostDefHdl. Next probe per B21: count ring
insertions vs drain calls. The alive line also stops
at the stall (uiT blocked somewhere in its 5 s loop -
heap-size walk is the standing suspect from B20),
which is a second, smaller mystery worth one probe.

## State

- Device: stock AT restored, AT/AT+GMR verified. The
  stored credentials were in esp9 NVS flash, which the
  full-flash stock restore overwrote.
- Source: MAX_XPROCS fix + alive pool counters are in;
  build gate green; host tests pass (run this round:
  test_pp, test_esf_buf).
