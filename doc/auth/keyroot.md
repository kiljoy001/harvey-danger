# keyroot — hardware-rooted secret store

> **Design update:** the private-key path now follows the cryptographic
> oracle model in `doc/auth/crypto-oracle.md` (Approach B). The trusted
> application holds the private keys and performs the signing,
> decryption, MAC and protocol operations; `factotum` holds only public
> parameters. The seal/unseal/derive interface in this document remains
> only for opaque secrets such as the `secstore` password. Use the
> oracle document for authentication keys.

Status: design. No keyroot code exists yet; this document fixes the
interface, the blob format, and the backend split before implementation.

`keyroot` is a small userspace file server that performs exactly three
operations on small secrets: **seal**, **unseal**, and **derive**. It is
the only component that talks to a root of trust. `factotum` and
`secstore` consume it through a library, never through a hardware
transport.

```
factotum ── libkeyroot ── /mnt/keyroot ── keyroot ── backend
   │                                           ├── optee  (ARM64)
   └── secstore (portable, password-encrypted) └── tpm    (x86/VMX)
                                               └── mock   (tests)
```

## Terms used in this document

- **Trusted Application (TA)** — a small program that runs inside the
  processor's secure mode, separate from the normal operating system.
  It holds keys and performs the encryption/decryption.
- **OP-TEE** — the open-source secure-mode operating system running on
  the board. It is the `tee-1` firmware image in the boot container.
- **Pseudo Trusted Application (PTA)** — a service built directly into
  OP-TEE, not loaded from a file. The only one usable on this board is
  the one-time-programmable memory reader.
- **Hardware Unique Key (HUK)** — a key inside the chip that OP-TEE uses
  as the root for deriving other keys. On this board it comes from a
  manufacturing lot code, so it is shared by boards from the same lot.
- **One-Time Programmable memory (OTP)** — tiny permanent storage inside
  the chip, also called eFuses. It can be written once and cannot be
  changed. The board's OTP key slots are empty.
- **Original Equipment Manufacturer (OEM)** — here, Rockchip and the
  board vendor. The OEM signing key is the key Rockchip uses for its own
  Trusted Applications.
- **Shared memory (SHM)** — a memory region both the normal world and
  the secure world can access, used to pass requests and data.
- **Remote Procedure Call (RPC)** — here, the secure world asking the
  normal world for help, for example to read a file.
- **AES-256-GCM** — a standard encryption method that also detects
  tampering. It will be used to seal secrets.
- **Derive** — compute a key deterministically from the root key and a
  label (for example `factotum/nvram`). The same label always produces
  the same key.
- **keyroot** — the name of this secret-store service. It exposes
  `seal`, `unseal`, and `derive` as small files.
- **9P** — the file protocol Plan 9 uses. keyroot presents its operations
  as files under `/mnt/keyroot`.
- **SHDR_TA, type 0** — the outer header format of a Trusted Application
  file. Type 0 is a plain signed application with no extra version
  record. It avoids the secure-storage version database that our small
  file server does not implement.
- **FIT** — the firmware container U-Boot loads. It holds the bootloader,
  secure firmware, and device tree.

## 1. Goals and non-goals

Goals:

- One interface for sealing, unsealing, and deterministic derivation of
  small secrets (up to 64 KiB) on every platform.
- The plaintext of a sealed blob is never written to persistent storage
  by `keyroot`; the caller owns the blob.
- The root secret (HUK, TPM seed, or test key) never leaves the backend.
- A blob is bound to a label. Unsealing under a different label fails
  authentication, not just policy.
- The same blob can be backed up to `secstore` and restored on a
  replacement machine only if that machine has the same root of trust.

Non-goals:

- File encryption, disk encryption, or large-object storage.
- Multi-user isolation on one machine. `keyroot` v1 is a single-user
  (hostowner) service; it does not try to defend against a compromised
  hostowner.
- Attestation, PCR policy, or remote provisioning in v1.
- Replacing `secstore`'s password encryption. `secstore` remains the
  portable backup; `keyroot` is the unattended local unlock.

## 2. Threat model and the honest limits of this board

The design must not claim more than the hardware gives.

What `keyroot` protects against:

- Offline theft of a sealed blob: the blob is useless without the
  backend's root secret.
- Copying a blob to a different machine: the backend key differs.
- Mixing blobs between purposes: the label is authenticated.
- Accidental corruption: the blob is authenticated (AEAD or TPM).

What it does **not** protect against on the stock Orange Pi 3B:

- A local attacker who can run code as hostowner. That attacker can talk
  to the SMC gate, load a TA, and use the same root secret.
- A different board with the same lot code. The vendor BL32's SSK is
  derived from the OTP die ID; on this unit the readable ID is the lot
  code `M2Y401`, not a unique serial. The secure-OTP HUK path is not
  fully resolved, so treat the root as lot-code-bound until it is. The
  OEM HUK write path (`storage_write_oem_huk`) is stubbed out in this
  build.
