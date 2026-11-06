# B12 - memwatch: memory capture in action, plus register dump

Status: on-device test passed. Board restored to stock AT.

## 1. What was built

`firmware/kernel/memwatch.{c,h}` is a portable, no-heap sample
ring (128 records). A sampler only appends; it never allocates
and never prints. Each record carries tick, a caller-defined
event/phase marker, substrate heap free, arena used/high, queue
count, and the worst proc stack high-water. Dumping happens after
the action under test finishes.

On the Xtensa substrate (`firmware/kernel/p9_os_xtensa.c`,
hooks in `p9_os_xtensa.h`):

- Proc stacks are pattern-painted (`0xA5`) at creation.
  `p9_os_stack_highwater(slot)` scans the untouched low-end run
  to report bytes used on each 4 KB stack. `p9_os_capture_regs`
  snapshots the caller's return PC and SP plus `ps`, `exccause`,
  `excvaddr`, `epc1`, `intenable`, `interrupt`, `ccount`, `sar`,
  `vecbase`, and `prid` via `rsr`. This is a call-site snapshot,
  not an exception frame; that distinction matters when reading
  `epc1`/`exccause`, which hold whatever the last exception left.
- Finished proc slots are now recycled by
  `p9_os_thread_create`, so sequential test phases do not exhaust
  the 6 slots.

The kerntest firmware (`firmware/sdkbuild/kerntest`, builder
`build-kerntest.sh`) runs a sampler proc at 25 ms intervals while
the main context applies a known load: 2048 bytes of real heap
(`pvPortMalloc`), 1024 bytes of a 2048-byte pp_arena, and 3 queued
messages, then releases all of it.

Packaging fix: `gen_appbin.py` appends to `eagle.app.flash.bin`,
so rebuilds silently doubled the image. `build-kerntest.sh` now
removes the stale file first. (The Phase A and libpp build
scripts share the latent bug; noted, not changed here.)

## 2. Verification

Host: `test_memwatch: all tests passed` (ring order, wraparound,
min/max reductions, stack watermark edges) and
`test_kernel: all tests passed` (unchanged).

On-device image `user1-kerntest.bin`, 229908 bytes, SHA-256
`a45226e2712f3bcb59f932d0d9cae6d1c0514c37d540912e061f155c93392cea`,
first header bytes `ea 04`. Flashed at `0x1000`, UART capture:

    MEMWATCH seq=0 tick=256 ev=0 free=13560 arena=0/0 q=0 stack=128
    MEMWATCH seq=5 tick=386 ev=1 free=11504 arena=1024/1024 q=3 stack=160
    MEMWATCH seq=13 tick=594 ev=2 free=13560 arena=0/1024 q=0 stack=160
    KERNEL-TEST pass memwatch-heap-dip
    KERNEL-TEST pass memwatch-arena-peak
    KERNEL-TEST pass memwatch-stack-peak
    KERNEL-TEST pass memwatch-heap-recover
    REGDUMP pc=40206d08 sp=3ffff8d0 ps=00000020 exccause=4 excvaddr=00000000 epc1=401004af intenable=00000501 interrupt=00000044 ccount=67350426 sar=4 vecbase=40100000 prid=00000000
    KERNEL-TEST pass regdump-sp-dram
    KERNEL-TEST pass regdump-pc-irom
    KERNEL-TEST ALL-PASS heap=13560

Readings:

- The 2048-byte heap allocation shows as a 2056-byte dip
  (13560 -> 11504), i.e. allocation plus allocator header, and
  free heap returns to exactly 13560 after release. The sampler
  passes the known-allocation validation: it is measuring, not
  echoing.
- Arena used returns to 0 on release while arena high stays at
  1024, exactly the mark/release semantics of pp_arena.
- Stack high-water for the sampler proc is 160 bytes of its
  4096-byte stack. That is the first real stack measurement for
  the Phase C procs and suggests the 4 KB stacks are generously
  sized, at least for shallow procs.
- REGDUMP is sane: PC in IROM (`0x40206d08`), SP in DRAM
  (`0x3ffff8d0`), VECBASE at `0x40100000`, interrupt pending mask
  `0x44`. `exccause=4` is leftover state, as documented above.

Free heap is 13560 here versus 20168 in B11: the difference is
the memwatch ring, the 2 KB test arena, and other new statics
(`.bss` is 66136 bytes in this image). Memory accounting is now
visible enough to make that a measurement instead of a guess.

## 3. Restore

Full 4 MB stock backup rewritten at `0x0` (hash verified).
`AT` -> `OK`, `AT+GMR` -> the original 1.3.0.0 / SDK 2.0.0 /
Mar 13 2018 string. Board state: stock AT.

## 4. Next steps

1. Lift the sampler into the Phase A 9P firmware behind a `/mem`
   file: one 9P read returns the ring dump, so memory can be
   captured over the air while serving traffic.
2. Apply the stale-flash-bin fix to `build-esp9srv.sh` and
   `build-testapp.sh`.
3. Use REGDUMP in the exception path: capture the same register
   set from the vector handler, where `epc1`/`exccause` are
   actually meaningful, before any Phase C hardening work.
