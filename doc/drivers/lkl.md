# LKL as the final driver architecture

Status: adopted, in progress
Date: 2026-11-05

## Decision

Harvey's board support becomes a **small native core plus one or more Linux
kernel instances (LKL) that own the complex peripherals**, with 9P as the
boundary between them.  Native code provides the things a driver host cannot
provide for itself: boot, console, memory, interrupts, the resource manager,
display/rio, and the Plan 9 network stack.  Everything else is driven by the
real Linux drivers inside LKL.

This replaces the "semantic re-implementation of each Linux driver" plan.  A
re-implementation can only ever approach the original; running the original
is the definition of done.

## Correctness rule

- **The Linux driver is the oracle.**  For this SoC the reference is the
  vendor tree `9rock/linux-rockchip` 6.1.115, because that is what the
  silicon was validated against.  Mainline is a secondary reference.
- **Parity first.**  A behaviour is "done" when it matches the reference:
  same register sequences, same error classification, same retry counts,
  same timeouts, same state transitions, same power behaviour.
- **Deviations are explicit.**  A deviation is allowed only for: an erratum
  workaround, a Plan 9 interface requirement, additive observability, or a
  measured improvement.  Each one is recorded with the reason, the test that
  proves it, and the flag that isolates it.  "Cleaner" or "simpler" is not a
  reason.
- **Hardware capability beyond the vendor stack is a legitimate outcome**,
  but only with a probe, a golden reference, and a number.  The NPU is the
  example: vendor parity defines the supported subset; probes define the
  ceiling.

## Shape

```text
        rc, rio, filesystem servers            (unchanged)
                    |  9P
        +-----------+-----------+
        |  driver servers       |
        |  lklemc, lklusb, ...  |   each = one LKL kernel + 9P export
        +-----------+-----------+
                    |  host ops
        +-----------+-----------+
        |  #P platform bus      |   mmio, irq, dma, clk, reset, regulator,
        |  (kernel)             |   gpio, power-domain, inventory
        +-----------+-----------+
        |  native core          |   boot, console, GIC, timer, MMU, PSCI,
        |                       |   display/rio, #l + IP
        +-----------+-----------+
```

One process per driver **family**, not per driver.  Families are defined by
shared hardware or bus: mmc+sdio, xhci+classes, drm, audio, camera.  Splitting
a family recreates the ownership problem inside one device.

## Resource contract (`#P`)

`#P` is the platform bus.  For every device node it must expose, in a form LKL
can turn into a `platform_device`:

- `reg` / `size` (physical base and length)
- `irq` / `irqflags`
- `compatible`, `class`
- clock and reset references (enable, disable, rate)
- regulator references (enable, voltage)
- power-domain references
- GPIO references (reset, card-detect, enable)
- DMA constraints (address width, coherency, IOMMU)

Ownership: **one owner per device**.  The boot configuration selects native or
LKL for each device; `#P` does not arbitrate, so the selection must.

Open question: whether clocks/regulators/power domains are owned by native
board code (LKL gets fixed-clock/fixed-regulator shims) or by LKL (Linux
clk/regulator frameworks, same FDT).  Boot-critical rails stay native; devices
driven by LKL get their clocks from LKL, one owner per controller.

## Family model (adopted from lux9-kernel)

Lux9's HAL groups hardware into **families** (PCI, USB, I2C) registered in a
`FamilyRegistry` and routed through a congruent 9P tree (`/dev/family/...`).
Each family exposes per-device resources (`config`, `bar{N}`) and a `ctl`;
a channel id identifies an access grant; an exchange page carries zero-copy
requests and IRQ events; a borrow checker enforces single-writer ownership.
Its rump driver server plan is the same shape as this one: a `rumpuser`
backend (clock, threads, locks, block I/O) over a HAL, exposed as 9P,
storage first.

Harvey adopts the parts that fit a process-per-family design:

- **`#P` as the family router.** `#P/<family>/<device>/...` with `reg`,
  `size`, `irq`, `irqflags`, `compatible`, `class`, the resources from the
  contract, and a `ctl` for `claim`, `release`, `reset`, `scan`.
