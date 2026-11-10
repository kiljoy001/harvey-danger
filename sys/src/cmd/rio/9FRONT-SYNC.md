# plan9front Rio in Harvey — port record

Status: done. This directory is the Rio source copied from
`plan9front/sys/src/cmd/rio` (rio.c, wind.c, wctl.c, fsys.c, xfid.c,
scrl.c, data.c, line.c, props.c, ui1.c, ui.c, ui.h, undo.c, undo.h,
pipe.c, dat.h, fns.h), compiled for Harvey with the plan9port
toolchain compiler (gcc) and linked against Harvey libc/libdraw/
libframe/libthread/libplumb. The browser guest build is
`extras/harvey/build_aspera.py` in the Halatha repo.

## Why gcc, not clang

Plan 9 C anonymous embedding (`Ref; QLock; Frame;` inside `Window`)
is what kencc and gcc's `-fplan9-extensions` accept. Harvey's clang
accepts only `-fms-extensions`, which rejects the resulting
promoted members. All Rio objects are therefore built with
`gcc -fplan9-extensions`; the rest of the guest still uses clang.

## Port shims (all marked in-tree)

- `Window.r` renamed to `Window.run`, `Window.modified` to
  `Window.fmodified` (dat.h + users): GCC rejects a field that
  duplicates a promoted embedded `Frame` member, kencc does not.
- Embedded-Mouse accesses (`w->mc.Mouse`, `mousectl->Mouse`) became
  `(Mouse*)` casts: Harvey's Mousectl flattens the Mouse fields
  instead of embedding them.
- Every libframe call passing a `Window*` now passes `&w->Frame`
  (kencc converts implicitly, GCC does not). This was the root cause
  of dead scrolling/selection: the frame was read at a wrong offset.
- `compat.c`: `min`, `max`, `badrect` (9front libc/libdraw helpers
  Harvey lacks). `frcol.c`: copied from plan9front libframe, whose
  `frsetcol` Harvey's libframe lacks.
- `sys/include/keyboard.h` gained the 9front keyboard runes
  (Kscroll*, Kcaps, Kmod4, control names, media keys), and
  `sys/include/complete.h` the `Completion.force` field.
- Objects are compiled `-mstackrealign`: GCC's aligned SSE spills
  faulted on Harvey's thread stacks, which are not 16-byte aligned.
