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
