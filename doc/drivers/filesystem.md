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

```
/       ext2 on eMMC, mounted -r     immutable system image
/tmp    #@, the kernel RAM fs        volatile scratch
/mnt    #@, the kernel RAM fs        mounts and scratch
/dev /env /srv /proc   kernel devices, rebound after re-root
```

`ext2srv` is started with `-r`, so it opens the card `OREAD` and never
writes the superblock: the filesystem stays clean, `-F` is not needed,
and a power cut cannot corrupt it.  Writes to `/` now fail with
`read-only file system` instead of being buffered and lost.

Because the root is read-only, every mount point has to exist in the
image.  `util/arm64/mkrootfs.py` builds it from the boot archive and
creates `/bin /boot /dev /env /fd /mnt /n /net /proc /root /srv /tmp`.

## Measurements

A full boot of the read-only layout, with the network up and a ping
answered:

```
msize 65536
read 823808 bytes in 805 requests
write 0 bytes in 0 requests
```

Zero bytes written to the card for the whole boot.  The boot self-test
now reads instead of writing; the old write test is behind
`/env/emmcwtest` for when the write path needs exercising.

## Persistent data

When the system needs storage that survives a power cycle, add a second
partition and mount it read-write on demand.  Keep it small, keep the
system image out of it, and leave `ext2srv`'s write-through `syncbuf()`
in place for that mount: durability is worth the writes when a program
explicitly asked for them.

The eMMC also supports an enhanced (pseudo-SLC) area that trades
capacity for endurance.  Setting it erases the card, including the
bootloader area, so it is not something to do casually; it is worth
considering only after the boot path no longer depends on this card's
current contents.
