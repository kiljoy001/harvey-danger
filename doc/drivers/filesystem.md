# Filesystem choice for the eMMC

The board boots from a 233 GiB eMMC (HS200, 8-bit) that carries the
Rockchip IDB in its user area.  The card is managed NAND: the controller
does its own wear levelling and error correction, and the filesystem
only controls *what* it asks the card to write.

## What the card says about itself

`emmcfs` reports the eMMC life-time estimates from EXT_CSD:

```
wear pre-eol 1 type-a 1 type-b 0
```

| Field | EXT_CSD | Value | Meaning |
| --- | --- | --- | --- |
| `PRE_EOL_INFO` | 267 | `1` | normal, not near end of life |
| life time type A | 268 | `1` | 0-10% of rated endurance used |
| life time type B | 269 | `0` | not reported |

This unit is effectively unused, so endurance is not an emergency.  The
thing worth controlling is the *write pattern*, and that is a software
decision.

## What accumulates writes

Measured on the earlier read-write layout, one boot wrote to the card
even with nothing else happening:

- `ext2srv` clears `EXT2_VALID_FS` on every mount and writes the
  superblock, then marks the filesystem dirty until the last reference
  goes away.  A board that is power cycled never gets there, so every
  boot wrote the superblock and every next boot needed `-F`.
- The boot self-test wrote 4 KiB of scratch blocks and flushed.
- Any accidental write to `/` landed in the ext2 buffer cache, which the
  server then tried to push to a device it had opened `OREAD`.

## Candidates

| Option | Blocks | Write behaviour | Dependencies | Verdict |
| --- | --- | --- | --- | --- |
| ext2 (`ext2srv`) | 1-4 KiB | in-place metadata, now batched | in tree | **current**, wear-mitigated |
| **gefs** | **16 KiB** | log-structured metadata, snapshots, hash check | libc, libbio, libauth, libavl (341 lines); own 9P | **best wear profile** |
| hjfs | 4 KiB | cache + archival dump, 10 s sync | lib9p, libthread, libsec, libbio (all built) | easiest port |
| fossil (+venti) | 4 KiB | copy-on-write, batched snapshots | libventi, 20k+ lines | large port |
| cwfs | 4 KiB | cache + WORM, append-only | heavy, built for optical | no |
| kfs | 1-4 KiB | in-place metadata | 9.3k lines | no advantage over ext2 |
| dossrv (FAT) | 512 B- | in-place, no permissions | in tree | no |
| f2fs / nilfs2 | - | NAND-aware / log-structured | Linux kernel only | not reachable |

The system filesystem is small and read-mostly, so ext2 with batching
is adequate there; the wear risk sits on the large data partition,
which is where a log-structured fileserver earns its keep.  A first
read-only root was a mistake: it protected the image by making 200 GiB
of the device unusable, which is not a trade worth making on a card
that reports under 10% of its endurance used.

## The layout in use

A 233 GiB card that is under 10% worn does not need to be read-only.
The write budget is large, so the design is a normal split: a small
system filesystem and a large data filesystem, both writable.

```
/        ext2, 128 MiB, sector 0x8000     system
/mnt/data  ext2, 200 GiB, sector 0x48000  data, writable
/tmp /mnt  #@, the kernel RAM filesystem  scratch
```

`ext2srv` serves each partition from `emmcfs`'s `/partN`, init starts the
data server before re-rooting and mounts it afterwards, and both mounts
are read-write.

Writes to the data partition are pushed to the card before the request
is acknowledged (`syncbuf()` in `rwrite`), because that is where a
program asked for durability.  `ext2srv -r` still exists for a
read-only mount and now rejects create/write/remove/wstat with
`read-only file system` instead of buffering them into a cache whose
device is open `OREAD`.

## Creating the data filesystem

A fresh 200 GiB ext2 has only about 13 MiB of non-zero metadata, but
TFTP cannot send a 200 GiB sparse file.  `util/arm64/mkmmcdata.py`
builds the filesystem on the host, extracts the non-zero 4 KiB blocks,
and packs them into one blob with a table of (partition block, count)
runs.  `provision` then copies each run through `emmcfs`:

```
python3 util/arm64/mkmmcdata.py --size 200G --outdir /tmp/mmc
# write /tmp/mmc/mbr.bin to sector 0 from U-Boot (two partitions)
ARM64_MMCDATA=/tmp/mmc/datameta.bin python3 util/arm64/build_kernel.py
# boot, init runs /boot/provision once, then skips it
```

`provision` is idempotent: it skips the partition when the superblock
magic is present and the inode size matches what the blob creates.
The blob is only in the boot archive for the provisioning build; a
normal build leaves it out.

Two things bit us and are worth remembering:

- `ext2srv` assumes 128-byte inodes (`sizeof(Inode)`).  `mke2fs`
  defaults to 256 for ext2 now, and the filesystem mounted as garbage
  instead of failing.  `mkmmcdata.py` passes `-I 128`, and `ext2srv`
  now refuses a superblock whose `s_inode_size` it cannot handle.
- `emmcfs` exposes `/partN` with a **partition-relative** offset.  The
  first version of the table stored card LBAs and wrote every run
  144 MiB too far.

## What actually wears the media

Measured through `emmcfs`'s own request counters on the board, before any
changes:

| Workload | Bytes written to the card |
| --- | --- |
| create one 2-byte file | 36 864 |
| create ten 2-byte files | 368 640 |

