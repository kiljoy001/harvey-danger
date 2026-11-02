# Aspera TrueType port: stages 1–3

Source: sibling plan9front working tree, HEAD
`f8034a765445cd677a01408a86afa1948d17112d`, including local font modifications.
Imported `ttf.h`, libttf, truetypefs, and libdraw font/string/width code.
The source is a partial shaper and COLRv1 renderer, not full OpenType support.

Build and regression entry points are in the sibling Halatha checkout:

```
python3 extras/harvey/build_aspera.py
python3 -m http.server 7072 --bind 127.0.0.1
node dev/test-aspera-fonts.mjs
node dev/test-rio-titlebars.mjs
```

`extras/harvey/build_fonts.py` compiles fresh libttf and libdraw archives and
links the server/probe into Halatha's ignored local/aspera output. The full
build relinks Rio and existing libframe against matching headers. This port
has been validated with that amd64 GCC/ld.lld build, not the whole historical
Harvey JSON/mk build. The libttf mkfile is retained from the source import.

Adaptations to retain on re-sync:

- Preserve Harvey's u32 draw protocol types and i8 Cacheinfo offset. Font and
  Cacheinfo layouts changed: rebuild consumers with matching libdraw.
- Retain seven-argument cachechars for existing libframe; cachecharscluster
  offers the imported extended interface. _cachecharsn bounds remaining input
  separately from cache slots so bounded strings stay bounded and batches do
  not split ligatures. Check length before dereferencing bounded rune input.
- Free capability, shape queue, and cluster image allocations in freefont;
  free the temporary nextclusterlen queue too.
- Compare font capability prefixes when grouping Unicode pages of one font;
  different /s.* ranges from the same face may form a combining cluster.
- truetypefs uses threadmain/threadpostmountsrv/threadexits for Harvey lib9p.
  Halatha mounts it before Rio in the shared parent namespace at /n/ttf.
- Use getcallerpc() without an argument, named TTGDEFRange for GCC type
  compatibility, and correctly typed qsort/getrune callbacks.
- Include the ordinary GPOS mark feature alongside abvm/blwm/mkmk.
- Server cluster boundaries include first-GSUB-ligature rune spans and GDEF
  marks, retaining the existing Indic rules.
- COLRv1 header consumes all five v1 offsets. With no ClipList, scale the
  outline and compute pixel bounds before allocating the bitmap. Release
  temporary outline glyphs; treat negative rendering results as errors.
- Free GDEF class/range storage when closing the font. Explicit returns after
  abort avoid false uninitialized warnings with Harvey's libc declarations.

Guest ttfcheck validates real 9P font opening, rasterization, GSUB, GPOS,
bounded measurement, repeated ligatures across batches, combining clusters,
24/36px rendering, and colour bitmap delivery/capability caching. A generated
COLRv1 red-A fixture tests the no-ClipList fallback without external downloads.

Rio still defaults to its bitmap font. Cluster-aware libframe, the kernel
colour-character path, and the newer Rio font UI are separate later stages.
This service does not fix ShelfViews registration. See Halatha's
extras/harvey/FONTS.md for usage, licensing, and the verified scope.
