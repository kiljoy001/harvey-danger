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

## Endurance

The card reports its own wear through `emmcfs`'s ctl:

```
wear pre-eol 1 type-a 1 type-b 0
```

`PRE_EOL_INFO 1` is normal and `1` in the type-A estimate means under
10% of rated endurance used.  The boot self-test now reads instead of
writing ~5 KiB of scratch blocks every boot; the write half is behind
`/env/emmcwtest`.

The eMMC also supports an enhanced (pseudo-SLC) area that trades
capacity for endurance.  Setting it erases the card including the
bootloader area, so it is only worth considering once the boot path no
longer depends on this card's current contents.

The eMMC also supports an enhanced (pseudo-SLC) area that trades
capacity for endurance.  Setting it erases the card, including the
bootloader area, so it is not something to do casually; it is worth
considering only after the boot path no longer depends on this card's
current contents.
