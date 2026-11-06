# Making a Plan 9 version of the ESP8266 RTOS

Answer to: "if you look at the RTOS, how can we make a plan 9
version of it?" Based on reading ESP8266_RTOS_SDK v3.4 (commit
89a3f254b63819035f65d9c5dcdae8864f1a6a8a, shallow clone at
`../rtos/ESP8266_RTOS_SDK`, submodules not fetched). This
supersedes the RTOS-vs-NONOS discussion in DESIGN section 6 for
the OS-replacement goal; DESIGN remains the 9P/IL contract.

## 1. What the RTOS actually is (observed)

Boot flow: ROM -> bootloader component -> `call_start_cpu` /
chip_boot -> `startup.c` clears BSS, `heap_caps_init`,
`esp_newlib_init`, then `xTaskCreate(user_init_entry, "uiT", ...)`
and `vTaskStartScheduler()`. `user_init_entry` calls `app_main`.
Everything after that is FreeRTOS tasks and components:

- `components/freertos` - tasks, queues, semaphores, timers
  (tasks.c, queue.c, timers.c...), Xtensa port underneath.
- `components/esp8266` - chip drivers, `esp_wifi.c`, and the key
  seam: `esp_wifi_os_adapter.c` implements the `g_wifi_osi_funcs`
  table (task create/delete, queues, semaphores, timers, delay)
  that the closed Wi-Fi libs in `components/esp8266/lib`
  (libnet80211.a, libphy.a, libpp.a - the same blobs as B0-B5)
  call for every OS service.
- `components/lwip` + `tcpip_adapter` - the TCP/IP stack and its
  event glue. `esp_event` distributes system events.
- `components/newlib`, `vfs`, `spiffs`, `nvs_flash` - POSIX-ish
  C library, file abstraction, flash filesystems.
- ~40 more components (mqtt, http, tls...) - none needed here.

## 2. The key insight

The blobs do not need FreeRTOS. They need the OS-adapter
surface: in the v3.4 tree it is not a table struct but 22 named
`__wifi_*` functions (tasks, queues, semaphores, timers -
measured in re/B10-PHASE-B.md section 1). Whoever provides
those symbols owns the OS the blobs run on.
So a Plan 9 version is built by implementing the adapter over
Plan 9 kernel primitives, then replacing everything above it
piece by piece - the same incremental style as the blob RE, and
it explains the phase order below.

## 3. Mapping: RTOS piece -> Plan 9 replacement

| RTOS piece | Plan 9 version | Notes |
| --- | --- | --- |
| bootloader, ROM (76 functions, B3) | keep | hardware, as before |
| startup/chip_boot to scheduler start | keep, then jump to Plan 9 `main` instead of `vTaskStartScheduler` | one function changes |
| FreeRTOS tasks/queues/semaphores | `Proc`, runqueue, `Rendez`, `QLock`, channels | no MMU: procs share one address space, like Plan 9 kernel processes; stacks 1-2 KB, static tables |
| Xtensa context switch (FreeRTOS port) | ours, last | keep FreeRTOS switch under a Proc API in Phase A; write our own in Phase C using the port as the ABI reference |
| esp_wifi_os_adapter (g_wifi_osi_funcs) | same table, implemented over Proc/Rendez/QLock | the seam; blobs unchanged |
| esp_wifi + blobs (net80211/phy/pp) | devwifi file server: `/wifi/ctl`, `/wifi/status`, `/wifi/scan` (DESIGN section 5) | blobs inside until B-stage RE replaces them |
| lwIP + tcpip_adapter | devip: IP/ARP + **IL first**, TCP fallback (DESIGN section 4) | written from Harvey `sys/src/9/ip/il.c` semantics, sized for one connection |
| esp_event | nothing: events are reads of `/wifi/status` and ctl writes | events become files |
| newlib / vfs / spiffs / nvs | the namespace itself; config as files served by a small flash-backed file server process | no POSIX layer at all |
| app_main | `init` proc: builds namespace (bind devcons, devwifi, devip), starts wifi, starts the 9P server on IL 564, serves console | programs are kernel-linked procs in v1; no ELF loader |
| log/console components | devcons on UART0 (the CP2102 log) | same ctl grammar as `/ctl` |

What is deliberately not ported: Harvey's `sys/src/9/port`
assumes an MMU arch layer (segments, per-process VM). We take
its *shapes* - Proc, Chan, QLock, Rendez, devtab, 9P - and
implement them at kernel-process scale. Per-process namespaces
become one mount table per Proc (small fixed array), which is
where the RAM goes furthest on this chip.

## 4. Phases (each ends working and restorable)