Two causes, both fixed:

- `readfile()` stamped `i_atime` and dirtied the inode on **every read**,
  so a directory listing wrote inodes back.  It is now relative atime:
  refresh at most once a day, `-a` restores strict atime.
- `rwrite()` called `syncbuf()` before acknowledging, so the block
  bitmap, inode bitmap, group descriptor and directory block were
  rewritten once *per operation*.  Writes are now batched: a flush runs
  at most once every five seconds, driven by request traffic and on file
  close.  A power cut can lose that window, the same trade-off as
  `ext4 commit=5`.

After the changes, ten 2-byte files cost **3 072 bytes**, and a pure read
workload (repeated `ls`, `cat`) costs zero.

What remains is ext2's in-place metadata: every flush still rewrites the
shared bitmap and directory blocks, and inodes are updated in place.

## gefs port status

Started, not finished.  In place so far:

- `sys/src/libavl` (avl.c from 9front) plus `sys/include/avl.h`, built
  for ARM64 as `build/arm64-o9/libavl.a`.  `schar` is not defined in
  this u.h, so the header uses `signed char`.
- `sys/include/libc.h` carries 9front's `Along`/`Avlong`/`Aptr` types
  and accessor prototypes; `util/arm64/atomic9.c` implements them with
  `__atomic` builtins instead of 9front's hand-written ARM64 assembly.
  `getcallerpc` now tolerates an argument.
- `sys/src/cmd/gefs` and `sys/man/4/gefs` are imported, with a
  `build_gefs` target that links against libavl, libbio, libauth,
  libsec, libmp and libc.

The blocker is Plan 9's anonymous struct member extension:

```c
struct Kvp { Key; Val; };      /* Plan 9: injects k, nk, nv, v */
struct Msg { char op; Kvp; };  /* and flattens two levels */
```

Plan 9's `9c` on this host is a gcc wrapper: it cannot target ARM64 and
it does not implement the extension either, so it is not a way out.

Clang does, under another name: the same construct is the MSVC
anonymous-struct extension, and `-fms-extensions` accepts it
unchanged.  That alone takes gefs from about 590 errors to about 30
without touching the source.  The build now passes it.

What is left is that Plan 9 also lets the member be *named* (`d->Xdir`,
`m->Key`, `f->RWLock`), which the C extension cannot do at the same
time as injection, plus a handful of implicit conversions and one
duplicate member.  Those are a few dozen sites, not a rewrite.

One question to settle before trusting the result: some conversions
pass a `Msg *` where a `Key *` is expected, and `Msg` starts with
`char op`, so the member is not at offset 0.  Plan 9 must be adding the
member offset; a plain cast would not, which would silently corrupt
keys.  Verify that against 9front's own compiler before shipping.

Until this is finished `build_gefs` only runs with
`ARM64_BUILD_GEFS=1` so normal builds stay green.

## Endurance

The card reports its own wear through `emmcfs`'s ctl:

```
wear pre-eol 1 type-a 1 type-b 0
```

`PRE_EOL_INFO 1` is normal and `1` in the type-A estimate means under
10% of rated endurance used.  The boot self-test now reads instead of
writing ~5 KiB of scratch blocks every boot; the write half is behind
`/env/emmcwtest`.

## Is ext2 the right filesystem for this?

ext2 is an in-place filesystem: a create or a rename rewrites the block
bitmap, the inode bitmap, the group descriptor, the directory block and
the inode.  Batching amortises that across a burst and relative atime
removes the read side, which is most of what this system does, but the
per-flush cost stays.

The textbook answer for media with limited endurance is a
log-structured or copy-on-write filesystem: everything is appended
sequentially and metadata is never rewritten in place, so the FTL sees
large sequential writes and does little garbage collection of its own.
Of the options in the table above, the cheap answer is to keep ext2 and
batch, and the structural answer is gefs.

The honest position: with relative atime and batched write-back this
stack is fine for a read-mostly system, which is what it is.  If the
write volume ever grows, the next step is a log-structured file server.
**Gefs is the right one**, not fossil:

- 16 KiB blocks -- four to sixteen times fewer, larger writes than
  ext2, which is what the FTL wants.
- metadata goes through a log (`flushlog`), so block allocation and
  inode updates are not rewritten in place one small region at a time.
- every block is hashed (metrohash64, built in), so corruption is
  detected rather than propagated.
- it keeps a write-back cache of 25% of RAM by default (`-m`), so
  bursts coalesce into sequential writes.
- it has its own 9P server, so it does not need lib9p.

Its only missing dependency is `libavl`, which is a single 341-line
file plus a 26-line header.  That makes the port far smaller than
fossil's.

`hjfs` is the cheaper alternative: ~4k lines, and it already depends on
lib9p, libthread, libsec and libbio, all of which this tree builds for
ARM64 today.  It uses 4 KiB blocks, a write-back cache and a 10 second
sync interval (`SYNCINTERVAL`), so it is also much kinder to the medium
than per-request ext2, but its metadata is still updated in place.

Either one also removes the need for the provisioning machinery: gefs
reams with `-r` and grows to the partition with `-g`, and hjfs reams
with `-r`, so a 200 GiB partition is built on the card instead of being
generated on the host and transferred.

The eMMC also supports an enhanced (pseudo-SLC) area that trades
capacity for endurance.  Setting it erases the card including the
bootloader area, so it is only worth considering once the boot path no
longer depends on this card's current contents.
