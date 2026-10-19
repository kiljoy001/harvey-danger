# o9drvman

`o9drvman` is the Objective-9C control plane for user-space driver services.
It exposes a small 9P view:

* `view/drivers.tab` reports lifecycle, PID, and last exit status;
* writes to `view/ctl` submit lifecycle events;
* `view/result` reports the result for that attach.

Each run gets a monotonically increasing generation. Readiness, progress,
exit, and stopped events carry that generation. Reports from an old process
are ignored after a restart, so a delayed exit cannot kill its replacement.
The manager also owns heartbeat deadlines, bounded exponential backoff, a
restart budget, process launch, shutdown notes, and exit collection.

The supported control records are:

```
start NAME NOW_MS
ready NAME GENERATION NOW_MS
progress NAME GENERATION NOW_MS SERIAL
exit NAME GENERATION NOW_MS
stop NAME NOW_MS
stopped NAME GENERATION NOW_MS
tick NAME NOW_MS
reset NAME NOW_MS
```

Driver names and absolute executable paths are supplied only by the trusted
launcher as command-line pairs:

```
o9drvman o9drvman serial /bin/serialfs network /bin/netfs
```

The writable control view cannot allocate slots or replace executable paths.
Each driver receives its trusted name, generation, slot, and inherited status
directory as `argv[1]` through `argv[4]` and
runs with private descriptor, namespace, environment, rendezvous, and note
groups. Objective-9C uses the trusted name for the service's global
`/srv/<name>` post. After its view is usable, the child writes `ready` to its
generation-scoped status file; the supervisor consumes that marker and rejects
stale generations. A detached per-driver
monitor owns the child wait message and sends a
fixed exit record to the manager, so it cannot consume Objective-9C runtime
wait messages. Stop and timeout paths write `shutdown` to the driver's note
group. The manager restarts desired drivers only after generation-safe
backoff. Long-running Objective-9C services need a service-level shutdown
handler before their stop/reap path can be considered complete.

The manager must run in the supervisor's private namespace; a read-only status
service can be bound into broader namespaces later. AppUser authorization is
still needed before any control view may be exported over a network.

The VM integration test launches a real Objective-9C driver, verifies that its
namespace mutation does not escape, reports ready and progress through the 9P
control view, shuts it down with a note, consumes its exit, and exercises a
crash/backoff/relaunch generation. Device-specific services remain separate
9P programs; connecting the first service to `o9driverfs` is the next slice.

Build or test it with the generic Objective-9C C source support:

```
python3 util/9vm/prism.py build sys/src/cmd/o9drvman/service.o9 \
  --c-source sys/src/cmd/o9drvman/lifecycle.c \
  --c-source sys/src/cmd/o9drvman/binding.c -o build/o9drvman

python3 util/9vm/prism.py run sys/src/cmd/o9drvman/test_service.o9 \
  --c-source sys/src/cmd/o9drvman/lifecycle.c \
  --c-source sys/src/cmd/o9drvman/binding.c -m 512 -t 35
```
