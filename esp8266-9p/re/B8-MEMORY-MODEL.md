# B8 - Goose-style memory model adopted and prototyped

Continues B7 and the RTOS reading in doc/PLAN9-OS.md. No
flashing; host and cross-compile verification only.

## Decision

The Plan 9 OS memory model is the Goose data-stack approach
(aardappel.github.io/goose), adapted to a chip with no MMU and
~50-80 KB RAM. Full text: doc/PLAN9-OS.md section 4.5.

What transfers: no general heap in our code; per-proc/server
statically sized bump arenas; scope exit frees by rewinding to
a mark; at most one growable buffer per arena, always on top,
so nothing ever moves and references survive growth; internal
links as relative offsets; explicit communication between
procs (Goose's no-shared-memory threads match Plan 9 channels).
What does not: Goose's 2 GB virtual reservations. Our slabs
are physical and sized from high-water marks reported in
/status. The one true heap left in the system is inside the
Wi-Fi blob OSI boundary, whose per-allocation malloc/free
contract the blobs impose (PLAN9-OS section 2 seam).

## Prototype: firmware/libpp/pp_arena.*

`pp_arena` implements the model in C99: init over a caller slab,
aligned bump alloc, mark/release (rewind), reset, a single
growable buffer with in-place growth, and high-water query.
It joins pp_lldesc/pp_esf in libpp. DMA buffers are excluded by
design - hardware-owned memory lives in the pp_esf pool, never
in an arena that can rewind.

Verified:

- Host: `cc -std=c99 -Wall -Wextra -Werror` build of
  lldesc+esf+arena+tests -> `test_pp: all tests passed`,
  including stable-address assertions (allocation after a
  rewind reuses the same address; growth returns the identical
  pointer with contents intact; non-growable growth and
  exhaustion return NULL without moving anything).
- Target: xtensa-lx106-elf cross-build under the same warning
  set, archived into build-xtensa/libpp_clean.a.

## Next

Use pp_arena in the Phase A servers (PLAN9-OS section 4):
the 9P server takes one mark per message and releases after
the reply; arenas get linker-sized slabs once high-water data
exists from the host mock and device runs.