- **Claim/lease instead of a token game.** One owner per device; shared
  readers for monitoring; explicit transfer. `#P` records the owner and
  refuses a second claim. This is the borrow-checker idea reduced to what
  the boot model needs, and it enforces "one owner per device" in the
  kernel rather than by convention.
- **Exchange ring for events (later).** `#P/<dev>/wait` is enough for eMMC;
  a shared ring for IRQ and completion events is the right step for network
  and USB families, where per-event reads dominate.
- **Family manifest and supervisor.** A family registers with the
  supervisor (name, devices, endpoint); the supervisor mounts and restarts
  it. The `lkl.devices` file in a BSP is the manifest.
- **Error translation table.** Linux `errno` to Plan 9 strings, in one
  place, so a driver failure is legible to the 9P client.

Not adopted: WASM-inside-HAL isolation (a process is a stronger boundary),
Pebble/CLR tokens (not needed here), and the kernel "diverter" (Harvey has no
native driver to divert; the server *is* the driver).

Also borrowed from the rump plan: storage first (eMMC only, no SD slot),
the `rumpuser`-style host mapping (identical to the LKL host ops), the 9P
POSIX bridge, boot integration, and the functional test list (stat, read,
write, readdir, seek, rename, write/reboot/read back).

## Memory

- A single physically contiguous arena is reserved at boot from the top of
  usable RAM, before the page allocator exists, and published as `#P/dma`.
- Target size: **1 GiB for LKL plus 256 MiB** for other user-space DMA users.
- LKL's guest RAM is that arena.  `arch/lkl` `__pa`/`__va` become an offset
  translation through the arena base (`virt - ram_va + ram_pa`), not identity.
  Every Linux allocation (page tables, slab, `kmalloc`,
  `dma_alloc_coherent`) then has a real, device-addressable physical address.
- **The arena must sit below 4 GiB** while any enabled controller uses 32-bit
  DMA descriptors (the eMMC ADMA2 path does).  Verify per SoC whether 64-bit
  descriptors are supported before relaxing this.
- Cache maintenance for DMA goes through `#P/dma/clean|inval|cleaninval`.
- DMA on stack or static kernel buffers is invalid; drivers use `kmalloc` or
  `dma_alloc_coherent`.  This is documented, not worked around.

## Host interface

`arch/lkl/include/uapi/asm/host_ops.h` mapped to Harvey:

| LKL host op | Harvey |
| --- | --- |
| `sem_*`, `mutex_*` | `Lock`, `QLock`, `Rendez` |
| `thread_*`, `tls_*` | `rfork` + per-process private data |
| `mem_alloc`, `page_alloc`, `mmap` | the `#P/dma` arena |
| `time`, `timer_*` | `nsec()`, timer threads |
| `ioremap`, `iomem_access` | `#P/<dev>/reg` + `segattach` (as `emmcfs` does) |
| IRQ delivery | `#P/<dev>/wait` thread -> `lkl_trigger_irq()` |
| `jmp_buf_*` | `setjmp`/`longjmp` |
| `print`, `panic` | console |

`ioremap` resolves a physical address to the `#P` node whose `reg` covers it,
maps the 2 MiB big page, and returns the virtual address.  `iomem_access` is
not used for hot paths (the driver uses `readl`/`writel` on the mapping).

## Instances

| Instance | Owns | Exposes | RAM |
| --- | --- | --- | --- |
| `lklemc` | dwcmshc + mmc core + block | `/srv/emmc` block device | 512 MiB |
| `lklusb` | xhci + usb core + hubs + hid + storage | `/srv/input`, `/srv/usbN` | 256 MiB |
| `lklnet` | stmmac or sdio + wifi | raw frames / 9P net | 256 MiB |
| `lklmedia` | vpu/vepu/rga | codec devices | 256 MiB |
| `lklmisc` | thermal, wdt, crypto, rng, pcie | `/srv/misc` | 128 MiB |

Native stays: boot, console, GIC/timer/MMU/PSCI, `#P`, GPIO/I2C/PMIC/UART,
VOP2/HDMI/rio, `#l` + IP, USB PHY/port power.

## Build

- One LKL binary per configuration, cross-compiled on the host.  The board
  does not build LKL.
