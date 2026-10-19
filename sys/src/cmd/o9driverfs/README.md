# o9driverfs

`o9driverfs` is the Objective-9C user-space driver filesystem. A trusted
launcher names already mounted backend files when starting it:

```
o9driverfs serial0 /n/host/dev/serial0 disk0 /n/host/dev/disk0
```

It posts a 9P service through the Objective-9C view runtime. Each attach
exposes `view/protocol` and `view/drivers/<name>/data`. Reads and writes to
`data` are forwarded to the configured backend. Driver names are restricted to
ASCII letters, digits, `_`, and `-`; duplicate names and relative backend
paths are rejected. Backend files must exist at startup: the service never
creates one.
Each operation is capped at 64 KiB.

When `o9drvman` launches the service, it passes the trusted service name,
generation, slot, and status directory. `o9driverfs` then reads
`/mnt/o9driverfs-config/<service>.tab` instead of accepting backend paths from
the managed process command line. The file has a fixed two-column schema:

```
name\tbackend
serial0\t/n/host/dev/serial0
```

It must end in a newline, contain 1 to 16 records, and stay below 8 KiB.
After installing its view controller, the service writes `ready` to
`<status-directory>/<service>.<generation>`. The manager consumes this exact
marker, so an old process cannot make its replacement ready.
For shutdown, it watches
`<status-directory>/<service>.<generation>.stop`. An exact `shutdown` record
wakes the root Objective-9C task; returning from the generated application
entry then invokes the runtime's whole-application exit path. The generation
in both control paths prevents a late stop from terminating a replacement
process.

This first service is a 9P device namespace and forwarding layer. It is not
yet an MMIO driver, an interrupt broker, or a complete serial, block, network,
or display implementation. Those device-specific services should run as
Objective-9C processes and attach to this namespace after the ARM64 `#9`
virtio transport and user process launch work. The VM test uses a regular
file as a backend to verify the 9P view and data path.

Build and test from the Harvey repository root:

```
python3 util/9vm/prism.py build sys/src/cmd/o9driverfs/service.o9 -o build/o9driverfs
python3 util/9vm/prism.py run sys/src/cmd/o9driverfs/test_service.o9 -m 512 -t 35
```