- **Phase A - Plan 9 face on FreeRTOS muscle.** Keep the RTOS
  kernel and blobs. Implement the namespace, devtab, synthetic
  files, and the 9P server (TCP first for host testing) as RTOS
  tasks. This is DESIGN phases 1-3 with the RTOS as substrate.
  Proves the model on hardware fast; FreeRTOS is invisible from
  the mount.
- **Phase B - Plan 9 kernel under the blobs.** Implement Proc/
  Rendez/QLock/channels; re-implement `g_wifi_osi_funcs` over
  them; wifi and devip (now including IL) run as Plan 9 procs.
  FreeRTOS linked out. This is the point where it stops being
  "Plan 9 on RTOS" and becomes the Plan 9 OS with blob drivers.
- **Phase C - our scheduler.** Replace the FreeRTOS context
  switch with our own Xtensa windowed-ABI switch; remove the
  last RTOS component. Init, namespace, and servers unchanged.
- **Phase D - blob-free wifi.** The B-stage RE output (SPEC-
  WDEV-DMA, libpp in firmware/libpp) replaces libpp, then
  libnet80211, then libphy behind devwifi. ROM stays: it is
  mask ROM, i.e. hardware (B3's 76-function contract).

## 4.5 Memory model: Goose-style data stacks (adopted)

From the Goose language (aardappel.github.io/goose): no heap at
all. Dynamic values live inline on compiler-managed **data
stacks**; growth advances a bump pointer; leaving a scope frees
everything above a mark with one store. At most one resizable
value is live per stack, sitting on top, so **memory never
moves and references survive growth**. Links inside a pool are
relative offsets, narrower than pointers. Threads have no
shared memory and communicate explicitly.

We cannot reserve Goose's 2 GB virtual stacks - the ESP8266 has
~50-80 KB of RAM and no MMU. The model still transfers, with
physical slabs instead of virtual reservations:

- **Per-proc arenas instead of a kernel heap.** Each Plan 9 proc
  and each file server gets one statically sized arena (a data
  stack) in a linker-reserved region. Our kernel code does no
  general malloc/free at all.
- **Scoped marks for request handling.** The 9P/IL server takes
  an arena mark per message; parsing and reply assembly bump-
  allocate; reply sent, one store rewinds. Fids and other
  cross-message state stay in the static tables DESIGN already
  mandates - arenas are transient only.
- **Nothing moves.** An arena's capacity is fixed at build
  time, so an allocation either fits or fails with `Rerror
  "no memory"`; it never relocates. Pointers into an arena are
  stable for the scope, which is exactly what descriptor and
  blob-OSI code needs.
- **DMA is not arena memory.** Hardware-owned buffers and
  descriptors live in the separate static pp_esf pool (B5/B7);
  an arena rewind must never be able to free memory a DMA
  descriptor still points at.
- **One growable on top.** Each arena allows a single growable
  buffer, always its topmost allocation (the Goose rule that
  makes stable references provable); everything else is fixed
  size at allocation time.
- **Relative links.** Internal links (fid slots, descriptor
  indices, namespace nodes) are 16/32-bit offsets from a pool
  base, not raw pointers - narrower, relocatable, and safe to
  persist to flash for configuration.
- **The blob boundary is the one heap left.** The Wi-Fi blobs
  call OSI malloc/free with per-allocation lifetimes, so the
  OSI adapter (section 2) provides exactly that - a small
  fixed-block allocator over a reserved slab, visible nowhere
  else. Our code never calls it.
- **Visibility.** Each arena exposes its high-water mark, so
  `/status` can report real memory pressure per server instead
  of a heap free count.

High-water sizing replaces heap tuning: run, read the marks,
set the slab sizes in the linker layout, rebuild.

## 5. Feasibility notes

- RAM: a Proc is a few hundred bytes plus its stack; the 9P/IL
  server design already assumes 16 fids, 1 client, static
  buffers (DESIGN 4.3). Nothing in Phase A/B needs dynamic
  growth beyond what the blobs already use.
- The OSI adapter is bounded: the table in esp_wifi_osi.h is
  tens of entries, each with an obvious Proc/Rendez equivalent.
  It is the single highest-leverage file in the RTOS tree.
- Context switch is the only genuinely hard new code, and it
  is deferred to Phase C when everything above it already runs.
- Toolchain is already pinned (B6) and the flash/restore cycle
  is proven (B7), so every phase exit test is the same: boot,
  mount, exercise, restore stock, verify AT.

## 6. Immediate next step

Phase A substrate decision is already made by this reading:
build on the RTOS SDK (Phase A tasks), not NONOS - because the
OSI-adapter seam and the component layout are what Phase B
needs, and NONOS has neither. The Phase 1 host mock
(esp8266-9p/mock9p, Go, already builds) freezes the namespace
before Phase A code is written against it.