- One vendor kernel tree per SoC family, pinned by commit; host layer, `#P`
  board file and DMA ops live in clearly separated files so a rebase is
  mechanical.
- The device list is selected at run time (`-d dwcmshc -m 512M -s /srv/emmc`),
  so one binary serves several instances.
- Keep the native `emmc.c` as the fallback path behind a boot switch until LKL
  has read and written sector 0 across a power cycle.

## Build recipe (validated 2026-11-05)

LKL tree: `/home/scott/Repo/lkl` (6.12 with the LKL arch patches).
Host: x86_64, LLVM 23.  Target: aarch64, `ARCH=lkl`, no MMU, 64-bit.

The system `clang` is LLVM 22 and has no AArch64 target; use the LLVM 23
binaries.  `ld.lld` must be told the emulation, and the config's
`OUTPUT_FORMAT` must be set for the target because defconfig detects it with
the host compiler.

```sh
LLVM=/usr/lib/llvm-23/bin
# a directory with clang/llvm-* from LLVM 23 and ld.lld from the system
mkdir -p /tmp/lkl-tools && cd /tmp/lkl-tools
for t in clang llvm-ar llvm-nm llvm-objcopy llvm-objdump llvm-strip; do
    ln -sf $LLVM/$t .
done
ln -sf $(which ld.lld) ld.lld

make -C /home/scott/Repo/lkl ARCH=lkl O=/tmp/lkl-build \
     LLVM=/tmp/lkl-tools/ CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu defconfig

# defconfig detected the host format; override for the target
/home/scott/Repo/lkl/scripts/config --file /tmp/lkl-build/.config \
     --set-str OUTPUT_FORMAT elf64-littleaarch64
make -C /home/scott/Repo/lkl ARCH=lkl O=/tmp/lkl-build \
     LLVM=/tmp/lkl-tools/ CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu \
     LD="/tmp/lkl-tools/ld.lld -m aarch64elf" olddefconfig

# kernel object
make -C /home/scott/Repo/lkl ARCH=lkl O=/tmp/lkl-build \
     LLVM=/tmp/lkl-tools/ CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu \
     LD="/tmp/lkl-tools/ld.lld -m aarch64elf" -j8

make -C /home/scott/Repo/lkl ARCH=lkl O=/tmp/lkl-build \
     LLVM=/tmp/lkl-tools/ CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu \
     LD="/tmp/lkl-tools/ld.lld -m aarch64elf" \
     INSTALL_PATH=/tmp/lkl-install install
```

Result: `/tmp/lkl-install/lib/lkl.o`, ELF64 aarch64 relocatable, ~207 MiB
with debug info (strip before deployment), headers under
`/tmp/lkl-install/include`.  The only undefined symbols are `lkl_bug` and
`lkl_printf`, both provided by the host layer; everything else resolves
inside the object or through `lkl_host_operations`.

M1 therefore reduces to `harvey-host.c` (the ops table, the two symbols,
and the arena) plus a link script that keeps the kernel's `.init.*` sections.

## M1 status (2026-11-05)

The Linux kernel boots inside a Harvey process.  `lklboot` runs
`lkl_init()` + `lkl_start_kernel("mem=128M")` on the board and the kernel
banner and early boot reach the Harvey console through
`lkl_operations.print`:

```text
K: [    0.000000] Linux version 6.12.0+ (scott@pop-os) ... clang 23 ...
K: [    0.000000] memblock address range: 0x10d39000 - 0x18d39000
K: [    0.064470] Memory: 127276K/131072K available
K: [    0.087267] printk: legacy console [lkl_console0] enabled
K: [    0.358419] clocksource: Switched to clocksource lkl
```

The host layer is `sys/src/cmd/lkl/harvey-host.c`: semaphores and
mutexes on QLock/Rendez, host threads on `rfork(RFPROC|RFMEM)` with a
private stack, timers on a polling thread, page memory from `mallocz`.
`sys/src/cmd/lkl/tramp.s` switches to the thread stack before calling
the entry point.

Four things had to be fixed to get this far, each worth remembering:

1. **User binaries must link below `0x40000000`.**  That is the
   kernel/user boundary (`KPTR` in `sys/src/9/aarch64/mem.h`); a binary
   linked at `0x40000000` executes into kernel mappings and kills the
   console.  `build_lkl.py` uses `0x10000000`.
