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

| Option | In tree | Write behaviour | Verdict |
| --- | --- | --- | --- |
| **ext2, read-only root** | `ext2srv` | none while mounted | **chosen** |
| ext2, read-write root | `ext2srv` | in-place metadata, dirty bit per mount | rejected: churn and fsck need |
| kfs | `sys/src/cmd/disk/kfs` | in-place metadata, write-back cache | no wear advantage, 9.3k lines to port |
| fossil (+venti) | `sys/src/cmd/fossil` | copy-on-write, batched snapshots | best for a writable root, large port |
| cwfs | `sys/src/cmd/cwfs` | cache + WORM, append-only | designed for optical; heavy |
| dossrv (FAT) | `sys/src/cmd/dossrv` | in-place, no permissions | rejected |
| f2fs / squashfs / UBIFS | no | Linux kernel filesystems | not usable: the fs lives in user space |

For an immutable system image there is nothing to gain from a
log-structured or copy-on-write filesystem: the cheapest write is the
one that never happens.  Fossil becomes interesting only if the system
itself must be writable, because it turns every change into an
append-only snapshot with a write-behind cache.  That is a much larger
port and is deliberately deferred.

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
In tree, the candidates are:

| Option | Behaviour | Cost |
| --- | --- | --- |
| ext2 + batching | in-place metadata, amortised | done |
| fossil | copy-on-write, batched snapshots, write-behind | large port (libventi, snapshots) |
| cwfs | cache + WORM, append-only | heavy, built for optical |
| kfs | in-place, same as ext2 | no wear advantage |
| f2fs / nilfs2 | NAND-aware / log-structured | Linux kernel only |

The honest position: with relative atime and batched write-back this
stack is fine for a read-mostly system, which is what it is.  If the
write volume ever grows, the next step is a log-structured file server,
and fossil is the one already in the tree.

The eMMC also supports an enhanced (pseudo-SLC) area that trades
capacity for endurance.  Setting it erases the card including the
bootloader area, so it is only worth considering once the boot path no
longer depends on this card's current contents.

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
In tree, the candidates are:

| Option | Behaviour | Cost |
| --- | --- | --- |
| ext2 + batching | in-place metadata, amortised | done |
| fossil | copy-on-write, batched snapshots, write-behind | large port (libventi, snapshots) |
| cwfs | cache + WORM, append-only | heavy, built for optical |
| kfs | in-place, same as ext2 | no wear advantage |
| f2fs / nilfs2 | NAND-aware / log-structured | Linux kernel only |

The honest position: with relative atime and batched write-back this
stack is fine for a read-mostly system, which is what it is.  If the
write volume ever grows, the next step is a log-structured file server,
and fossil is the one already in the tree.

The eMMC also supports an enhanced (pseudo-SLC) area that trades
capacity for endurance.  Setting it erases the card, including the
bootloader area, so it is not something to do casually; it is worth
considering only after the boot path no longer depends on this card's
current contents.