- TA substitution. The vendor BL32 embeds OP-TEE's **default** TA public
  key (`a65a18ad…`, matching `optee_os/keys/default_ta.pem`; the
  modulus is at offset `0x6c0ad` in the extracted BL32 ELF, `0x6b0ad`
  in the raw `tee-1` image). The matching private key is public. Anyone
  who can load a TA can sign one that claims the same UUID. TA
  authentication is therefore not a trust boundary on this board.

Consequences for the product:

- On ARM, `keyroot` is tamper-resistance and convenient unattended
  unlock, not a per-device hardware root of trust.
- The x86/VMX TPM backend is the stronger story and the one to lead
  with when a real per-device root is required.
- A per-device root on the RK3566 requires one of: a one-time OEM OTP
  key burn (write, currently out of scope), or an upstream OP-TEE
  rk3568 port with our own TA and signing key (large, auditable work).

The OTP state measured on the board (2026-10-30):

| Item | Result |
|---|---|
| NS OEM zone | 32 bytes, all `00` |
| OEM key slots 0–3 | blank |
| OEM key slot 4 | not supported |
| Vendor BL32 | OP-TEE 3.13.0-1064-g5f256dc1a, `rk3568_bl32_v2.18.bin` |
| TA loader | REE FS store + `shdr_verify_signature` present |
| Default TA key | modulus present in BL32 at file offset `0x6c0ad` |

### Chosen direction (2026-10-30)

The target root is a **one-time OEM OTP key**, not `huk-lot`. The key is
read only inside the TEE and used as a key-encryption key (KEK); it is
never exported to the normal world. `keyroot` will generate a random
32-byte master key in the normal world, wrap it with the OEM key through
the vendor PTA, and store only the wrapped master. At boot, the wrapped
master is unwrapped in the TEE and returned to `keyroot`, which uses it
for AES-256-GCM blobs and deterministic derivation. The OEM key itself
never leaves the secure world.

This gives a per-board root without a BL32 rebuild. It costs one
irreversible OTP write, so the provisioning tool is separate from the
normal `keyroot` path and is guarded (see §6.7).

The TA path is now **working**: the vendor OEM signing key from the
Rockchip SDK is accepted, and a type-0 (`SHDR_TA`) TA loads, opens a
session, and derives a key (§6.1, §6.5). This is the supported v1
mechanism: the TA derives per-label keys from the device HUK via
`PTA_SYSTEM_DERIVE_TA_UNIQUE_KEY` and will implement AES-GCM seal/unseal
under those keys.

The OEM OTP key is still the desired stronger root, but the vendor
`rk_crypto_service.ta` that performs `rk_oem_otp_key_cipher` is a
vendor-signed user TA, not a PTA, and it is not in the open
`rk_tee_user` SDK. `OPEN_SESSION` with its UUID requests
`/lib/optee_armtz/0cacdb5d-....ta`; absent, it returns
`TEE_ERROR_ITEM_NOT_FOUND`. `rk_os_service` is the same kind of user TA.
The only built-in service that answers without a TA is
`uboot_storedata_otp` (`2d26d8a8-...`), which exposes OTP read and
key-is-written only.

So v1 seals under the HUK (lot-code-bound on this board). The OEM OTP
key remains a future upgrade once the vendor cipher TA is obtained from
an Orange Pi/Rockchip vendor image or SDK; the TA framework, OEM
signing key and loader are already in place. A discrete TPM/ATECC or an
upstream OP-TEE rk3568 port remain the stronger-root options.

## 2a. Security decision guide

No unattended service that can decrypt on request protects against a
malicious process running as the host owner. That process can simply ask
the service to decrypt. A hardware root protects against stolen storage
and copied blobs; a user passphrase or PIN is what protects against a
compromised host.

| Threat | Current: lot-code chip key + our trusted application | One-time-memory key + vendor secure program | Separate security chip (TPM/ATECC) | Upstream secure-mode port |
|---|---|---|---|---|
| Stolen storage card or backup | Yes, unless the thief has a board from the same lot | Yes | Yes | Yes |
| Stolen whole board | No; the board can decrypt | No; the board can ask its own service | No, unless the chip requires a PIN per operation | No, unless a PIN is required |
| Another board from the same lot | No | Yes, if the key is unique per board | Yes | Yes, with a per-device key |
| Malicious root process on the board | No; the service is an oracle | No; the service is an oracle | No; the service is an oracle | No; the service is an oracle |
| Attacker reflashes the boot firmware | No; Rockchip's signing key is public | No; vendor software is closed and its key is public | Partial; independent chip, but the host can still ask it | Partial; secure boot with our own key |
| Code can be audited | Partial; our trusted application is auditable, the vendor secure mode is not | No; vendor program is closed | Partial; chip firmware is closed, interface documented | Yes |
| Effort / risk | Working today | Irreversible key burn; vendor program missing | Hardware and wiring | Large porting effort |

