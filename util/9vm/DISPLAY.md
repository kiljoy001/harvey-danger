# Aspera display service

The display stack runs inside Aspera. `o9display` is an Objective-9C service
with one native protocol for image buffers, surfaces, composition, presentation,
and input routing. It does not parse Plan 9 `draw(3)` commands or expose
`/dev/draw`. Aspera is a separate Objective-9C window system: it chooses window
placement and focus, draws decorations, and serves window-specific input
files. The display service routes input according to the focus chosen by Aspera.

```text
Objective-9C GUI ────────────────────────────────┐
                                                │ native 9P buffer/surface protocol
legacy libdraw GUI ──> optional drawcompat ──────┤
                         Objective-9C 9P server  │
Aspera ───────────────> placement and focus ───────┤
                                                ▼
                                 o9display: one buffer store,
                                 compositor, and input router
                                                │
                                      /dev/display, /dev/input
                                                │
                                   9vm or bare-metal backend
```

`drawcompat` is a **separate service**. It implements the standard
`/dev/draw` tree for clients that need it, then sends native buffer and
surface operations to `o9display`. The normal native namespace does not mount
`drawcompat`; a compatibility namespace can bind its `draw` tree at
`/dev/draw`. There is one compositor and one image store in `o9display`.
`drawcompat` has protocol state, not a second renderer or scanout path.

## Native display contract

The native API must support offscreen images as well as visible surfaces.
An image buffer owns pixels, format, stride, clip, and damage. The core offers
bounded operations to allocate and free buffers, upload pixels, clear, blit
or composite, and read back pixels. A surface attaches a buffer and commits
its pending state with a damage region. The compositor consumes committed
snapshots only; a client cannot change a buffer while the compositor reads it.
The first software backend may copy dirty regions at commit. Buffer rotation
or copy-on-write can reduce copying later. Multiple committed buffers may
remain in flight until the compositor releases them.

The native service posts `/srv/display`. A 9P attach creates an isolated
session. The service derives ownership from the attach and never accepts an
owner ID supplied in a client command. Each session has image, surface, byte,
and request limits. Closing the session releases its resources and clears any
focus on its surfaces. The service serializes state changes through one
Objective-9C actor; rendering may use workers, but surface order and focus
routing have one owner.

The proposed native tree is:

```text
ctl                         create/destroy surfaces and buffers
events                      configure, input, frame-done, buffer-release
buffers/<id>/ctl            format, size, stride, clip
buffers/<id>/data           bounded binary pixel upload/readback
buffers/<id>/ops            bounded binary clear/blit/composite commands
surfaces/<id>/ctl           title, attach buffer, damage, commit
surfaces/<id>/status        dimensions, serial, buffer state
outputs                     output geometry, scale, mode, color capability
```

The exact file names and binary operation encoding are still design work.
Control files can use bounded tabula rows. Pixel data and drawing operations
remain binary. The first backend can use XRGB8888 and software composition.
Direct shared buffers need a safe mapping and revocation contract; the
current `#~` IPC carries kernel-owned 9P message copies and is not a
zero-copy shared-memory API. Future protocol versions can add GPU buffers,
fences, color profiles, and HDR metadata when a backend needs them.

Wayland is a design reference for per-client surfaces, atomic commits,
buffer release, input focus, and frame callbacks. It is not a dependency or
the guest wire protocol. Text shaping, bidirectional layout, accessibility,
and touch gestures belong primarily in Objective-9C client libraries and
Aspera's window/input service.

## Optional draw compatibility service

`drawcompat` implements the 9front `draw(3)` lifecycle and binary command
stream: `/dev/draw/new`, per-client `ctl` and `data`, image allocation and
free, clipping, drawing, font caches, window layers, and refresh events. It
keeps legacy image IDs, named images, screens, and font state private to each
client. A visible `flushimage(display, 1)` becomes a native surface commit;
commands modifying offscreen images become native buffer operations. This is
why the native API includes image operations beyond attach and present.

