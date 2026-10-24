# 9vm host 9P transports

To launch the current GEFS-backed graphical Aspera VM from this checkout, run
`./bin/aspera check` followed by `./bin/aspera start`. Run
`./bin/aspera preview` for the host-tree Objective-9C development boot. See
[the startup and packaging notes](PACKAGING.md) for the runtime bundle target.

For the full guest, o9c, Aspera, and later bare-metal layout, see
[the architecture map](ARCHITECTURE.md).

For development boots, `9vm` offers the host directory through two
independent 9P2000 connections:

- `#~/host` uses the shared 9ring transport. The guest init tries this first.
- `#9/0` uses VirtIO 9P. Init falls back to it if the ring is unavailable.
  `9vm -no9ring` disables the shared ring and exercises this path.
  9vm also selects VirtIO automatically when its CPU affinity contains only
  one CPU; the ring's polling thread needs another CPU to avoid stalling the
  guest.

The runner reserves the last 2 MiB of guest physical RAM with a private
Multiboot memory type. Harvey maps it writable but does not give it to its page
allocator. The host and guest address that RAM through different virtual
addresses. The shared region contains a submission ring, a completion ring,
and eight pairs of 32 KiB request and response buffers. A descriptor's
`buf_addr` is a checked buffer index, not a pointer. Release and acquire
operations publish messages and completions. A host thread handles the ring
while the vCPU runs; it uses a separate hostfs fid table from VirtIO.

The guest's `#~/host` file is a normal 9P byte stream usable by `mount(2)`.
The guest copies each request into a shared buffer and each response out.
The host currently dispatches requests in submission order. After a request,
the host spins briefly, then polls every 50 µs while idle. The guest spins
briefly after submitting, then sleeps between completion checks. When there
are no outstanding requests, its reader sleeps until a local writer submits
one. This avoids a VirtIO kick per request, but it is not zero-copy and does
not provide out-of-order execution. `Tflush` is passed to hostfs; it does not cancel an
already running host operation. The existing local `#~` descriptor interface
is separate from the host transport and is not a memory-mapped user API.

Build and run both guest transports:

```sh
make -C util/9vm 9vm
python3 util/9vm/build_boot.py
python3 util/9vm/build_kernel.py
python3 util/9vm/test_boot.py
python3 util/9vm/bench_9p.py -n 3
```

The test boots through each transport and checks a 100,000-byte create,
write, read, compare, and remove cycle in `build/9ring-smoke`.
The 32-KiB buffer bounds one 9P message, not a file. The guest negotiates
an `msize` of 32,768 bytes and uses an `iounit` of at most 32,744 bytes;
larger file operations are split into multiple messages.

## Kernel IP stack and IL

The amd64 kernel now builds the real `#I` stack (`devip`, `ip`, `ipifc`,
`iproute`, `tcp`, `udp`, `rudp`, `icmp`, `icmp6`, `ipmux`, `gre`) and the
improved IL in `sys/src/9/ip/il.c`: IPv6, credit-based flow control, a send
window, and congestion control.  `il.c` is listed in `sys/src/9/ip/ip.json`;
the amd64 build adds the IP sources and the `klibip` subset, while
`sys/src/9/amd64/ipproto.c` supplies the protocol init order that `mkdevc`
normally generates.

The GEFS init binds `#I` at `/net` and brings up `127.0.0.1/8` on loopback.
The old netfs 9P mock is mounted at `/net9p` so it cannot shadow the real
stack.  `nettest=1` runs an IL announce/connect/listen round trip over
loopback:

```sh
util/9vm/9vm -r /tmp/9vm/stage \
  -c "bootargs=local!#9/0 nobootprompt=local gefsroot=1 nettest=1" \
  -disk /tmp/9vm/gefs.img build/harvey.32bit
```

It prints `nettest: server got 'ping'` and `nettest: client got 'pong'`.
There is no external NIC yet: 9vm still exposes the netfs 9P service rather
than a virtio-net device, and `test_net.py` exercises that mock.  The next
step is a virtio-net device in 9vm plus a Harvey ethernet driver.

