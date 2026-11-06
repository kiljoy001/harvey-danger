# B28 - RX stall root-caused and fixed: interrupt-lock gate deadlock

Status: DONE (instrumented reproduction, root cause,
fix, 3 consecutive full benches, stock restored).

## Instrumentation

Heartbeat (p9_os_xtensa.c, raw UART) now reports the
whole RX chain: rx (pp-queue deliveries), pd (PendSV
posts), dr (MacIsrSigPostDefHdl drain calls), s7
(soft-interrupt vector hits), pend (deferred interrupt
bits awaiting dispatch), il (per-proc saved PS.INTLEVEL
as hex digits), last/runs as before. Counters
g_pendsv_posts/g_soft_drains live in shim_xtensa.c.
The alive line keeps the esf pool levels from B26.

## Evidence at the stall (bench 300, held-open UART)

    HB rx=000000c8 pd=00000154 dr=00000122 s7=00000123
       pend=00000480 il=0300000. last=1 runs=9x253190

- rx/pd/dr/s7 all frozen, scheduler alive (HB keeps
  printing, slot 1 run digit keeps changing).
- pend = 0x480 forever: soft interrupt (bit 7) and a
  timer interrupt (bit 10) pending, never dispatched.
- il digits: slot 1 = tiT (lwIP tcpip task) holds
  saved INTLEVEL 3 permanently, and is the only proc
  running (last=1, spinning).

## Root cause

tiT yielded inside a ROM ets_intr_lock (level 3) while
waiting for mailbox work. Two substrate gates then
made that fatal:

1. run_one's holder gate ran ONLY the proc with
   saved_intlevel > 0 - tiT, forever, spinning on a
   wait whose wakeup it had locked out.
2. p9_isr_dispatch_pending refused all dispatch while
   any proc held intlevel - so the soft drain and the
   timer that could feed tiT never ran.

Circular wait: tiT holds the lock waiting for work;
work arrives only via interrupts; interrupts dispatch
only when no lock is held. association stayed up, RX
delivery dead - the B21 signature exactly. The esf
pools were already exonerated in B26 (esfrx=8 at the
stall); this was never a buffer problem.

The holder gate exists for flash-cache-off windows
(PHY calibration), which enter through
vPortEnterCritical (PS.INTLEVEL 15). A level-3 ets
lock only ever meant "mask interrupts" - on the real
FreeRTOS port other tasks run under it freely, each
with its own saved PS.

## Fix (p9_os_xtensa.c)

Both gates now key on a full lock only:
proc_holds_full_lock() = saved_intlevel >= 15, used by
run_one's holder scan and p9_intlevel_held(). Level-3
locks no longer exclude other procs or dispatch. The
critical-section-owner gate (critos) is unchanged, so
cache-off critical sections are still exclusive.

## Verification

Three consecutive full bench runs (300 iters each,
~930 RPCs per run incl. scan writes), device held on
UART the whole time:

    run 1: BENCH9P: DONE  (Tread med 15.4 ms)
    run 2: BENCH9P: DONE  (Tread med 15.1 ms)
    run 3: BENCH9P: DONE  (Tread med 15.3 ms)

Before the fix, 300-iter runs stalled twice out of
two and 60-90-iter runs stalled as well. Heartbeat
after the runs: pend=00000000, rx still climbing
(0x12db and counting), il=0300303. (several procs
routinely sit at level 3 now, harmlessly). Host
tests: test_shim, test_kernel, test_pp, test_esf_buf
pass. Stock restored: AT/AT+GMR verified.

## Remaining risk, stated

If blob code ever holds a flash-cache-off window via
raw ROM ets_intr_lock + Cache_Read_Disable (not via
vPortEnterCritical) across a yield, other procs can
now run during it. No such path has been observed:
calibration runs inside init/channel operations, and
three load benches plus the join/scan cycles in this
round exercised the radio without a fault. The
critos gate still covers every cache-off window that
enters through the shim's critical sections.