For each legacy draw client, `drawcompat` opens a distinct `o9display`
session. It must preserve that session boundary when proxying requests, so
one legacy client cannot access another client's buffers or input. Closing a
draw client closes its corresponding native session. The display core does
not know legacy image IDs or parse `draw(3)` packets. The bridge can be
started and mounted only where old applications need it.

Aspera supplies decorations, workspaces, placement, and focus policy. As in
Rio, it serves window-specific mouse, keyboard, console, and control files
into each client's namespace. Aspera and `drawcompat` are clients of the same
native display service; neither owns the hardware scanout.

## Current implementation boundary

`sys/src/cmd/o9display/service.o9` now serves a native Objective-9C 9P
prototype through `/srv/DisplayViews`. Each attach gets its own view with
`protocol`, `ctl`, `result`, `buffers`, and `surfaces`. `ctl` accepts `buffer
W H`, `surface W H`, `drop-buffer ID`, `drop-surface ID`, and `close`. A buffer
has binary `data`, `ctl` (`clear RGB`), and `status`; a surface has `ctl`
(`attach ID`, `commit`, `destroy`) and `status`. `close` revokes that session's
surfaces and future writes. Ownership comes from the attach, not client text.

The service limits a session to eight buffers, 4 MiB of pixels, 512 by 512
pixels per buffer or surface, and sixteen surfaces. The state core tracks
commit serials and one in-flight buffer per surface. It rejects writes to a
committed buffer until release. There is no scanout worker to release it yet.
The current protocol does not support blits, damage, input, output modes, or
multiple in-flight frames. It does not start during the normal Aspera boot.
`drawcompat` remains a separate planned service.

The Objective-9C runtime now preserves zero bytes in `readfile`, which is
required for pixel readback, and can obtain randomness from `#c/random` in a
minimal service namespace. A view callback avoids binding a new object
subtree through the same 9P request it is serving. That guard is global and
needs a narrower implementation before unrelated concurrent actor creation
is relied on. The Objective-9C view runtime now offers an opt-in `closeView`
notification on final fid clunk. `o9display` uses it to revoke surfaces and
reuse one of its 64 concurrent attach slots after unmount. `ctl close` revokes
resources immediately while the mount remains active. Short-lived `String`
actors in the write path initially exhausted a 512 MiB VM; the current parser
and buffer methods use bounded stdlib string operations instead. Sustained
memory use and shutdown races need longer-running tests.

Next work is bounded request and actor memory, scanout and input backends,
damage and buffer release events, and then Aspera
and `drawcompat` as clients of this protocol. The existing `o9aspera` C
canvas/SDL code is a host prototype; the target userland services are
Objective-9C.

## Aspera VM desktop preview

The Aspera desktop can now run inside 9vm as Objective-9C userland. From the
Harvey checkout, launch its SDL presentation window with:

```sh
python3 util/9vm/aspera_live.py
```

The guest manages focus, dragging, workspaces, themes, and the shelf view. It
also runs a Notes client in a separate Objective-9C offscreen canvas. The
client receives focused keyboard input and saves its text to
`build/aspera-live/note.txt` with Ctrl+S. The host SDL process only presents
completed frames and forwards mouse/key records. The shelf has workspace
buttons, a window list, a local clock, and a menu to open Notes or change
themes. The shelf state and hit routing run in a separate Objective-9C guest
program, `shelf_service.o9`, which Aspera mounts through `/srv/ShelfViews` at
`/mnt/shelf`. Its 9P view exposes `protocol`, `widgets`, `layout`, `state`,
`ctl`, and a per-attach `result`. Aspera sends clock and click commands to
`ctl`, reads the resulting action, and paints the returned state. Commands
are bounded and clock values are validated. The `/srv` posting and mount use
the guest namespace; no AppUser or network authentication is involved yet.
Click a window name to focus it; use Alt+1 through Alt+4 to change
workspaces, Alt+s to shade,
and Alt+p to pin the focused window. Escape exits the preview.