## Opaque GEFS image

Create a new 512 MiB image using an installed 9front VM as the formatter:

```sh
python3 util/9vm/format_gefs_image.py --base /path/to/installed-9front.img /path/to/gefs.img
```

The script refuses to overwrite an existing image. It formats, mounts, writes
a probe file, syncs, and verifies the probe after a new 9front boot. The image
can then be attached to 9vm with `-gefs-image /path/to/gefs.img`, or to a
Prism workload with `--gefs-image /path/to/gefs.img`. The guest sees one file,
`image`, on VirtIO 9P device `#9/2` (mount tag `gefs-image`). Build the guest
GEFS compatibility server from a sibling 9front source checkout, then boot:

```sh
python3 util/9vm/build_gefs_guest.py
python3 util/9vm/build_kernel.py
util/9vm/9vm -gefs-image /path/to/gefs.img build/harvey.32bit
```

With `-gefs-image`, 9vm defaults to an isolated image boot. The kernel's
read-only `#/boot` contains the Objective-9C bootstrap and guest GEFS server.
The bootstrap starts GEFS, prepares the persistent directories, mounts its
tree as `/`, and binds the console, service, and environment devices. The
runner exports only the image file; it does not serve the Linux source tree.
`-r` is ignored in this mode. `user=<name>` on the kernel command line selects
the initial owner (`glenda` by default); that user must exist in GEFS's
`/adm/users`.

`-boot <guest>` or `-legacy-host-root` selects the host-tree boot path used by
development workloads. Use `-legacy-host-root` for image transport tests
that need the old Objective-9C init. The embedded GEFS bootstrap currently
mounts storage and stays alive; loading shell and desktop programs from GEFS
is the next userland packaging step.

## Native block device (MBR + GEFS)

`install_gefs.py` creates a real disk instead of exporting an image file: the
guest writes an MBR with one type-0x39 partition, adds the partition to
`#S/sdV0/ctl`, and reams it with `gefs -r`.  It then starts GEFS on
`#S/sdV0/plan9`, mounts it through 9P, and copies a minimal bootable root
(`bin/rc`, `bin/ls`, `bin/cat`, `bin/test`, `bin/echo`, `rc/lib/rcmain`,
`boot/boot`) onto it.

GEFS is write-back cached, and its `sync()` only commits the tree for
**active mounts**.  The installer therefore issues `sync` and `halt` while
`/mnt` is still mounted, then re-opens the partition with a fresh GEFS and
checks that `/bin/rc` is really in the committed tree.  Unmounting first
orphans the new files and produces an image that looks fine until the next
boot.  Once the sync is done, the installed tree also survives an unclean
VM kill; GEFS replays its log.

```sh
python3 util/9vm/build_gefs_guest.py
python3 util/9vm/build_kernel.py
python3 util/9vm/install_gefs.py -o /tmp/9vm/gefs.img -s 192
```

The harness boots the result from the partition.  `devsd` parses the MBR when
a unit is first brought online, so `#S/sdV0/plan9` exists without a boot-time
`part` command: type 0x39 maps to `plan9`, 0x83 to `linux`, and the FAT types
to `fat`.  `ASPERA-GEFS-ROOT PASS` means the installed tree was mounted as `/`;
the init then starts `/bin/rc -i` on the console (the desktop path takes over
when `asperadesktop=1`).

```sh
util/9vm/9vm -r /tmp/9vm/stage \
  -c "bootargs=local!#9/0 nobootprompt=local gefsroot=1" \
  -disk /tmp/9vm/gefs.img build/harvey.32bit
```