2. **`getpid()` returns 0 in an `RFMEM` child** (it reads `#c/pid`).
   Host threads carry their identity at the base of their own stack
   instead; ids start at 2, and the main thread is 1.
3. **`conf.nproc` was 64.**  LKL's kernel threads plus the built-in
   services exhausted the Harvey process table, which floods
   `resrcwait("no procs")`.  Raised to 256 in `boot.c`.
4. **The semaphore trace floods the UART.**  It is only enabled while
   debugging (`hostdebug` in `harvey-host.c`).

Known limit: the kernel reaches the end of early initcalls and then the
run stops; the console no longer responds.  The next step is to find
where it blocks — most likely a host-thread scheduling/handoff point
(the console thread and the LKL CPU owner).  Debug prints for thread
create/start, timer arm/fire and the kernel console are in place.

## Console output, and why it is not on the hot path

The ARM64 port sets `consuartputs = uartwrite`, which ends in `uartputc()`
busy-waiting on the UART transmit FIFO.  `print()` reaches it through
`putstrn()`, which takes an `ilock` -- so the wait happens with interrupts
masked.  A console reader that stalls (a slow or desynchronised serial
session is enough) therefore stops the clock interrupt, which stops both
preemption and every `tsleep` wakeup: the machine looks asleep, with the
shell and every LKL thread parked.

Two changes address this:

* `uartputc()` now gives the FIFO a bounded number of polls and drops the
  character rather than waiting forever.  Losing console output is much
  better than losing the machine.
* `/dev/lkllog` is a synthetic file backed by a 1 MiB ring buffer.  Host
  diagnostics are written there instead of to `cons`; the ring append only
  takes a short `ilock` around a `memmove` and never touches the UART.
  `cat /dev/lkllog` reads it at the reader's own pace.  The pattern is the
  same as `/dev/kmesg`, but user-writable.

`lklboot` should be run as

```text
/boot/lklboot > /dev/lkllog &
cat /dev/lkllog
```

## M1 complete (2026-11-05)

The kernel now boots reliably and shuts down cleanly, repeatedly:

```text
K: [    0.000000] Linux version 6.12.0+ ...
K: [    0.000000] memblock address range: 0x10d33000 - 0x18d33000
K: [    0.093829] Memory: 127276K/131072K available
K: [    0.119574] printk: legacy console [lkl_console0] enabled
K: [    0.419644] clocksource: Switched to clocksource lkl
lklboot: kernel up
lklboot: halted
```

Three separate faults had to be fixed, each of which presented as "the
machine went to sleep":

1. **Console output could wedge the kernel.**  `print()` reaches
   `uartputc()`, which busy-waited on the UART FIFO with IRQs masked.
   Fixed with a bounded wait; see the console section above.
2. **No preemption on the ARM64 port.**  `_arm64irq` returned straight
   to EL0 without checking `up->delaysched`.  Preemption is now checked
   on the syscall return path.
3. **LKL's idle host task was created too late.**  `arch_cpu_idle()`
   hands the CPU back to the host only through `idle_host_task`, which
   `syscalls_init()` created when the guest executes `/init`.  If the
   guest entered the idle loop before that -- which it does during early
   init -- it spun in `do_idle()` forever, holding the CPU, so the
   pending timer IRQ was never delivered and every host thread stayed
   asleep.  `wakeup_idle_host_task()` now creates the task on first
   demand.  Patch: `util/lkl/idle-host-task.patch`.

The third one is the one that looked exactly like a sleep: the clock
interrupt and `hzclock()` kept running (provable with in-kernel
counters), a `sleep(1)` watchdog never woke, and `/proc` was not even
reachable.  Resolving the sampled PC to symbols showed the guest sitting
in `do_idle`/`arch_cpu_idle`, which named the bug.

## Preemption on the ARM64 port (fixed 2026-11-06)

The port had no preemption point on the interrupt return path:
`_arm64irq` restored its frame and executed `eret` straight back to EL0,
so nothing ever acted on `up->delaysched` raised by `hzclock()`.
LKL's kernel is user-space code, so one guest spin loop starved the whole
machine -- the timer thread, every woken task, everything.

