# B11 - Phase C v1: Plan 9 kernel substrate on Xtensa

Status: on-device exit test passed. Board restored to stock AT.

## 1. What was built

`firmware/kernel/p9_os_xtensa.c` implements the `p9_os_*`
substrate hooks declared in `firmware/kernel/p9.h` for the real
ESP8266, hook-for-hook replacing the POSIX test substrate
(`p9_os_posix.c`). Design (doc/PLAN9-OS.md Phase C):

- Procs are cooperative `setjmp`/`longjmp` coroutines, each on
  its own 4 KB DRAM stack, at most 6 procs. First entry goes
  through `p9_xt_enter` (file-scope assembly: load SP, move the
  argument, jump) because Xtensa GCC 5.2 has no working `naked`
  function attribute. The C trampoline immediately `setjmp`s its
  resume anchor and returns control to the scheduler, so every
  later resume is a plain `longjmp` into that anchor.
- `setjmp`/`longjmp` are called through function pointers so GCC
  cannot treat them as builtins with different rules. The
  windowed-ABI context comes from the pinned toolchain libc.
- The SDK main context is proc 0 by convention. `p9_os_delay_ms`
  and yield in scheduler context pump ready procs round-robin.
- Locks are cooperative spin flags. A lock is never held across a
  yield except through `p9_os_wait`, which unlocks first, exactly
  mirroring the rendez discipline in `p9.c`.
- `p9_os_wait` is yield-on-wait: wake is a no-op and every waiter
  rechecks its condition after each yield, as the Plan 9 sleep
  loop is written. No heap is used anywhere: procs, stacks, and
  lock slots are static, consistent with the Goose-style arena
  rule (own code never calls malloc).

## 2. Verification

Host: `test_kernel: all tests passed` (POSIX substrate, unchanged
contract). Cross-build with the pinned Xtensa GCC
(`-std=c99 -Os -Wall -Wextra -Werror -mlongcalls
-mtext-section-literals`) archives to `libp9.a`; the only
undefined symbols are SDK/ROM externs (`system_get_time`,
`ets_delay_us`, `os_printf_plus`) plus libc (`setjmp`, `longjmp`,
`memcpy`, `memset`). No undefined `p9_os_*` hooks remain.

On-device image `user1-kerntest.bin`, 227796 bytes, SHA-256
`7b56ed506f1ca58266547cf84c9c92ff19b8099395527cd27d94be7bb0e1bc61`,
packaged with the Python-3 `gen_appbin.py` port (boot 1.6 format,
first header bytes `ea 04`). It links the p9 primitives, the OSI
Wi-Fi surface, and the Xtensa substrate behind stock boot/init,
exactly the B7 test pattern.

Flashed at `0x1000` on `/dev/ttyUSB1` (hash verified by esptool),
UART capture after reset:

    KERNEL-TEST boot sdk=2.0.0(656edbf)
    KERNEL-TEST pass proc-create
    KERNEL-TEST pass msgq-procs
    KERNEL-TEST pass qlock-fifo
    KERNEL-TEST pass canqlock
    KERNEL-TEST pass sem-create
    KERNEL-TEST pass sem-take
    KERNEL-TEST pass sem-timeout
    KERNEL-TEST pass sem-give
    KERNEL-TEST pass sem-retake
    KERNEL-TEST pass timer-create
    KERNEL-TEST pass timer-fired
    KERNEL-TEST pass osi-queue-create
    KERNEL-TEST pass osi-overwrite
    KERNEL-TEST pass osi-order1
    KERNEL-TEST pass osi-order2
    KERNEL-TEST ALL-PASS heap=20168

So the `__wifi_*` OSI surface (semaphores, timers, queues with
overwrite semantics) and the core Plan 9 primitives (msgq between
procs, FIFO qlock, canqlock) run correctly on the Plan 9
substrate on real hardware. Free heap is lower than the B7 test
(20168 vs 52328 bytes) because of the 6 proc stacks and kernel
static state; that is expected and bounded.

## 3. Restore

The full 4 MB stock backup was rewritten at `0x0` (hash verified
by esptool). AT probe afterwards:

    AT -> OK
    AT+GMR -> AT version:1.3.0.0(Jul 14 2016 18:54:01)
              SDK version:2.0.0(5a875ba)
              v1.0.0.3
              Mar 13 2018 09:37:06
              OK

Identical to the pre-flash behaviour. Board state: stock AT.

## 4. Known limits (v1, deliberate)

- Cooperative only: no preemption, no interrupt-driven wakeups.
  `os_lock` is a cooperative spin flag and is NOT ISR-safe;
  interrupt masking is the next hardening step.
- No per-proc teardown dance beyond `done`; a proc that never
  yields starves the others, and the SDK watchdog expectations
  still apply to long proc bodies.
- `__wifi_task_top_sp` is meaningful only for the calling proc;
  cross-proc delete and true timer waits arrive with the RTOS
  blob link step.
- 6 procs x 4 KB stacks is a starting budget, not a measured one.

## 5. Next steps

1. Link the OSI surface (`osi_wifi.c`) under the RTOS Wi-Fi blobs
   without FreeRTOS, so blob tasks run as p9 procs.
2. devwifi over the clean-room libpp (B5-B7), then devip with
   improved IL primary and TCP fallback on port 564 per
   doc/DESIGN.md.
3. Harden locking with interrupt masking once blob ISRs are in
   the picture.
