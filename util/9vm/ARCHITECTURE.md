# Plan 9 on Linux: current stack and portability target

The first product target is a Plan 9 environment inside a Linux KVM microVM.
The later target is the same guest system booting on bare metal or under a
standard VM monitor. User programs should see the same namespaces and 9P
services in either case.

## Current microVM

```text
 Linux desktop / server
 +---------------------------------------------------------------+
 | Linux kernel: KVM, memory, threads, host files, display       |
 |                                                               |
 | 9vm runner                                                    |
 |   + KVM vCPU and guest RAM                                     |
 |   + hostfs 9P server and optional netfs                        |
 |   + shared host 9ring or VirtIO 9P devices                     |
 |            |                                                  |
 |============ guest / host boundary ============================|
 | Harvey amd64 kernel                                           |
 |   + Plan 9 processes, per-process namespaces, 9P mount       |
 |   + #~/host shared-ring host transport                        |
 |   + #9/0 VirtIO hostfs fallback, #9/1 netfs                   |
 |   + #~!name/listen and #~name/connect local IPC               |
 |            |                                                  |
 | o9c PID 1 supervisor: boot/init.o9                             |
 |   + builds /dev, /proc, /srv, /env, /n/host namespace          |
 |   + runs Objective-9C actors                                  |
 |            |                                                  |
 | o9c services <------------- #~ local rings ----> clients     |
 |                                            + shell / tools     |
 |                                            + future Aspera GUI   |
 +---------------------------------------------------------------+
```

The host ring and the local rings solve different problems. `#~/host` carries
9P between the guest and the Linux host. A named `#~` connection carries 9P
between two guest processes, even when 9vm uses VirtIO for hostfs. Each local
connection has its own tag and fid space. It uses kernel-owned buffers and
copies full 9P messages; it does not currently map a zero-copy ring into user
memory. The kernel limits local connections to 16, with eight message buffers
per connection. One buffer is reserved for `Tflush` when ordinary requests
fill the others.

The named service contract is:

1. A service opens `#~!name/announce` and keeps that descriptor open.
2. It opens `#~!name/listen` for each client. Each successful open becomes a
   bidirectional 9P message stream and can be passed to lib9p `srv()`.
3. A client opens `#~name/connect` and mounts that descriptor in its own
   namespace. Closing the announcement prevents new connections.

The current VM test mounts two clients concurrently, reads a file through
both mounts, then unmounts them. The test runs with the shared host ring and
with the VirtIO fallback. See `boot/test_local_9ring.c` and `test_boot.py`.

For a GEFS-backed VM, Linux holds an opaque image file. The 9front VM
formatter in `format_gefs_image.py` creates and checks it, then verifies a
GEFS file across a 9front reboot. `9vm -gefs-image image` exports exactly
that file as `image` on a separate VirtIO 9P device, `#9/2`, tagged
`gefs-image`. Directory traversal and create/remove operations are rejected
on this export. Image writes use `O_DSYNC`, so each completed write reaches
the host's synchronous data-write boundary. Linux never mounts or interprets
GEFS.

`build_gefs_guest.py` compiles a guest GEFS server from the 9front source with
small Harvey compatibility adapters. The kernel embeds that server and an
Objective-9C bootstrap in its read-only `#/boot`. With `-gefs-image`, the
bootstrap mounts the image transport, starts GEFS, and mounts its 9P tree at
`/`. The Linux source tree is not served to the guest in this mode. The adapter
also avoids relinking a nonexistent snapshot predecessor with ID `-1` and
uses unsigned byte shifts to decode on-disk integers. The cross-OS regression
writes in Harvey, verifies after a Harvey reboot, then reads the file in
9front. A separate root regression writes, syncs, and verifies across two
isolated Aspera boots. Shell and desktop packaging on the GEFS image and a
power-loss fault test remain before full desktop or host-crash consistency
can be claimed.

`sys/src/cmd/o9sophia` remains an Objective-9C filesystem experiment, tested
by `util/9vm/test_sophia.py`. It is not a prerequisite for persistent storage
in this microVM. A native disk filesystem may still matter for a later
standalone or bare-metal Aspera system.

The optional o9irc package is built from its Objective-9C source and copied
to GEFS by a native 9front VM. Aspera embeds the IRC service in its read-only
boot files and mounts the running service at `/n/o9irc`. Its Objective-9C
runtime records use the kernel RAM filesystem at `/mnt/o9`; chat history is
configured under `/lib/o9irc` on GEFS. The Rio GUI binary is present on GEFS,
but requires the planned `drawcompat` service for display. Harvey's older
libsec cannot perform o9irc's certificate pinning, so this build rejects TLS
connections until that API is available.

## Where o9c and Aspera fit

Objective-9C is a userland language and runtime. `boot/init.o9` is already PID
1 in this VM, and its actors run above Harvey's process and namespace system.
An o9c application may export a 9P tree, use the named `#~` service contract,
or mount another service. 9P tag dispatch is in the kernel transport; mapping
those completions directly to o9c actor futures is still a separate runtime
feature.

Aspera belongs in guest userland above the native Objective-9C `o9display`
service. The repository contains an o9c desktop/theme/window prototype in
`sys/src/cmd/o9aspera`; some files there link to `o9aspera`. Its C canvas and
host SDL viewer are prototypes, not the operating system's display server.
`o9display` owns buffers, surfaces, composition, and input routing over its
native 9P protocol. Aspera owns window policy. A separate, optional Objective-9C
`drawcompat` server translates legacy `/dev/draw` commands into native display
operations. The display service uses a minimal display/input device for
scanout and hardware events. See [DISPLAY.md](DISPLAY.md) for the service
contract, implementation boundary, and VM test.

## Bare-metal and standard-VM packaging

```text
                     same Plan 9 userland
       o9c services + ordinary 9P clients + Aspera
                            |
                     namespaces and #~ IPC
                            |
                       Harvey kernel
                            |
             +--------------+----------------+
             |                               |
       Linux KVM / 9vm                bare metal or VM
       hostfs via #~/host             storage, network,
       or #9 VirtIO                   display drivers
```

The portability rule is to keep host-specific work below the guest's device
and 9P interfaces. A service using `#~` and a client mounting its 9P tree
should behave the same in both packages. The bare-metal drivers, boot path,
and Aspera display/input integration are future work. The present named `#~`
registry restricts a service to processes with the same Plan 9 user name; it
is not a complete multi-user authorization system.
