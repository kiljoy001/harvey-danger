# vdir in Harvey

Imported from `/home/scott/Repo/vdir` at
`b26bec431aa67f698b453b382b478adbead7d735`, including its file-color changes.
`enter.c` comes from plan9front's libdraw; Harvey does not provide that helper.

Rio's **New** menu item opens this browser. **Terminal** creates an interactive
rc window. In vdir, click **Terminal**, choose **terminal here** from an entry's
middle-button menu, or press `t` to open rc in the displayed directory.
The terminal action uses `/dev/wctl` and does not require a plumber or `window`
script. Paths are quoted for rc, including spaces and apostrophes.

Existing navigation remains: right-click folders to enter them, right-click
files to plumb them, and use the Home/Open/Up toolbar icons. Page Up/Down and
the mouse wheel scroll; Home goes home, Up goes to the parent, `r` refreshes,
and `q` or Delete closes the browser. Middle-click entries to rename/delete.
Deletion is recursive only with `vdir -r`. Browsing works without a plumber;
opening a file reports plumbing errors. Halatha starts the plumber before rio
and installs rules that send directories to vdir and text files to rio's editor
via `rio-open`. Its `/n/host` contains only browser services (`clip` and
`note.txt`), not another copy of the OS root. Missing `$home` falls back to `/`.

The read-only `/dev/theme` served by this rio supplies `name RRGGBB` lines.
Supported keys are `back`, `text`, `border`, `high`, `htext`, `dir`, `exec`,
`link`, `dev`, `pipe`, `broken`, `socket`, and `titleback`.
The browser background uses `titleback` (rio’s pale title-bar tint) when present,
otherwise `back`. Missing or malformed values retain
defaults. A different theme file can be bound over `/dev/theme` before launch.
File defaults follow the supplied bright ANSI palette: directories `5555ff`,
executables `55ff55`, links `55ffff`, devices/pipes `ffff55`, broken links
`ff5555`, sockets `ff55ff`.

Native Plan 9 `Dir.type` identifies the serving device, not a POSIX inode type.
The browser currently classifies directories using QTDIR and executables using
permission bits; other entries use the text color. The other palette keys are
reserved for sources that can identify those types reliably. Extension-based
coloring is not enabled.

Build with Harvey's normal command build, or:

```
python3 9vm/build_vdir.py
```

The latter writes `build/aspera-vdir`; `--out` selects a guest destination and
`--draw` selects a matching libdraw archive for TTF-enabled guests.

The browser VM integration regression is `9vm/test-rio-vdir.mjs`.
It uses Playwright from the sibling Halatha checkout. With that guest built
and served, run:

```
node 9vm/test-rio-vdir.mjs http://127.0.0.1:7072/examples/basic-v64-aspera.html?test
```

It checks the theme endpoint, the New mouse-menu sweep, and Terminal here
from both `/` and a path with spaces and an apostrophe.

`9vm/test-rio-vdir-files.mjs` checks that the hosted `/n/host` tree
cannot recurse into the guest root, that plumber is mounted in new windows,
and that right-clicking a filename with spaces and an apostrophe opens an
editable document whose saved bytes match exactly. It accepts the same page URL.


Text loading and fallback opening are covered by `9vm/test-rio-plumbing.mjs`.
The opener reads text outside Rio's UI process, validates UTF-8, and installs it
in one batch. Binary/non-UTF-8 files and text over 4 MiB get a read-only byte
preview; read failures get a visible error window. Slow reads time out after
five seconds, and the plumber does not stat paths while matching these rules.