The earlier attempt to call `sched()` from the IRQ path was rejected
because other processes then faulted at `pc 0x0`.  The missing piece was
not the `Ureg` frame: it is `SP_EL0`.  On an exception from EL0 the CPU
leaves the user stack pointer in `SP_EL0`, which is *outside* `Ureg`, and
a context switch inside the IRQ ran another process's user resume path
over it.  `_arm64irq` now saves `SP_EL0` in an extra word at offset 288
and restores it before `eret`, and `arm64irq()` calls `sched()` (clock
interrupt) or `preempted()` (other interrupts) after the handler when the
interrupted context was EL0 and the process is Running.

Verified with a host-layer test rather than a board run.  A thread spins
in user mode (no syscalls); the main thread times a `sleep(10)`:

| kernel | `sleep(10)` returns in |
|---|---|
| before | 1178 ms (only after the spinner finished) |
| after | 132 ms (one 100 ms scheduling quantum) |

The default M1 boot still reaches `lklboot: halted`, so the IRQ-frame
change does not disturb the syscall path.

## Deterministic host-layer test (2026-11-06)

LKL's own tests (`tools/lkl/tests/boot`) exercise liblkl's host ops on
the build host.  They do not touch `harvey-host.c`, which is where the
port's bugs live, so `lklboot -s` now runs the same *payload* against
our host layer:

* a 1 s clock calibration (`sleep(1000)` must return in ~1000 ms);
* a user-mode spin preemption test (see above);
* a strict block-then-post handoff: the poster waits 100 ms so the
  waiter is already asleep;
* a 20 000 round-trip semaphore ping-pong;
* a 4-thread, 80 000-iteration contended mutex.

All five pass.  This isolates the host layer from the guest: when they
pass, a guest stall is not a lost semaphore, a lost mutex, or a clock
problem.

Two host-layer contract bugs were found by reading rather than
bisecting:

* `timer_set_oneshot` computed `nsec() + delta` in signed 64 bits.  LKL
  programs `KTIME_MAX` when `NO_HZ_IDLE` stops the tick, so the sum
  overflowed into the past and the timer fired immediately, forever --
  an IRQ storm exactly when the guest went idle.  The POSIX reference
  host avoids this by splitting the delta into seconds and nanoseconds;
  we now treat anything beyond the representable deadline as unarmed.
* `timer_free` could free the `Timer` while the timer thread was still
  using it.  The thread now owns the object and frees it on exit.

## Root cause (corrected again): jiffies freezes in hrtimer_interrupt

Two earlier diagnoses were wrong, both from misreading the host note
trace:

* "a Harvey timer wakeup is lost" -- the frozen "beat" counter was only
  the host debug-print budget, not a lost timer.  The `CLKP` probe shows
  the Harvey clock advancing the whole run.
* "run_irq receives a corrupted irq" -- the note op `U` collided: the
  host layer's `hsem_up` uses `U` for the cpu-semaphore *pointer*
  (0x10cf5868) while the `run_irq` probe uses `U` for the irq.  `run_irq`
  actually receives the correct irq (1) every time; adjacent `R`/`U`
  pairs all match.

The real chain, now fully traced through the guest:

```text
timer_fn             irq = 1
run_irq              irq = 1          (correct)
handle_simple_irq    -> handle_irq_event   (irq_may_run ok, action ok)
__handle_irq_event_percpu -> action->handler = timer_irq_handler
timer_irq_handler    called 34 times; dev->event_handler = hrtimer_interrupt
hrtimer_interrupt    called 34 times
BUT jiffies is frozen at -29988 (12 ticks after INITIAL_JIFFIES = -300*HZ)
AND the clockevent is never reprogrammed ("arm" stops after the first few)
```

So the guest is in highres mode and `hrtimer_interrupt` runs, but it
neither advances `jiffies` nor reprograms the clockevent.  Guest time
freezes, the nanosleep hrtimer never expires, and the task blocks
forever.

