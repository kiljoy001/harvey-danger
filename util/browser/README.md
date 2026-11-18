# Browser pipeline on Harvey/ARM64

The browser is decomposed into 9P file servers that compose through
[9ring](../arm64/README.md) (`#~`), not through sockets or shared memory:

```
controller -> textfs -> layoutfs -> cssfs -> htmlfs
          \-> paintfs -> layoutfs
jsfs (d8) ----------------------------> (script stage)
genwatch -----------------------------> htmlfs (blocks in gen)
```

Each stage is a `lib9p` server whose transport is a named ring endpoint.
A stage mounts its immediate upstream by name, so the namespace (and the
`#~` service names) is the wiring.

## Files

| File | Role |
|---|---|
| `sys/src/cmd/browser/ringfs.c` | announce/listen/connect/mount over `#~` |
| `sys/src/cmd/browser/htmlfs.c` | HTML -> flat node table (`tree`, `links`, `gen`) |
| `sys/src/cmd/browser/cssfs.c` | `<style>` rules + default tag styles (`rules`, `styles`) |
| `sys/src/cmd/browser/layoutfs.c` | node table -> wrapped text + box list (`text`, `boxes`, `width`, `ctl`) |
| `sys/src/cmd/browser/paintfs.c` | box list -> image (`ppm`, `snapshot`, `size`, `ctl`) |
| `sys/src/cmd/browser/jsfs.c` | script stage (`eval`, `result`, `console`, `gen`, `ctl`) |
| `sys/src/cmd/browser/textfs.c` | terminal stage: re-exports layoutfs as text |
| `sys/src/cmd/browser/browsertest.c` | end-to-end pipeline self-test |

## Build and test

```sh
python3 util/browser/test-browser.py --build
```

The test builds `build/arm64-browser/browsertest`, wraps it as init with
`util/arm64/mkrun.py`, boots it under QEMU virt, and expects
`BROWSER-TEXT-OK`.

## Protocol

`htmlfs`:

```
src     read/write    HTML source; a write bumps gen and appends to gen
tree    read          B            block break
                      T<text>      text run
links   read          L <href>     one per link
gen     read          append-only stream of 11-byte generation records;
                      a read past the end blocks until the next write
```

`layoutfs` mounts cssfs (default `/mnt/css`) and serves:

```
text    read           line-wrapped text
boxes   read           id x y w h <text>   (8x16 text cells for now)
width   read           current wrap width in columns
ctl     write          "width N"
tree    read           pass-through of htmlfs tree
links   read           pass-through of htmlfs links
src     read/write     pass-through read; write proxy to htmlfs src
gen     read           pass-through of htmlfs generation stream
```

`cssfs` mounts htmlfs (default `/mnt/html`) and serves:

```
rules     read         text of the <style> blocks in the document
styles    read         default per-tag style table
tree/links/gen/src     pass-through of htmlfs
```

`textfs` mounts layoutfs (default `/mnt/layout`) and re-exports `text`,
`boxes`, `rules`, `styles`, `tree`, `links`, `gen`, and a `src` write-proxy.

`jsfs` serves the script stage:

```
eval      write        script to execute (through /js/d8)
result    read         output of the last script
console   read         alias of result
gen       read         generation counter
ctl       write        "clear"
```

`paintfs` mounts layoutfs and serves:

```
ppm       read         P6 PPM of the current frame
snapshot  read         "PAINT <w> <h> 4\n" + raw RGBA
size      read         "<w> <h>"
ctl       write        "size WxH"
boxes/text/tree/links  pass-through of layoutfs
src       read/write   write proxy to layoutfs src
gen       read         pass-through generation stream
```

The default frame is 640x480 (grown to fit the boxes).  Drawing is a local
RGBA rasterizer today; a libdraw/Memimage or font renderer can replace it
without changing the file protocol.

## Serving model

Both stages serve any number of connections.  The accept loop uses
`proccreate` (RFMEM) per connection, so all connection handlers share the
server's globals; `Srv.threaded` puts each blocking read/write in its own
RFMEM process, so a reader parked in `gen` does not stall a writer on the
same or another connection.  `gen` uses `Rendez`/`rsleep`/`rwakeup` on the
shared `srclk`, which is what lets `browsertest`'s second consumer sleep
until the controller publishes.

## Notes

* The parser is a small tokenizer, not an HTML5 tree builder.  It exists
  to exercise the pipeline; a conforming front end (libhubbub or a
  scripted parser) replaces `htmlfs` without changing `layoutfs`.
* libthread's main thread defaults to an 8 KiB stack; `browsertest` sets
  `mainstacksize = 256*1024` because it keeps buffers there.  Stage
  connection handlers get 64 KiB from `proccreate`.
* `gen` is a blocking stream.  `paintfs` is the first raster terminal; the
  next stages are `cssfs` (style before layout) and `jsfs` (a DOM/JS
  writer that bumps the generation stream).
* `jsfs` runs `d8` as a normal child process.  The hang was never "d8 must
  be init": the APEX `hello` child execs and is reaped, and `d8` prints its
  result then wedges in `OnExit(isolate, true)` — V8's global dispose path
  (`V8::Dispose` / `V8::DisposePlatform` / `isolate->Dispose`).
  `patches/v8-exit-harvey.patch` changes normal exit to
  `OnExit(isolate, false)`, which is what d8's own `quit()` does, so d8
  exits cleanly and the parent reaps it.  `--single-threaded` does not
  change the behaviour; the fault is the global dispose, not worker
  threads.
* One full run showed an intermittent `fault fail, no segment ... pc 0x0`
  in a stage process; the same binary passed on re-run, so it looks like
  the kernel's demand-load race rather than an application fault.
* 9ring service names are global and owner-scoped, so a real tab picker
  should use unique names (`tab0.htmlfs`) rather than relying on the
  mount namespace alone.