A practical reading:

- If the threat is **stolen storage or a leaked backup**, the current
  lot-code option is adequate for a first version. Label it honestly as
  `root=huk-lot` and do not claim per-board protection.
- If the threat is **a stolen board or another board from the same
  lot**, the current option is not enough. Choose a separate security
  chip, find the vendor one-time-memory program, or port the secure-mode
  operating system upstream.
- If the threat is **a malicious root process**, add a user passphrase
  or PIN at boot. No hardware root alone solves it.
- If the threat is **firmware reflashing**, add secure boot. The current
  secure mode cannot stop it because Rockchip's signing key is public.

## 2b. Intended uses

### secstore and factotum

`secstore` stores authentication keys and passwords in a remote file
encrypted with a user passphrase. `factotum` loads those keys at boot.
On the ARM board there is no non-volatile RAM to hold the passphrase, so
today `factotum` cannot start unattended.

`keyroot` fixes that by sealing the passphrase or the key list with the
chip's built-in key and storing only the sealed blob. At boot, `keyroot`
unseals it and hands it to `factotum`. This protects the blob at rest:
a stolen storage card or leaked backup is useless without the board's
chip key. It does **not** stop a stolen board or another board from the
same manufacturing lot, and it does not stop a malicious process with
administrator-equivalent rights on the running board.

### Yggdrasil identity

A Yggdrasil node has a long-lived private key that determines its
address and identity. "Burned into the device" can mean three different
things:

1. **Sealed identity (achievable now).** `keyroot` stores the private
   key encrypted under the chip key. At boot it unseals the key and
   gives it to the Yggdrasil daemon. The key survives reinstalls and is
   not a plain file, but a malicious process or a stolen board can read
   it while the daemon is running.
2. **Non-extractable identity (larger project).** The secure world holds
   the private key and performs the signing and decryption. The daemon
   never sees the key, so it cannot be copied to another machine. A
   malicious process on the running board can still ask the secure world
   to sign or decrypt, so this does not stop impersonation while the
   board is running. It requires changing Yggdrasil or adding a proxy.
3. **One-time-memory identity (blocked).** The private key is written to
   the chip's permanent memory. Without the vendor secure program that
   uses such a key, we cannot use it. With that program, the same
   oracle/non-extractable trade-off applies.

For the stated purpose, the sealed identity is the natural first step
for both `secstore`/`factotum` and Yggdrasil. If the board itself may be
stolen, add a personal identification number or passphrase at boot. If
the key must never be copyable even by an administrator-equivalent
process, plan the non-extractable design and the Yggdrasil changes
before claiming that property.

### If the key must be unusable on a stolen board

A board that can boot and unseal the key by itself contains everything
needed to unseal it. "Unusable on a stolen board" therefore requires
something the thief does not have:

- **A user passphrase or PIN entered at boot.** The seal is bound to both
  the chip key and the user secret. Without the secret, the board cannot
  unseal the key. This works with the current hardware, but the device
  cannot boot unattended.
- **A separate security chip or token.** The key is sealed in the chip
  and the chip requires the user's PIN or the token's presence. This is
  stronger, but adds hardware.
- **A remote unlock service.** The board contacts a server that releases
  the key after authenticating the device or user. This needs a network
  and a trusted server.

Without one of these, a stolen board can unseal the key. Sealing under
the chip key alone protects storage, not the board.

If the device must boot unattended **and** the key must be unusable on a
stolen board, those two requirements conflict on this hardware. One of
them has to give: accept the stolen-board risk, require a human secret,
or add a separate security chip or token.

If a passphrase is used, it must be a real passphrase, not a short PIN:
an attacker with the board can try guesses against the sealed blob.
Use a memory-hard key derivation function, a long passphrase, and
rate-limiting in the secure program. The passphrase is never stored.

## 3. The 9P interface

`keyroot` posts `/srv/keyroot` and is normally mounted at
`/mnt/keyroot`. The tree is flat and small:

```
/mnt/keyroot/status     read-only text: backend, version, capabilities
/mnt/keyroot/ctl        write-only control: flush, reload, lock, unlock
/mnt/keyroot/seal       write a request, read the sealed blob
/mnt/keyroot/unseal     write a blob, read the plaintext
/mnt/keyroot/derive     write a label, read derived key material
```

All files are mode `0600` owned by the hostowner except `status`
(`0444`). The server serializes backend calls with a single mutex; a
transaction lives in the fid, so two clients cannot interleave one
seal operation. The client ABI is declared in
`sys/include/keyroot.h`.

### 3.1 `seal`

Write `label '\n' plaintext` (the first newline terminates the label;
everything after it is binary). Read back the blob. Example:

```
label = "factotum/nvram"
plain = sizeof(Nvrsafe) bytes (109 on this tree)
blob  = 60 + len(label) + len(plain) bytes
```

The label is authenticated as part of the blob header. A blob sealed as
`factotum/nvram` cannot be unsealed as `tls/example.com` even if the
ciphertext is otherwise valid.

### 3.2 `unseal`

Write the blob; read the plaintext. The label is carried in the blob
header; the server checks it is a syntactically valid label but does not
need a caller-supplied one.

### 3.3 `derive`

Write `label` optionally followed by `'\n' length` (default 32, range
16–64). Read that many bytes. Derivation is deterministic for a given
backend, root secret, and label. It is intended for protocol keys, not
for storing a secret: the output is handed to the caller and is gone
from `keyroot` afterwards.

### 3.4 `status`

Text, one `key=value` per line:

```
backend=optee
version=1
root=huk-lot          # huk-unique | huk-lot | tpm | mock
seal=yes
derive=yes
locked=no
ta=6b1d7c2e-...       # optee only
```

`root=huk-lot` is the honest label for the stock RK3566. A deployment
that burns an OEM key or ports OP-TEE reports `huk-unique` or `otp-key`.

### 3.5 `ctl`

- `flush` — zero any cached master key and forget in-memory state.
- `reload` — re-read the backend configuration.
- `lock` / `unlock` — for backends that support a user passphrase
  (future); v1 accepts them as no-ops or returns `unsupported`.

### 3.6 Errors and limits

- Errors are Plan 9 error strings: `keyroot: unseal: authentication
  failed`, `keyroot: bad label`, `keyroot: blob too large`.
- Maximum plaintext: 64 KiB. Maximum label: 64 bytes.
- Labels: `[A-Za-z0-9._/-]+`, no leading `/`, no `..`, no NUL. A label
  is a purpose, not a path; `/` is allowed only as a namespace
  separator (`factotum/nvram`, `tls/example.com`).

## 4. Blob format

All integers are big-endian. The format is versioned and backend-tagged
so a blob can be rejected with a useful error instead of a decryption
failure.

```
offset  size  field
0       6     magic "KROOT1"
6       1     version (1)
7       1     backend (1=mock, 2=optee, 3=tpm)
8       1     alg (1=AES-256-GCM, 2=TPM2-sealed)
9       1     label_len
10      2     reserved (0)
12      4     plain_len
16      4     blob_len
20      4     flags (0)
24      8     reserved (0)
32      12    nonce
44      16    tag
60      label_len    label
60+label_len  ...   ciphertext (or TPM blob)
```

For AES-256-GCM backends, the AAD is bytes `0 .. 60+label_len` (the
fixed header plus the label), and `ciphertext` is `plain_len` bytes.
For the TPM backend, `nonce` and `tag` are zero, the payload is the
TPM2 sealed blob, its length is `blob_len - 60 - label_len`, and label
binding is done by sealing `label '\0' plaintext` inside the TPM blob;
`keyroot` checks the label after unsealing.

The blob is self-describing, so `keyroot` can reject a blob from the
wrong backend before asking the backend to decrypt it. A future version
can add a key epoch or a PCR policy without breaking v1 blobs.

## 5. Backend contract

Internally:

```c
typedef struct Keyroot Keyroot;
struct Keyroot {
    char *name;
    int  (*open)(Keyroot *, char *arg);
    int  (*seal)(Keyroot *, uchar *label, int nlabel,
                 uchar *plain, int nplain, uchar *blob, int *nblob);
    int  (*unseal)(Keyroot *, uchar *label, int nlabel,
                   uchar *blob, int nblob, uchar *plain, int *nplain);
    int  (*derive)(Keyroot *, uchar *label, int nlabel,
                   uchar *key, int nkey);
    void (*close)(Keyroot *);
};
```

The server owns blob framing and label validation; backends only see
raw label/plaintext/ciphertext and implement the root operation. This
keeps the OP-TEE and TPM code from duplicating parsing.

## 6. OP-TEE backend

### 6.1 Why a self-signed TA is possible

The vendor BL32 contains the REE FS TA store
(`core/kernel/ree_fs_ta.c`), `shdr_verify_signature`, and the default
OP-TEE TA public key modulus. The modulus in the BL32 is byte-for-byte
the one in `optee_os/keys/default_ta.pem`; the matching private key is
public. A TA built against OP-TEE 3.13 and signed with
`keys/default_ta.pem` will therefore pass the signature check without
rebuilding BL32.

**Measured 2026-10-30: the default key is rejected, the OEM key is
accepted.** The vendor BL32 contains two TA public keys:

- default OP-TEE key at `0x6c0ad` (rejected with "TA signd by old default
  key is not support now! please resign TA!")
- OEM Rockchip key at `0x6c2ad`

The corresponding OEM private key is public in the Rockchip
`rk_tee_user` SDK (`v2/export-ta_arm64/keys/oem_privkey.pem`). A TA
signed with it passes `shdr_verify_signature`.

The vendor BL32 also requires the TA to be a plain `SHDR_TA` (type 0),
not the `SHDR_BOOTSTRAP_TA` emitted by `sign_encrypt.py`. The bootstrap
header makes the core use `ta_ver.db` in the REE FS; our probe's minimal
FS supplicant does not implement the secure-storage format, and the load
fails in `check_update_version()`. `ta/sign_type0.py` in this tree signs
`struct shdr | sha256 | RSA-PSS | ELF` with no bootstrap subheader, so
the version DB is skipped.

With the OEM key and type-0 signing, the probe TA loads, opens session 1,
and PING/INFO/DERIVE all succeed. DERIVE returns 32 bytes from
`PTA_SYSTEM_DERIVE_TA_UNIQUE_KEY`.

**Security note:** the OEM private key is committed to a public SDK, so
it is not a secret and TA authentication is still not a strong boundary.
The value of the OEM key is compatibility with the vendor BL32, not
authentication. The gate must remain hostowner-only, and the TA file
must live on a read-only or hostowner-owned path.

### 6.2 Transport

No kernel TEE driver is added. The ARM64 `#P` device (`devpnp`) already
provides the two primitives a userspace OP-TEE client needs:

- `#P/dma/base`, `#P/dma/size`, `#P/dma/alloc`, `segattach("dma")` for
  physically contiguous, cache-managed shared memory.
- A new `#P/tee/smc` file: write eight `u64` registers, read four. The
  kernel executes `smc #0` at EL1 and returns. It whitelists the OP-TEE
  fast-call range (`0xbf000000`–`0xbf00ffff`), refuses PSCI and other
  secure-monitor calls, and checks that any message-argument physical
  address lies inside the DMA arena.

This is a privileged instruction gate, not a TEE driver: no sessions,
no TA loading, no policy in the kernel. The OP-TEE client, shared
memory registration, session handling, and supplicant RPC live in
`keyroot`, over `#P`.

The message argument and the shared buffer are built in the DMA arena:

1. `#P/dma/alloc` a 4 KiB (or larger) buffer; compute its physical
   address from the DMA base and the `segattach` mapping.
2. Register it with `OPTEE_MSG_CMD_REGISTER_SHM`
   (`CALL_WITH_ARG`, `TMEM_OUTPUT`, contiguous buffer).
3. Open a session to the TA UUID, then invoke commands with `RMEM_*`
   parameters referencing the registered `shm_ref`.

### 6.3 Supplicant RPC

The vendor BL32 loads user TAs from REE FS through `tee-supplicant`
RPC. `keyroot` implements the minimum supplicant loop:

- `OPTEE_RPC_FUNC_ALLOC` / `FREE` — allocate/free from the DMA arena
  and return the physical address and cookie.
- `OPTEE_RPC_CMD_LOAD_TA` — read
  `/lib/optee_armtz/<uuid>.ta`, copy it into the buffer, return its
  size. This is the only file the supplicant needs for v1.
- `OPTEE_RPC_CMD_GET_TIME` — return `nsec()` if the TA asks.
- Unknown RPCs fail the call with a clear error; they are not ignored.

The kernel gate is stateless. A `CALL_WITH_ARG` that returns
`OPTEE_SMC_RETURN_RPC_*` comes back to `keyroot`, which services it and
resumes with `OPTEE_SMC_CALL_RETURN_FROM_RPC`. This keeps the secure
world's state in the secure world and the loop in userspace.

### 6.4 TA commands

Proposed TA UUID: `6b1d7c2e-5a41-4f0b-9c7e-2d1a8e6f3b90` (to be
generated once and frozen; changing it changes every derived key).

| cmd | name | params |
|---|---|---|
| 0 | INFO | output: version, flags, max sizes |
| 1 | SEAL | input: label, plaintext; output: blob |
| 2 | UNSEAL | input: blob; output: label, plaintext |
| 3 | DERIVE | input: label, length; output: key |

Key derivation:

```
extra = "keyroot/v1/" || label
open session to PTA_SYSTEM_UUID
invoke PTA_SYSTEM_DERIVE_TA_UNIQUE_KEY
    params[0].memref = extra
    params[1].memref = 32-byte key
```

`PTA_SYSTEM_DERIVE_TA_UNIQUE_KEY` is present in OP-TEE 3.13 and mixes
the TA UUID, the device HUK, and the extra data. The TA never sees the
HUK itself. For AES-GCM, the 32-byte output is the key, the blob header
and label are the AAD, and the nonce is `TEE_GenerateRandom(12)`.
Plaintext, key, and intermediate buffers are zeroized with
`TEE_MemFill` before release. Authentication failure returns
`TEE_ERROR_SECURITY`; malformed input returns `TEE_ERROR_BAD_PARAMETERS`.

### 6.5 Deployment

- Build the TA against OP-TEE 3.13.0 (the vendor core's ABI) with the
  standard TA dev kit. The dev kit is platform-independent; a
  `vexpress-qemu_armv8a` or `rockchip-rk3399` build is sufficient to
  produce headers and `libutee`.
- Sign as a plain `SHDR_TA` with the Rockchip OEM key:
  `ta/sign_type0.py --key oem_privkey.pem --in <ta>.stripped.elf
  --out <uuid>.ta`. Do **not** use `sign_encrypt.py`'s bootstrap image;
  it requires the REE-FS version DB.
- Install as `/lib/optee_armtz/6b1d7c2e-....ta` on the ARM root
  filesystem (read-only or hostowner-owned). The same file is served by
  the FS supplicant; `keyroot` can keep it in `/boot` during bring-up.
- `keyroot -t optee -m /mnt/keyroot` starts after `/lib` is mounted and
  before `factotum`.

**The version database was the original blocker.** `sign_encrypt.py`
emits a `SHDR_BOOTSTRAP_TA` image, and `ree_fs_ta_read()` calls
`check_update_version()`, which stores `ta_ver.db` through
`TEE_STORAGE_PRIVATE`. Our minimal FS supplicant does not implement the
REE-FS secure-storage format, so that path fails. The type-0 signer
skips `check_update_version()` and is the supported deployment path
for v1. If a future TA needs bootstrap features (dependencies or
anti-rollback), the FS supplicant must implement the REE FS format or
use RPMB; that is a separate piece of work.

If the vendor BL32 ever changes its embedded TA key, the TA must be
re-signed or the gate must refuse to run; `status` should report the
BL32 revision and the TA UUID.

### 6.6 Limits

- `root=huk-lot` on this board; see §2.
- No RPMB use in v1. RPMB would add anti-rollback and persistence, but
  the eMMC RPMB key is not provisioned and provisioning is out of scope.
- No secure storage in the TA; blobs are returned to the caller. This
  is deliberate: `secstore` is the backup, and the TA has no filesystem
  dependency.
- The TA is stateless. Concurrent invocations are serialized by the
  single `keyroot` mutex; the TA itself needs no locking beyond OP-TEE's
  session model.

### 6.7 OEM OTP key path

The vendor BL32 exposes two relevant pseudo-TAs. Their descriptors are
in the BL32 image; the command IDs below are from static analysis and
must be confirmed by the probe before any write.

`rk_crypto_service.ta` — UUID
`0cacdb5d-466c-4fea-8f83-5216543d1697`:

| cmd | handler | meaning |
|---|---|---|
| 1 | `rk_oem_otp_key_cipher` | encrypt/decrypt with an OEM OTP key slot |
| 2 | `rk_oem_otp_key_phys_cipher` | same, physical-address buffers |
| 3 | `rk_user_ta_cipher` | user-TA-only; not callable from NS |
| 4–8 | key ladder / user-TA cipher | stubbed or user-TA-only |
| 9–11 | AEAD / key-ladder KDF | stubbed ("not supported") |

`rk_os_service.ta` — UUID
`527f12de-3f8e-434f-8f40-03078e86ae4b`:

| cmd | handler string | meaning |
|---|---|---|
| `0x78` | `rk_crash_optee` | debug; never call |
| `0x82` | `rk_read_oem_otp` | read OEM/OTP data |
| `0x8c` | `rk_write_oem_otp` | write OEM/OTP data |
| `0x96` | `rk_get_trng` | hardware random |
| `0xa0` | `rk_write_oem_otp` | write variant |
| `0xbe` | `rk_write_oem_otp` / `rk_set_otp_flags` | write/flags |

The exact parameter types for `0x8c`/`0xa0`/`0xbe` and for
`rk_oem_otp_key_cipher` are not documented. The disassembly shows that
command 1 expects `TEE_PARAM_TYPES(VALUE_INPUT, MEMREF_INPUT,
MEMREF_INOUT, NONE)` (`param_types == 0x751`), with `value.a` as the key
id and a 104-byte cipher-info struct in `param[1]` (IV length at offset
76, IV at offset 80). The probe calls it with zeroed buffers first;
because the key slots are blank, the cipher path should fail at the key
read, which proves reachability without writing anything. Only after the
ABI and the slot semantics are known does the provisioning tool get a
write path.

**Probe result (2026-10-30):** `rk_crypto_service` is a loadable user TA,
not a PTA. `OPEN_SESSION` with its UUID makes the core request
`/lib/optee_armtz/0cacdb5d-....ta` through the FS supplicant; the file is
absent and the call returns `TEE_ERROR_ITEM_NOT_FOUND`. The same applies
to `rk_os_service`. Both are subject to the default-key signature guard.
The only built-in service that is reachable without a TA is
`uboot_storedata_otp` (`2d26d8a8-...`), which offers OTP read and
key-is-written but no cipher.

Provisioning procedure (planned; not implemented, not executed):

1. **Inventory.** Read the four OEM key slots with the existing
   read-only `is_written` query. All four are blank today. Confirm the
   slot semantics from the vendor map; do not assume slot 0 is free for
   general use.
2. **Plan.** `keyroot otp plan --slot N` generates 32 random bytes in
   memory, prints the slot, a SHA-256 commitment, and the BL32 revision.
   It does not print or persist the key. It refuses if the slot is
   already written.
3. **Confirm.** The burn requires `--commit` plus a typed confirmation
   naming the slot and the board (the OTP lot code). This is a separate
   command from any normal `keyroot` operation.
4. **Burn.** Write the key through the vendor PTA (or the matching
   U-Boot command), then read back `is_written`. A failure after the
   write is not retried: the slot is one-way.
5. **Verify.** Reboot, wrap and unwrap a known test vector through
   `rk_oem_otp_key_cipher`, and confirm the wrapped master survives a
   power cycle. Record the slot, date, BL32 revision, and commitment in
   `/var/lib/keyroot/otp.json`; never record the key.
6. **Use.** `keyroot` generates a random master key, wraps it with the
   OEM key, and stores the wrapped blob as `keyroot/master`. All
   `seal`/`unseal`/`derive` operations use the master; the OEM key is
   touched only at startup.

Until step 4 is explicitly approved, the repository must not contain a
code path that can write OTP. The probe and the read-only commands are
safe; the write command stays out of the build.

## 7. TPM backend

`keyroot -t tpm -T /mnt/tpm` uses the existing `tpmfs` interface:

- On first use, generate a 32-byte master key and seal it with
  `tpmfs` (`/mnt/tpm/seal`), storing the blob as `keyroot/master`.
- `derive(label)` = HMAC-SHA256(master, `"keyroot/v1/" || label`).
- `seal(label, plain)` = TPM2 seal of `label '\0' plain` (so the label
  is bound inside the TPM blob), wrapped in the `KROOT1` header with
  `alg=TPM2`.
- `unseal(blob)` = TPM2 unseal, split and verify the label.

The TPM backend inherits `tpmfs`'s current properties: the deterministic
owner-hierarchy SRK, password sessions, no PCR policy, no persistent
handles. That is adequate for a VM/x86 v1 and can be strengthened later
without changing the `keyroot` interface. A TPM clear loses the master
key and therefore every blob; `secstore` remains the recovery path.
TPM2 sealed blobs add a small overhead over the plaintext, so the TPM
backend caps plaintext a little below `KrootMaxplain` to keep the framed
blob within `KrootMaxblob`; `KrootMaxplain` is the AES-GCM limit.

## 8. factotum and secstore integration

`factotum` already has the right hook: `getnvramkey()` in
`sys/src/cmd/auth/factotum/util.c` reads a `Nvrsafe` (machine key +
`secstore` password) from NVRAM, and `secstorefetch()` decrypts the
remote `factotum` key file with that password. On ARM there is no
NVRAM, so this path is dead today.

Proposed changes:

1. Add `libkeyroot` (`sys/src/libkeyroot` or a small addition to
   `libauth`): `keyroot_seal`, `keyroot_unseal`, `keyroot_derive`,
   `keyroot_status`, speaking 9P to `/mnt/keyroot`.
2. In `getnvramkey()`, if `/mnt/keyroot/status` is readable, try
   `keyroot_unseal("factotum/nvram", &safe, sizeof safe)` before
   `readnvram()`. Validate `nvcsum` exactly as the NVRAM path does.
   Fall back to `readnvram()` on any error, so x86 and existing setups
   are unchanged.
3. Add `factotum -K <file>`: after a successful `secstorefetch()` or a
   key-list change, read `/mnt/factotum/ctl` and seal it as
   `factotum/keys` into `<file>` (e.g. `/var/lib/factotum.sealed`).
   This is the offline cache used when the network or `secstore` is
   unavailable.
4. Boot order: `keyroot` → `factotum -S -K …` → `secstore` fetch if
   the network is up. If `keyroot` is absent, factotum behaves exactly
   as it does today.

`secstore` itself does not change. The sealed blob is an opaque file;
it can be copied to `secstore` (`secstore -p factotum.sealed`) or kept
on the local eMMC. The password-encrypted `factotum` file remains the
portable source of truth.

## 9. Provisioning, backup, migration

- First boot: `keyroot` creates or loads the root state. For OP-TEE,
  there is nothing to create; the HUK is the root. For TPM, the master
  key is created and sealed on first `seal`.
- Enrolment: `factotum -K` writes the first sealed blob after the user
  has authenticated once. The blob is bound to the label and the
  backend; copying it to another board fails.
- Backup: `secstore -p` the blob and the password-encrypted `factotum`
  file. Recovery on a new board uses the passphrase path, then
  re-enrols a new blob.
- Migration: a blob from `backend=optee` is rejected by `backend=tpm`
  with `wrong backend`; the user re-enrols. Do not silently fall back.
- Destruction: `keyroot ctl flush` zeroizes cached keys; deleting the
  blob is sufficient to make the secret unrecoverable from that copy.
  There is no secure erase of the root itself (it is in OTP/TPM).

## 10. Tests

- `keyroottest` (mock): seal/unseal round trip, wrong label, truncated
  blob, bit flip in ciphertext/tag, oversized plaintext, bad label,
  derive determinism, derive length bounds, concurrent seal/unseal.
- `keyroottest -t tpm` against `tpmfs -t mock` in QEMU: same vector,
  plus TPM clear → unseal fails, re-enrol works.
- `keyroottest -t optee` on the board: probe `status`, load the TA,
  seal/unseal/derive, verify a blob sealed on board A fails on board B
  (expected on different lot codes; record the actual result).
- `factotum` integration: with `keyroot` absent, `-S` behaves as today;
  with `keyroot` present and a valid blob, no passphrase prompt; with a
  corrupted blob, fall back to the prompt.
- Negative: a non-hostowner user cannot open `seal`/`unseal`/`derive`;
  a process other than `keyroot` cannot use `#P/tee/smc`.

## 11. Implementation phases

0. **Interface + mock** — this document, `libkeyroot`, `keyroot -t
   mock`, `keyroottest`. Runs in QEMU and on 9front today.
1. **TPM** — `keyroot -t tpm` over `tpmfs`; test with `tpmfs -t mock`,
   then CRB on the x86/VMX target.
2. **OP-TEE transport + TA load** — **done.** `#P/tee/smc`, the NSEC
   SHM segment (`0x09200000`, 2 MiB, `#P/tee/shmbase`/`shmsize`),
   session/invoke, the FS supplicant, and TA loading all work. The
   vendor OEM key from `rk_tee_user` is accepted; a type-0 signed TA
   opens session 1 and PING/INFO/DERIVE succeed. The default key is
   rejected by the vendor guard.
3. **TA seal/unseal + keyroot server** — add SEAL/UNSEAL to the TA
   (AES-256-GCM, per-label key from
   `PTA_SYSTEM_DERIVE_TA_UNIQUE_KEY`), build `keyroot -t optee` with
   the 9P interface, and pass `keyroottest`.
4. **factotum/secstore** — `getnvramkey()` hook and `-K`.
5. **OEM OTP cipher (future)** — obtain the vendor
   `rk_crypto_service.ta`; guarded one-time OTP key burn; wrap the
   master key with `rk_oem_otp_key_cipher`. Only after explicit
   approval; the write path is not built until then.
6. **Hardening** — gate exclusivity, TA version pinning, zeroization
   audit, optional PCR/passphrase policy.

## 12. Open decisions

1. **ARM root of trust — decided: one-time OEM OTP key.** The remaining
   questions are which slot (0–3), which write command, and when the
   irreversible burn is approved. The probe in phase 2 answers the first
   two; the burn is a separate, explicit step.
2. **Gate ownership.** Should `#P/tee/smc` be hostowner-only, or should
   it be tied to a single `keyroot` process (a pid check or a one-shot
   exclusive open)? Hostowner-only is simpler; a pid check is stronger
   against other hostowner processes but is not a real boundary.
3. **TPM policy.** v1 uses the deterministic SRK with no PCRs, matching
   `tpmfs`. PCR 0/7 binding is the next step and needs a boot
   measurement story.
4. **Blob backup.** Keep the sealed blob only on local eMMC, or also in
   `secstore`? The design supports both; the default should be local
   only, with an explicit `secstore -p` for backup.
5. **factotum cache granularity.** Seal the `Nvrsafe` (small, matches
   the existing code) or the whole `ctl` key list (works without a
   machine key, but larger and more sensitive)? Start with `Nvrsafe`
   and add the key-list cache if headless operation needs it.
6. **TA signing — resolved.** Use the Rockchip OEM key from
   `rk_tee_user/v2/export-ta_arm64/keys/oem_privkey.pem` and type-0
   `SHDR_TA` signing (`ta/sign_type0.py`). The default key is rejected;
   the OEM key is accepted. The OEM key is public, so TA authentication
   is still not a security boundary. Remaining future work: obtain the
   vendor `rk_crypto_service.ta` for the OEM OTP cipher, or replace the
   HUK root with a discrete TPM/ATECC or an upstream OP-TEE port.