Open question: whether `hrtimer_interrupt` sees an empty hrtimer queue
(expires_next == KTIME_MAX, so the tick_sched timer was never enqueued)
or a stale `now` (so reprogramming always returns -ETIME).  Both point
into the hrtimer/tick-sched setup on this host, not the Harvey timer and
not the IRQ delivery path.

## Root cause: a Harvey kernel timer wakeup is lost (superseded)

The nanosleep payload (`lklboot -t`) hangs after `getpid` returns, and
it is not an LKL scheduler defect at all.  Three independent results pin
it down:

* The LKL guest kernel is fine: `tools/lkl/tests/boot` passes all 35
  tests (including `nanosleep` and 20 concurrent syscall threads) when
  built natively on x86_64 with liblkl's host ops.
* The host layer is fine: `lklboot -s` passes clock, preemption,
  block/wake, 20k semaphore, 80k mutex and `jmp_buf` checks.
* A plain `defconfig` build (no MMC/OF fat options) hangs identically,
  so the configuration is not involved.

The kernel side is the smoking gun.  Attaching lldb to QEMU at the
freeze shows the Harvey kernel in `idlehands`:

```text
idlehands <- sched <- schedinit <- arm64kernelstart <- arm64main
```

Every process is asleep.  The host timer thread's `sleep(1)` -- a 1ms
`tsleep` -- is supposed to wake it constantly so it can fire the LKL
clockevent, but its "beat" counter stops at the freeze: the `tsleep`
timer is lost.  With the timer thread parked forever, no clockevent is
ever delivered, no guest task runs, and the `semacquire`-blocked LKL
host threads are never posted.  The whole machine idles.

This is the same Harvey timer corruption the SMP session has been
chasing (`timerintr` re-arm, `Proc.Timer` recycling, the `TIMERBAD`
diagnostic in `portclock.c`), now with a deterministic reproduction:
boot `lklboot -t` under QEMU.  `lklboot -s` (host only) does not
trigger it because it does not churn `tsleep` the way LKL does.


## Remaining stall

With the console fixed, the host stays alive and the clock keeps running
(`timerintr` and `hzclock` both provably complete), but a host thread that
calls `sleep()` stops waking.  The last trace line is the init task being
handed the CPU, after which the watchdog thread (which only does
`sleep(1)` and print) never runs again.  The fault is therefore in the
Harvey timer list or its interaction with the very high rate of
`tsleep`/`timeradd` calls that LKL generates, not in LKL's own sleep path
(`sem_down`/`sem_up` for kernel threads, `setjmp`/`longjmp` for host
tasks).


## Board race in the initial handoff (2026-11-05)

All four fat/lean configurations boot and halt in QEMU.  On the board the
same archive is timing dependent:

* with `lklboot -d`: reaches `lklboot: halted`;
* without `-d`: stops after `lklboot: init ok`, before the banner.

So the tracing syscalls mask it.  An in-memory liveness watchdog in the
host layer (one `sleep(1)` and one line per second, no host calls on the
hot path) dumps per-thread state and reports:

```
own=0 st: 1D<sem>... 2D<sem>@lkl_cpu_init c0 3R 4P ...
          5..10 D<sched_sem> c0
```

That is: **the CPU is free (`own=0`) and the boot task (thread 2) is
blocked in `hsem_down(cpu.sem)` with the semaphore count at 0.**  The
other kernel threads are parked on their sched_sems (normal) and the
watchdog runs, so the host is alive.  Nobody can post a permit: a lost
wakeup on `cpu.sem`.

Making `lkl_cpu_put()` post a permit unconditionally did not recover it,
so the loss is upstream of the release accounting -- most likely in
`lkl_cpu_get()`'s loop (ownership check, `cpu.sleepers++`, then
`sem_down`) or in the host `semacquire`/`semrelease` path.

Instrumentation notes:

* do not call `lkl_printf` inside `lkl_cpu_put()`: it runs with
  `cpu.lock` held and the write syscall can hang the handoff;
* the probe's `idleforever()` (syscall 17 = `sleep`) is what runs when the
  system is otherwise idle, so seeing it in `hzclock` samples is expected.

## Diagnosis: the guest goes idle and never wakes (2026-11-05)

The board-only stall is a genuine idle, not a busy loop and not a stuck
CPU handoff.  With the handoff ring and only `-d` output (so nothing can
perturb a quiet run), a stalling run shows:

