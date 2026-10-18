# o9drvman

`o9drvman` is the Objective-9C control plane for user-space driver services.
It exposes a small 9P view:

* `view/drivers.tab` reports every driver lifecycle as tabular data;
* writes to `view/ctl` submit lifecycle events;
* `view/result` reports the result for that attach.

Each run gets a monotonically increasing generation. Readiness, progress,
exit, and stopped events carry that generation. Reports from an old process
are ignored after a restart, so a delayed exit cannot kill its replacement.
The manager also owns heartbeat deadlines, bounded exponential backoff, and a
restart budget.

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

Driver names are supplied only by the trusted launcher as command-line
arguments. The writable control view cannot allocate new slots. The manager
must run in the supervisor's private namespace; a read-only status service can
be bound into broader namespaces later. AppUser authorization is still needed
before any control view may be exported over a network.

This slice implements and tests lifecycle policy. The next slice connects
these transitions to isolated Objective-9C process launch, notes, and wait
messages. Device-specific services remain separate 9P programs.

Build or test it with the generic Objective-9C C source support:

```
python3 util/9vm/prism.py build sys/src/cmd/o9drvman/service.o9 \
  --c-source sys/src/cmd/o9drvman/lifecycle.c \
  --c-source sys/src/cmd/o9drvman/binding.c -o build/o9drvman

python3 util/9vm/prism.py run sys/src/cmd/o9drvman/test_service.o9 \
  --c-source sys/src/cmd/o9drvman/lifecycle.c \
  --c-source sys/src/cmd/o9drvman/binding.c -m 512 -t 35
```
