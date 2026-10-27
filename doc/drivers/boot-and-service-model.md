# Boot and service model

Harvey follows the QNX-style model: a small kernel, a process manager, and
user-space resource managers started from an explicit boot manifest.

## What QNX does

A QNX boot image contains:

- the microkernel
- `proc` (process manager)
- `startup` (board init)
- an optional startup script / `.build` manifest
- driver resource managers: `devc-*`, `devb-*`, `io-pkt-*`, `pci-server`, ...

The boot script declares what must start, in what order, and what happens if a
service fails. Drivers are not “optional” in the sense of being ignored; their
policy is explicit:

```text
required       boot fails or enters recovery if it does not start
optional       boot continues, but the missing service is reported
respawn         restart on exit, with a retry/backoff policy
once            start once; failure is fatal or degraded by policy
```

This is different from both:

- a monolithic kernel that probes everything itself
- a boot that silently continues when the driver manager is missing

## Harvey mapping

```text
QNX                         Harvey
-----------------------     ---------------------------------
microkernel                 Harvey kernel
proc                        process/namespace core
startup                     init
.build / startup script     boot manifest
resource manager            user-space 9P driver service
waitfor /dev/...            wait for /srv/<service> or /dev/<name>
respawn                     o9drvman restart policy
slogger/dumper              health and fault reporting
```

## Boot manifest

A manifest entry should be data, not code:

```text
name       program             provides        requires        policy
cons       /boot/cons          srv:cons        -               required
pnp        /boot/pnp           srv:pnp,#P      -               required
usb        /boot/o9xhci        srv:usb         #P,pnp          required-on-board
audio      /boot/o9audio       srv:audio       usb             optional
display    /boot/o9display     srv:display     #P              optional
```

Per-board or per-image policy chooses `required` vs `optional`. A headless
server can make display optional; a USB audio appliance makes USB/audio
required. The policy is visible and testable.

There is no `driverfs` service. Each driver is already a 9P server;
`o9drvman` is the process/driver manager and is required for images that
launch user-space drivers.

## Readiness and failure

Readiness must be a **bounded** check, not a blocking `open`:

```text
start service
wait up to N ms for /srv/<name>
if present: continue
if absent:
    required -> fail, reboot or enter recovery shell
    optional -> report and continue
```

`alarm`/`notify` or a nonblocking service query should enforce the timeout.
A blocking `open` on a half-mounted service is a bug in the wait path, not a
reason to make the service optional.

## Rules

- The kernel does not know driver policy.
- The driver manager is a required service for images that use drivers.
- A missing required service must be loud and deterministic.
- A crashed driver is restarted according to policy, with a retry limit.
- A crash loop escalates to recovery, not an endless respawn.
- Clients never see `#P`, `#X`, or controller registers.
- The boot manifest is part of the board image, like a QNX `.build` file.

## Current gap

Harvey currently has both a C `drvman` and an Objective-9C `o9drvman`, and
init contains hard-coded waits. That is the wrong shape. The target is:

```text
init reads manifest
  -> starts required services in dependency order
  -> waits with timeout
  -> starts optional services without gating
  -> runs rc
o9drvman
  -> owns launch/health/restart policy
  -> never asks init to guess
```

Until that exists, changing service policy to hide a failed startup is a
workaround, not the model.