```
own=0 st: 1D<sem> 2D<sched_sem>@threads_init c0 3R 4X ... 5..10D<sched_sem> c0
tr: 2G2/1 212/1 2g2/1 2O5/1 ... 5O2/1 2p2/0 2P0/0 4G4/1 4p4/0 4P0/0 ...
```

Thread 2 (the boot task) runs, does `put`/release and parks on its own
`sched_sem`; another thread then repeatedly takes and releases the CPU
(the timer thread running `t->fn()`).  The CPU is free, every Linux task
is parked, and the timer callback runs -- but no task becomes runnable.

Ruled out by quiet-run retests (each still stalls intermittently):

* 512 KiB host thread stacks (so it is not a `curtid()` stack-mask
  collision);
* `lkl_idle_park()` vs the lazy `kernel_thread()` in `arch_cpu_idle()`;
* publishing the mutex permit before dropping the meta-lock (kept: it is
  correct, but not the cause);
* the `Proc`/`Timer` recycling fix in `port.c` (other session);
* creating the idle host task before the `cpu_startup_entry()` loop
  (`lkl_idle_start()`; kept, but not sufficient).

Working hypothesis: an IRQ delivered from the **host timer thread** runs
the clockevent handler and wakes tasks, but that thread is not a Linux
task, so it cannot `schedule()`.  The idle task is parked on its
`sched_sem`, and only `__switch_to()` posts that semaphore.  With no task
in a position to schedule, the newly runnable task never gets its host
thread woken.

Next design step: make the timer-IRQ path arrange a schedule in a task
context -- for example, have `lkl_trigger_irq()` wake the idle task (or
the idle host task) on its `sched_sem` when `need_resched()`, so the
idle task returns to `do_idle()` and calls `schedule_idle()`.

## Fat kernel experiment (2026-11-05)

A single "fat" LKL -- MMC, MMC_BLOCK, MMC_SDHCI, MMC_SDHCI_PLTFM, plus
OF, COMMON_CLK and the dwcmshc glue -- links and starts, but the guest
stalls in early init: no visible banner, while the host stays
alive (`wd` and `beat` keep ticking).  The lean kernel still boots and
halts.

Bisect so far:

| MMC | OF+COMMON_CLK | result |
|-----|---------------|--------|
| off | off           | boots  |
| on  | off           | stalls |
| off | on            | stalls |
| on  | on            | stalls |

This does not yet locate the stall before `console_init()`: LKL registers
its visible console with `core_initcall(lkl_console_init)`, later in
`kernel_init_freeable()`. Normal driver initcalls also run after
`console_init()`. Host timer liveness alone does not exclude a failed
host semaphore handoff.

Follow-up on the same day: all four saved LKL objects booted and halted
under QEMU `virt`, Cortex-A55, 768 MiB, using the board kernel image and
the existing final linker script. The full image announced SDHCI and
disabled unused clocks before reaching `/init` and halting. On the live
board a fresh MMC-only run created host threads 2 through 6 (guest boot,
watchdog, timer, init and kthreadd), armed a 10 ms timer, fired it once,
then stopped making progress. The guest threads were in `semacquire`;
the watchdog and timer kept sleeping and waking. This narrows the next
investigation to the initial scheduling handoffs, but does not establish
the cause or a fix. No linker-layout change has been justified yet.

`lklboot -d` enables bounded host thread, timer and semaphore tracing,
including the caller addresses to resolve against that exact ELF.
The repeatable QEMU test uses an external archive instead of replacing
generated kernel headers:

```
python3 util/arm64/test_lkl_boot.py --kernel /path/to/harvey.img \
  --elf build/arm64-lkl/lklboot --out /tmp/lkl-test --debug
```

Success requires the Linux banner, `lklboot: kernel up`, and
`lklboot: halted`. The transcript is written to `boot.log` in the output
directory. `--archive-only` prepares `lkl-test.img` for a board run; this
archive replaces `/boot/init` with a launcher that starts the probe and
streams `/dev/lkllog` to the console. It does not exercise actual MMC
hardware or establish that the board-specific stall is fixed.

