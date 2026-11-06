# kernel - Plan 9 proc layer and Wi-Fi OSI adapter (Phase B)

Implements the substrate from doc/PLAN9-OS.md section 4 Phase B:
the primitives the Wi-Fi blobs' OS adapter needs, in Plan 9
shapes, plus the adapter itself with the RTOS's exact symbols.

- `p9.h` / `p9.c` - portable primitives: P9QLock (FIFO ticket),
  P9Rendez (sleep/wakeup under a caller lock), P9Proc registry,
  scheduler suspend/resume gate, bounded fixed-item message
  queues, tick time (1 tick = 10 ms). Memory comes from static
  pools and pp_arena discipline (PLAN9-OS 4.5); there is no
  heap in this code.
- `p9_os_posix.c` - host substrate for tests ONLY. It provides
  the three blocking hooks (os lock, wait/wake, thread create)
  with pthreads so the portable logic can be exercised on a
  workstation. It is never linked into firmware.
- `osi_wifi.h` / `osi_wifi.c` - the blob-facing surface: all
  22 `__wifi_*` functions from the RTOS adapter
  (esp_wifi_os_adapter.c), over the p9 primitives: tasks,
  queues (front/back/overwrite, from-ISR), counting
  semaphores, timers (one service proc), priorities,
  suspend/resume-all, task stack-top query. Static pools:
  8 queues (max 32x32B items), 8 semaphores, 8 timers,
  16 procs.

Phase boundary, honestly: the Xtensa substrate that replaces
p9_os_posix.c (our own context switch, interrupt masking,
tick timer) is Phase C. Until then, p9.o cross-compiles for
Xtensa with exactly the p9_os_* hooks undefined - that symbol
list is the Phase C work order, and `nm` on
build-xtensa/p9.o reproduces it.

Host test:

    cc -std=c99 -Wall -Wextra -Werror -pthread -o /tmp/test_kernel \
      p9.c p9_os_posix.c osi_wifi.c test/test_kernel.c
    /tmp/test_kernel

Known limits of this stage: queue/semaphore timeouts poll at
1 ms (OSI compatibility deadlines; rendez-structured waits do
not poll); __wifi_task_delete only self-deletes - deleting
another proc awaits the Phase C scheduler;
__wifi_task_top_sp returns the current P9Proc (diagnostics
equivalent, not a saved stack pointer).