The launcher compiles a private guest binary and uses 9vm's `-boot` override;
it does not replace `boot/boot`. Guest frames and input pass through a bounded
9P display device under `build/aspera-live/dev`. Its payload lives in two
shared-memory frame slots and an input queue; the files are device endpoints,
not frame snapshots. The SDL bridge consumes committed slots and releases
them before the guest reuses them. This is an operational VM display path,
but the `o9display` service is not yet wired to Aspera, and the normal Aspera
boot does not start this desktop. The preview uses a fixed 1024 by 768 output
and one trusted guest display producer. Notes currently renders printable
ASCII with its built-in 8 by 14 font. It is therefore still a desktop
prototype rather than a complete multi-client display service.

## Remote view boundary

The host presenter now calls `vmdisplay_client`, a module with no SDL or
network dependency. It owns the single shared-memory ring consumer and copies
each committed frame before releasing the slot. SDL paints that copy and
turns host input into records. Multiple independent host processes must not
consume the ring directly: they would race to release the same frame. A host
broker could mirror copied frames, but that would only work for the 9vm case.

The drawterm-like remote view belongs at the guest display service. It must
serve a VM, a bare-metal machine, or a headless server from the same committed
surfaces and input policy. The target split is:

```text
Objective-9C apps -> o9display <- Aspera window and focus policy
                         |                |
                         |                +-- input arbitration
                         +-- local scanout -> 9vm ring -> SDL
                         +-- remote view stream -> network viewer
```

The remote view needs versioned frame serials, dimensions, pixel format,
damage rectangles, and frame acknowledgement. Slow viewers should skip to
the newest complete frame; they must not hold a guest buffer indefinitely.
Input goes the other way and needs a single active controller, with optional
view-only sessions. The remote endpoint must authenticate before accepting
input or clipboard data. Resize, clipboard, reconnect, and compression belong
in that remote protocol, not in Aspera's window policy. A 9P control tree can
expose sessions and permissions while a binary frame stream carries pixels.
The network transport and authentication are not implemented yet.

The shelf is the starting point for Enlightenment-style gadgets. Its
workspace, window list, clock, and theme menu are now modeled by a separate
Objective-9C service with mounted views. Aspera still paints the shelf with
its local canvas, so the `layout` view describes the current fixed geometry
but does not yet drive composition. A production gadget should have a bounded rectangle, receive only
its own pointer/key events, and publish a surface through the native display
service. A 9P status/control tree can let users configure placement and
query a gadget without putting gadget logic in Aspera or the host backend.
The next useful gadgets are a launcher and a notification/status area. Keep
shelf policy (placement and focus) in Aspera and gadget state in
separate Objective-9C services. Widget visibility currently belongs to the
shelf service; a later integration with `o9display` can give each gadget its
own surface.

The shelf view protocol is `o9shelf 1`. `ctl` accepts `clock HH:MM`,
`click X Y`, `hide clock`, and `show clock`, one command per write. `state`
reports `HH:MM menu-open clock-visible`; `widgets` lists each stable widget
ID, kind, and visibility; `layout` lists the current rectangles. `result`
reports `action N` for commands sent through that attach. A second attach
cannot overwrite the first attach's result. The service currently shares one
desktop state across local attaches, which is appropriate only until login
and per-user views exist.

Run guest state/property tests, the display control state-machine test, two
source mutations, and a headless SDL frame/input/Notes round trip with:

```sh
python3 util/9vm/test_aspera_live.py --mutate --window
```

Run the VM unit, generated property, and 9P tests with:

```sh
python3 util/9vm/test_o9display.py
```

Run the three targeted source mutations as well with:

```sh
python3 util/9vm/test_o9display.py --mutate
```

The runner requires explicit PASS markers from the guest, and each mutation
must produce an explicit FAIL marker. It rejects panics and timeouts.