Consequence: the "fat LKL" plan (one instance with every driver, instead
of one per family) is attractive -- one binary, one DT, shared
infrastructure, much faster bring-up -- but it is on hold until this is
understood.  The lean kernel remains the working base, and the 9P
boundary is unchanged, so the choice can be made later without a
redesign.

## M2: what the eMMC driver needs

The probe proves the path up to the controller: claim `mmc@fe310000`, map
the 2 MiB grant, read the SDHCI version register (`0x0005`).  Driving the
real `sdhci` core needs three more pieces.

**Kernel config.**  The LKL build has `CONFIG_MMC` unset and `CONFIG_OF`
unset.  Enable `MMC`, `MMC_BLOCK`, `MMC_SDHCI`, `MMC_SDHCI_PLTFM`
(and the dwcmshc glue) first.

**Device model.**  `sdhci-of-dwcmshc.c` is an OF driver; with `CONFIG_OF`
off it cannot bind.  Two viable paths:

1. enable `CONFIG_OF`, hand LKL a trimmed device-tree blob (the eMMC node
   plus enough clock/reset providers), and let the unmodified driver
   probe.  This is the most faithful to "Linux drivers as the oracle",
   but needs the CRU/reset pieces present;
2. add a small platform shim in LKL that allocates an `sdhci_host`, sets
   the RK3568 quirks and calls the sdhci core.  The core (command/response,
   ADMA2, error handling) stays the Linux source; only the Rockchip glue
   is ours.  The clocks and resets are already configured by U-Boot, so a
   first bring-up can leave them alone.

**Interrupts and DMA.**  A host thread blocks on `#P/<node>/wait` and
calls `lkl_trigger_irq(51)`; `lkl_trigger_irq` already exists in
`arch/lkl/kernel/irq.c`.  DMA comes from `#P/dma`; the arena is below
4 GiB, which 32-bit ADMA2 requires.

Storage then follows the M3 plan: the block device is served over 9P
(`lkl_sys_pread/pwrite`), not by a native driver.

## Supervision

A supervisor (extension of `drvman`) starts instances in dependency order,
mounts their 9P endpoints, and restarts a dead one.  A killed driver server
must not take storage, console or the network with it.  Consumers tolerate a
disappearing endpoint as they do for any Plan 9 server.

## Milestones

- **M1 — LKL builds and boots for aarch64.** Minimal config, banner printed
  inside a Harvey process; arena handed over as `MemTotal`.
- **M2 — eMMC.** `sdhci` + `sdhci-of-dwcmshc` probe, `/dev/mmcblk0` readable
  with ADMA, then writable, then across a power cycle.
- **M3 — 9P.** `emmcfs` becomes a shim over `lkl_sys_pread/pwrite/fsync`;
  ext2 root and gefs mount and boot from it.
- **M4 — USB.** Enumerate, HID to `/srv/input`, MSC as a 9P block device.
- **M5 — Reuse.** A second family (WiFi or media) proves the model.

## BSP template

A new board is data, not a driver project:

```text
boards/<soc>/<board>/
    board.dts        # inventory: addresses, irqs, clocks, rails
    quirks.c         # "needs X before Y" lines, with reasons
    bsp.md           # what is board-specific and why
    expect.txt       # console/storage/net/usb/display pass criteria
    lkl.devices      # device families this board enables
```

## Validation

- Parity ledger: one row per behaviour, Linux reference vs Harvey, evidence.
- Hardware-in-the-loop: `make BOARD=... test` boots and runs `expect.txt`;
  fails loudly.
- Power-cut test for storage: write, verify, cut, remount, verify.
- Error injection: forced timeouts, short transfers, bad blocks.
- Deviation register: each entry with reason, test, isolation flag, owner.

## Open questions

1. Clocks/regulators: native shims or LKL frameworks (see contract).
2. Whether the RK3566 SDHCI can use 64-bit ADMA, which decides the arena
   placement constraint.
3. LKL on aarch64: supported in principle (`arch/lkl` is generic C), not a
   well-tested configuration; M1 validates it.
4. Vendor-tree policy per SoC family (one tree, or one per family).
5. Display: keep native until a KMS bridge to rio is justified.