- `ui1.c savehistory/openproc`: guard a missing `/env/home`
  (`strcat(getenv("home"), …)` faulted at first window creation;
  Harvey's boot namespace sets no `home`).
- `ui1.c wkeyctl`: single control runes from the cons keyboard path
  are translated to the keymap's `C-<letter>` bindings. Harvey has
  no `/dev/kbd`, so `mod` is otherwise never set and every Ctrl
  binding in this fork would be dead.
- `ui1.c self_insert`: the fork's per-keystroke async autosuggest
  is disabled in Harvey. Its completion results raced typed input
  and mangled commands; completion remains on demand (C-tab / ^F).
- `ui1.c keymap_load`: key maps are created with capacity 128/64.
  Upstream creates them with capacity 1 and writes past the
  allocation, which Harvey's malloc does not forgive — key bindings
  (e.g. pgup) were silently lost.
- `ui1.c dellinel/delliner/delwordl/delwordr/lineup/linedown`:
  save and restore the global `shiftdown` around their temporary
  `shiftdown = 1`. Upstream never restores it; on 9front the
  keyboardtap thread continually rewrites it from `/dev/kbd`
  modifier state, but Harvey has no `/dev/kbd`, so after the first
  Ctrl-U every bol/eol/bob/eob extended the selection and the next
  typed character cut the whole input line (garbled commands after
  any Ctrl-U or window switch).

`ui.c` and `pipe.c` are kept for completeness but are not compiled,
matching plan9front's rio mkfile.

The pre-port Harvey Rio (with the Lola pager/workspace blend) is
preserved in the Harvey git history (HEAD) and, with its
uncommitted blend edits, under `/tmp/harvey-rio-blend-backup`.

## Editable titles and framebuffer restoration

Halatha starts Rio with `-t`. The title/frame port now routes title selections
separately from body selections, reserves room for status text, and uses an
opaque title background. File buffers keep their editing mode across title
cancellation, isolate their input/output from the attached shell, and preserve
unsaved text after a failed save. Closing a file restores terminal mode.

Backspace uses explicit completion only, deletes a selected range without an
extra preceding character, and stops at the pending-input boundary. Ctrl+U at
an empty prompt preserves the prompt. Forward deletion handles selections too.

The red caret trails came from libmemdraw's packed alpha multiply: each 16-bit
lane needs its own rounding offset (`0x00800080`). Bochs uses XRGB32 because its
high framebuffer byte is ignored, not alpha. With these fixed, libframe once
again draws the caret after insertion. Halatha compiles libframe from source
when rebuilding Rio, avoiding a stale archive with the old caret workaround.

Regression checks in Halatha: `dev/test-rio-titlebars.mjs` exercises title edits,
saves, drag/resize, prompt deletion and scrolling; `dev/test-harvey-alpha.py`
checks packed channel arithmetic against scalar rounding. Rebuild the Harvey
kernel after changing softscreen/libmemdraw, then rebuild Halatha's guest.

## Directory-first new windows

The New mouse-menu action launches `/bin/vdir`; Terminal retains the interactive
rc action. If vdir is absent, New falls back to rc with a diagnostic. Explicit
wctl and window launches retain their existing semantics. `/dev/theme` exposes
the palette generated by `iconinit`, including vdir's file-color keys.
The browser source and build manifest live in `../vdir`.

Graphical clients that have opened `/dev/mouse` own the complete interior.
Rio does not paint its editable terminal title over their toolbar or intercept
that row as a title drag. Border resizing/moving remains available.

The `open /absolute/path` wctl command creates a directory or text-file window.
`rio-open` is the plumber launch target; when started without `/dev/wctl` it
attaches to `$wsys` with `none`. Successful mount consumes the service descriptor,
so the helper must not close that descriptor number after opening wctl.
Halatha starts plumber before rio, sharing its mount and the `$wsys` environment.
File windows enter file mode before starting their attached shell. The console
write handler also rechecks file mode after receiving a write selected by an
older alt, preventing a startup prompt from being inserted into the document.

## Two-button mouse and lower-right control cluster

Rio supports two-button mice and modern laptop trackpads without breaking classic
three-button Plan 9 conventions:

1. **Lower-right control cluster**: A 48x14 widget is rendered in the bottom-right
   corner of each window border:
   - Cell 0 `[—]`: Minimize / Hide (`whide`).
   - Cell 1 `[□]`: Maximize / Restore toggle (`wtogglemax`).
   - Cell 2 `[×]`: Close / Delete (`wsendctlmesg(w, Deleted, ZR, nil)`).
   - Right-click on the cluster directly opens the Window edit menu (`button2menu`:
     Cut, Paste, Snarf, Send, Scroll) for two-button mouse users.
   - Resizing by dragging the outer corner border remains fully intact.
2. **Keyboard modifier emulation**: Holding `Shift` while right-clicking translates
   Button 3 into Button 2 (Middle Click) across all Rio windows and Plan 9 tools.

## Persistent window controls

The bottom bar reserves `font->height + 10` pixels independently of terminal
frames. `/dev/winrect` publishes the remaining graphical client rectangle;
libdraw's `gengetwindow` reads it on every attach/resize, falling back to the
usual border inset on older servers. Rebuild graphical clients with this
libdraw (the Halatha guest build does this for vdir). Existing statically linked
clients that do not read `winrect` can still draw over the bar.

Buttons use black text and outlines on white, including inactive windows.
Labels become compact symbols only in narrow windows. Hit testing uses the
visible window under the pointer, so inactive controls work on the first click.
`util/9vm/test-rio-controls.mjs` checks redraws, resizing, maximize/restore,
inactive controls, canceled clicks, hide, and close in the browser guest.

## Bounded file opening

`wreadfile` runs filesystem I/O in a separate process with a private descriptor
table, a five-second deadline, and a 4 MiB limit that also bounds streams with
no EOF. A separate UI timer enforces the deadline even when a transport ignores
interrupts. At most eight reads can remain outstanding; late results are discarded
and freed when the server returns. It validates complete UTF-8 before handing
the document to the UI.
`floadtext` inserts the document in one batch, avoiding per-character drawing
and preserving multibyte sequences across read boundaries. Failed loads leave
existing documents intact; cached-buffer paths no longer leak read descriptors.

The Halatha plumbing rule resolves `$file` without synchronous `isfile`/`isdir`
checks. `rio-open` presents errors in a visible window and provides a bounded,
read-only byte preview for binary, non-UTF-8, and oversized files. This is a
fallback inspector, not an image/PDF renderer. `test-rio-plumbing.mjs` exercises
large/empty/repeated text opens, UTF-8 boundaries and exact saves, fallback
previews, missing paths, and a 9P read that never replies (but honors flush).

Cached documents now belong to a specific window. Window teardown disposes
of those buffers and their undo stacks together, preventing the dangling undo
pointer that crashed Rio after reopening a file. Two live windows on the same
path no longer share undo ownership.


## VPS plumber startup

The GEFS/VPS guest enters through `boot/vps_init.o9`, not Halatha's standalone
`aspera_init.o9`. Both boot paths must start the plumber before Rio, provide
`$home` and `/mnt/plumb`, and share their namespace and environment. In
particular, the VPS Rio fork must not use RFENVG: the already-running plumber
needs Rio's subsequently published `$wsys` when launching `rio-open`.

`util/9vm/test-rio-vps-plumber.mjs URL` boots the actual VPS browser page,
checks `/mnt/plumb/send`, then opens a filename containing
spaces and an apostrophe from vdir and verifies an exact edit/save. The VPS
kernel must be rebuilt and its deployed cache version changed for boot fixes;
rebuilding the standalone rootfs alone cannot update that boot path.
