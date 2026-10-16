# Sophia storage prototype

This directory contains an Objective-9C rewrite of Sophia's storage policy and
a mountable 9P service. The tiny native wrappers in `store.o9` supply positional
I/O; path validation, record encoding, replay, file operations, and service
behavior are Objective-9C. It is a prototype, not yet a bootable root file
system. The Linux-tied 9vm can expose a GEFS image as an opaque file over its
host 9P transport and run GEFS in guest userland, so this prototype is not
needed for that deployment path.

## Current format

The image starts with `SOPHIA1\n`. Each record has a 32-bit little-endian
payload length, a 64-byte ASCII BLAKE2b digest, and the payload. Payloads
contain an operation, path, mode, and operation data. Create-directory,
create-file, full-file replacement, and delete records remain readable.
New writes use delta records: a 32-bit file offset, one truncate flag byte,
and only the newly written bytes. A one-byte edit therefore appends one byte
of file data plus record metadata, even to a large file. Replay applies
deltas in order; sparse gaps read as zeroes.

Delete records remove names from the live tree. They **do not reclaim disk
space**. Every earlier record remains needed until a safe checkpoint and
reclamation scheme is implemented. Delta records reduce write volume, but
long histories still grow and replay time grows with them. A fixed-size image
returns an error when no more records fit. There is no compression, snapshot,
space allocator, compaction, fsync ordering, or recovery from a torn final
record yet. Corrupt records cause the service to fail closed.

Files are currently capped at 4 MiB and the entry array at 4096 historical
entries. The in-memory path lookup is linear and file writes reconstruct an
in-memory string; the on-disk delta format alone does not solve those limits.

## 9P surface

`SophiaViews` exposes a dynamic `/view` tree. Use `ctl` commands `mkdir /path`,
`mkfile /path`, and `remove /path`; writes to files honor offsets. The current
Objective-9C `FileTree` facade does not route ordinary 9P `Tcreate` or
`Tremove` to applications, so these commands are temporary. The service
does not yet enforce user permissions or offer `wstat`/rename.

The first argument to a standalone Objective-9C service names its `/srv`
endpoint. Sophia takes the image or device path as the second argument:

```
o9sophia sophia /path/to/image
```

The VM gate is `python3 util/9vm/test_sophia.py` from the Harvey root. It
checks offset and sparse writes, log replay, deletion, corruption detection,
the fixed-size image path, two 9P mounts, and persistence across VM boots.

## Next storage step

For a general-purpose VM disk, add bounded blocks or segments, checkpoints,
and a reusable free-space map. A delete should update the namespace and make
unreferenced blocks eligible for reuse; snapshots must keep their referenced
blocks alive. Commit a new root/checkpoint before recycling old blocks so
power loss always leaves a valid version. GEFS uses copy-on-write Bε trees,
snapshots, deadlists, and a free-space log for this; Sophia can adopt the
principles without copying its entire implementation.