`bench_boot.py` timestamps the kernel, host-ring, GEFS-root and rc-shell
milestones from the serial log and samples the 9vm process CPU over an idle
window (which includes the host ring's polling thread):

```sh
python3 util/9vm/bench_boot.py --image /tmp/9vm/gefs.img -n 3
```

Run the image transport regression, including a 9front format/reboot and two
9vm boots:

```sh
python3 util/9vm/test_gefs_image.py --base /path/to/installed-9front.img
python3 util/9vm/test_gefs_guest.py --base /path/to/installed-9front.img
```

The guest regression writes through Harvey's GEFS mount, reboots Harvey to
read and check it, exercises the VirtIO root fallback, and boots 9front to
read the same file. It then boots the isolated Aspera root twice to check a
write, sync, and read after reboot. Its write test uses a disposable image.

The common-system milestone test uses a disposable copy of the installed
Aspera image. On each boot it creates two children with private name groups,
mounts GEFS only in the first child's namespace, and proves that neither the
parent nor the second child inherits that mount. It then writes and syncs a
file before rebooting into a verification pass:

```sh
python3 util/9vm/build_kernel.py
./bin/aspera verify
```

This test covers the current image transport and GEFS service. It is the
behavioral baseline for moving the image behind a supervised userspace block
driver; it does not describe the current VirtIO 9P image export as a block
device. The filesystem is synced before the persistence marker; supervised
GEFS shutdown and restart remain a separate lifecycle milestone.

The packaged driver test boots for longer than the manager heartbeat interval
and rejects service restarts. Pass `--base` to exercise formatting and
provisioning as well:

```sh
python3 util/9vm/test_driver_image.py
python3 util/9vm/test_driver_image.py --base /path/to/installed-9front.img
```

## Include o9irc

Build o9irc and provision its binaries into an existing GEFS image using
9front's native GEFS implementation:

```sh
python3 util/9vm/install_o9irc.py --base /path/to/installed-9front.img --image build/aspera-gefs.img
python3 util/9vm/build_kernel.py
util/9vm/9vm -gefs-image build/aspera-gefs.img build/harvey.32bit
```

The image contains `/bin/o9irc-ircfs`, `/bin/o9irc-gui`, and
`/bin/o9irc-cli`. The kernel embeds the matching IRC service binary in
read-only `#/boot`; Aspera starts it when the image contains o9irc and mounts
its 9P view at `/n/o9irc`. The Objective-9C runtime tree at `/mnt/o9` is in
RAM, while IRC history is configured under persistent `/lib/o9irc`. Aspera
mounts the optional `#9/1` network service at `/net` when available.

The Harvey build returns a clear error for TLS connections because its older
libsec lacks the 9front certificate verification API used by o9irc. Plain IRC
is available through the mounted network service. The GUI binary is included,
but its Rio `libdraw` interface still needs the planned `drawcompat` server
before it can display in Aspera.

The disposable end-to-end test formats a fresh image, installs o9irc, boots
Aspera without a host tree, and reads the live 9P view:

```sh
python3 util/9vm/test_o9irc_image.py --base /path/to/installed-9front.img
```

The benchmark performs cached sequential reads and open/close operations
through the guest's mounted 9P tree. On the development machine, three
alternating runs gave these medians (microseconds per guest operation):

| Workload | Ring | VirtIO |
| --- | ---: | ---: |
| 128-byte reads, 2,000 in a burst | 4.10 | 12.73 |
| 16-KiB reads, 500 in a burst | 9.19 | 15.38 |
| 128-byte reads, 50 with 10-ms gaps | 83.06 | 93.77 |
| Open/close, 500 in a burst | 32.63 | 74.69 |

The burst workloads used about two CPU cores with the ring and one with
VirtIO. The spaced reads used about 0.10 and 0.03 cores respectively. During
the read bursts the ring recorded zero VirtIO host kicks and zero MMIO exits;
VirtIO recorded one host kick and two MMIO exits per request. These are
measurements of this VM, workload, and machine, not a general speed guarantee.
The benchmark requires at least two CPUs in its affinity mask. Forcing both
ring threads onto one CPU made this workload much slower than VirtIO, which
is why 9vm uses its fallback there.
