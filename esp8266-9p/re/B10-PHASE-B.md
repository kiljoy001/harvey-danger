# B10 - Phase B started: Proc/Rendez/QLock + OSI adapter over them

Continues B9 and doc/PLAN9-OS.md Phase B. Not blocked by the
host Wi-Fi permission issue, so this stage is all substrate.
No flashing; the board remains stock AT (B9 restore verified).
The over-air 9P item from B9 section 3 stays open and is not
re-attempted here.

## 1. The seam, measured in the RTOS tree

The Phase B target was re-read in the pinned RTOS SDK v3.4:
`components/esp8266/source/esp_wifi_os_adapter.c` is 175 lines
implementing the blob OS surface as direct FreeRTOS calls -
there is no table struct in this version; the blobs link
against 22 named `__wifi_*` functions (task create/delete/
yield/delay/priority/ticks/suspend/resume/top-sp, queue
create/delete/send/send_from_isr/recv/msg_num, semaphore
create/delete/take/give, timer create/reset/stop). Header
`esp_wifi_osi.h` only fixes constants (TIME_BLOCKING, queue
positions). Twenty-two functions is the whole contract, and
PLAN9-OS.md is corrected by this in substance if not in text:
the "table" is a symbol list, which is even easier to
substitute - provide the symbols, keep the blobs.

## 2. Implemented: firmware/kernel/

- `p9.c/h` - Plan 9 primitives sized for the chip: QLock as a
  FIFO ticket lock (arrival order is acquisition order, tested
  with staggered procs), Rendez sleep/wakeup under the caller's
  lock (Plan 9 discipline), a proc registry (16 static procs),
  a scheduler suspend/resume gate honoured at checkpoints and
  in delays, bounded message queues with FreeRTOS-compatible
  copy-in/copy-out item semantics (front/back/overwrite), and
  tick time at the RTOS's 10 ms tick. All state is static
  pools or caller storage; no heap anywhere in the layer.
- `p9_os_posix.c` - the blocking substrate for host tests
  (os lock, wait/wake, thread create over pthreads). Explicitly
  test-only: the three hooks are the entire platform surface,
  and the Xtensa replacement is Phase C.
- `osi_wifi.c/h` - all 22 `__wifi_*` symbols over the
  primitives, matching RTOS signatures and return conventions,
  static pools (8 queues up to 32 items x 32 bytes, 8 counting
  semaphores, 8 timers served by one timer proc, procs via
  p9). Tick conversion, blocking-forever timeout, and
  from-ISR send (never blocks) follow the RTOS adapter.

## 3. Verified

Host (test/test_kernel.c, -Wall -Wextra -Werror -pthread):

- producer/consumer procs exchange 0..19 in order through a
  bounded queue (sum asserted);
- four staggered lockers acquire a QLock in exact arrival
  order (ticket FIFO, not pthread luck);
- OSI queue semantics: full queue rejects, overwrite replaces
  the tail, front-send reads back first;
- counting semaphore take/timeout/give; one-shot timer fires
  its callback at 5 ticks;
- OSI task + queue echo end-to-end (a `__wifi_task_create`
  proc received 42 sent via `__wifi_queue_send`).
- Output: `test_kernel: all tests passed`.

Target: p9.c and osi_wifi.c cross-compile clean for
xtensa-lx106-elf under -Werror. `nm` shows all 22 `__wifi_*`
symbols exported by osi_wifi.o, and p9.o's undefined symbols
are exactly the p9_os_* substrate hooks (plus libc/ROM maths)
- the Phase C work list, reproduced mechanically.

## 4. Limits, plainly

- Timeouts in queues/semaphores poll at 1 ms. That is an OSI
  compatibility choice on the host substrate; proc waits
  structured as rendez do not poll. The Xtensa substrate gets
  real timer-driven waits in Phase C.
- `__wifi_task_delete` only self-deletes; deleting another
  proc requires the real scheduler.
- `__wifi_task_top_sp` returns the current P9Proc, a
  diagnostics equivalent, not a saved SP (its RTOS callers
  use it for stack checks).
- Nothing here has run on the chip. The primitives were
  designed for static allocation so the target link is a
  formality once a p9_os_xtensa.c exists; claiming more than
  compile-verified would be fabrication.

## 5. Next

1. Phase C substrate: p9_os_xtensa.c - context switch
   (FreeRTOS port as ABI reference), tick timer, interrupt
   lock for os_lock, thread create over proc stacks in DRAM.
   Exit test: test_kernel logic running on-device behind blob
   init, same style as B7.
2. Then link the OSI surface under the RTOS-SDK wifi blobs in
   a build where FreeRTOS is absent (Phase B completion),
   devwifi over it, and IL in devip for the 9P server.
3. Still blocked, unchanged: over-air 9P verification needs
   an unmanaged Wi-Fi path (B9 section 3).

