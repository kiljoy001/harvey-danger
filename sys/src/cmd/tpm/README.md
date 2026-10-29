# tpmfs — userspace TPM 2.0 driver

`tpmfs` owns the TPM transport and exposes it as a 9P file tree.  TPM2
protocol logic stays in userspace; the backend only moves command and
response bytes.

```
/mnt/tpm/status   read-only state ("mock ready", "mock selftest ok", ...)
/mnt/tpm/data     write one complete TPM2 command, seek(0), read response
/mnt/tpm/ctl      write: startup | selftest | getrandom N | pcrread N
/mnt/tpm/rand     read TPM2_GetRandom bytes
/mnt/tpm/pcr/N    read a SHA-256 PCR digest (N = 0..23)
/mnt/tpm/seal     write plaintext, read the sealed blob
/mnt/tpm/unseal   write a sealed blob, read the plaintext
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
- `crb` — userspace TPM2 CRB via `#P/acpitbls` and `#P/acpimem`,
  following the babyFileServer probe.  `#P/acpimem` must be writable
  (hostowner); this is the path used on 9front.

The sources are native Plan 9 C and build with both Harvey's clang and
9front's `6c`/`6l`.  On 9front:

```
cd /tmp; mkdir tpmbuild; cd tpmbuild
T=/path/to/sys/src/cmd/tpm
for(f in tpmfs tpm_crb tpm_tis tpm_mock tpm2_sapi) 6c -I$T $T/$f.c
6l -o tpmfs tpmfs.6 tpm_crb.6 tpm_tis.6 tpm_mock.6 tpm2_sapi.6 \
	-l9p -lthread -lauth -lsec -lndb -lbio
tpmfs -s tpm -t crb &
mount /srv/tpm /mnt/tpm
echo startup >/mnt/tpm/ctl
```

`tpmfs` raises `mainstacksize`: libthread's default 8 KiB `threadmain`
stack is not enough for the ACPI table read plus the SAPI frames.

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

- PCR read decodes the first SHA-256 digest for one PCR.
- Password sessions are used for CreatePrimary/Create/Load/Unseal; there
  are no HMAC/policy sessions or parameter encryption yet.
- `seal`/`unseal` seal under the deterministic owner-hierarchy SRK:
  seal flushes its transient handle, and unseal re-derives the same key
  from the same template.  Persistent SRK handles (`TPM2_EvictControl`)
  and auth policies are future work.
- Verified on babyFileServer's Intel TPM over `crb` (control area
  `0xfed40040`, start method 7): `tpmtest -t crb` passes (Startup,
  GetRandom), and PCR0/PCR7 plus a 19-byte seal/unseal round trip were
  checked by hand.
- `libtpm2.a` is built from `tpm2_sapi.c` (the minimal marshaling layer
  adapted from the lux9 kernel SAPI) for consumers outside tpmfs.
- `tis` needs a kernel `#T` device; `crb` is the hardware path that
  needs only `#P`.
- Bindings for factotum and libtab should consume `/mnt/tpm`, never the
  transport directly.
