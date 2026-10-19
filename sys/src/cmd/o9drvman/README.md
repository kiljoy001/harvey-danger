# o9drvman

`o9drvman` is the Objective-9C control plane for user-space driver services.
It exposes a small 9P view:

* `view/drivers.tab` reports lifecycle, PID, and last exit status;
* writes to `view/ctl` submit lifecycle events;
* `view/result` reports the result for that attach.

Each run gets a monotonically increasing generation. Readiness, health,
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
wait messages. A normal stop creates
`<status-directory>/<name>.<generation>.stop`. Managed Objective-9C services
consume the generation-scoped marker and use the
runtime's whole-application exit primitive, allowing the monitor to reap every
long-running service cleanly. The manager removes stale control markers before
launch and after reap. If a startup, heartbeat, or graceful-stop deadline
expires, the supervisor posts a forced shutdown note. It restarts desired
drivers only after generation-safe backoff.
Managed services publish generation-scoped `.health` records:

```
o9health/1 GENERATION SEQUENCE TIMESTAMP_MS STATE PROGRESS INFLIGHT COMPLETED ERRORS TIMEOUTS RESETS RECOMMENDATION ERROR_CODE
```

The fields are tab-separated and the record ends in a newline. Sequence,
timestamp, progress, and cumulative counters may not move backward. Healthy,
degraded, and recovering reports renew the lease; stalled, failed, and offline
reports trigger a restart. The supervisor consumes readiness and health before
applying a deadline tick, avoiding a false timeout when a valid report is
already waiting. A report is removed only after a complete parse, so polling
cannot discard a file while its writer is still filling it.

The manager must run in the supervisor's private namespace; a read-only status
service can be bound into broader namespaces later. AppUser authorization is
still needed before any control view may be exported over a network.

The VM integration test launches Objective-9C drivers, verifies that a child
namespace mutation does not escape, reports ready and progress through the 9P
control view, consumes clean and crashed exits, and exercises a
crash/backoff/relaunch generation. It also launches the real `o9driverfs`,
consumes its readiness and health markers, restarts a child reporting failed
hardware, and stops and reaps the blocked 9P service. A noncooperative child
verifies forced termination after the grace deadline.

Build or test it with the generic Objective-9C C source support:

```
python3 util/9vm/prism.py build sys/src/cmd/o9drvman/service.o9 \
  --c-source sys/src/cmd/o9drvman/lifecycle.c \
  --c-source sys/src/cmd/o9drvman/health.c \
  --c-source sys/src/cmd/o9drvman/binding.c -o build/o9drvman

python3 util/9vm/prism.py run sys/src/cmd/o9drvman/test_service.o9 \
  --c-source sys/src/cmd/o9drvman/lifecycle.c \
  --c-source sys/src/cmd/o9drvman/health.c \
  --c-source sys/src/cmd/o9drvman/binding.c -m 512 -t 45
```
