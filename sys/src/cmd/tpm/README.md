# tpmfs — userspace TPM 2.0 driver

`tpmfs` owns the TPM transport and exposes it as a 9P file tree.  TPM2
protocol logic stays in userspace; the backend only moves command and
response bytes.

```
/mnt/tpm/status   read-only state ("mock ready", "mock selftest ok", ...)
/mnt/tpm/data     write one complete TPM2 command, seek(0), read response
/mnt/tpm/ctl      write: startup | selftest | getrandom N | pcrread N
/mnt/tpm/rand     read TPM2_GetRandom bytes
```

`data` follows the file offset: after writing a command, `seek(fd, 0, 0)`
before reading the response.  A 9P client that prefers separate fds can
open `data` twice.

## Backends

- `mock` — deterministic TPM2 responder for development and tests.
  Supports Startup, SelfTest and GetRandom.  Random bytes are xorshift,
  not entropy.
- `tis` — raw TPM2 frames over a kernel transport, default
  `/dev/tpm/data`.  This expects a devtpm-style `#T` tree (or a future
  I2C/SPI portal) that owns the MMIO/TIS details.

```
tpmfs -m /tmp/tpm -t mock &
tpmtest -t mock /tmp/tpm
```

With no `-m`, `tpmfs` posts to `/srv/tpm`:

```
tpmfs -t tis -d /dev/tpm/data
mount /srv/tpm /mnt/tpm
```

## What is not here yet

This is the transport and command skeleton, not a TSS:

- PCR read is framed but its response is not decoded.
- No auth sessions, parameter encryption, or key hierarchy.
- No CreatePrimary/Create/Load/Unseal helpers yet; those belong in a
  `libtpm2` layer above `data`.
- `tis` needs a kernel `#T` device.  The next kernel-side piece is the
  `devtpm.c` TIS/CRB implementation; after that the same userspace code
  runs against real hardware.
- Bindings for factotum and libtab should consume `/mnt/tpm`, never the
  transport directly.
