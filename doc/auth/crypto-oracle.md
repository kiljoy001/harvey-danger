# Cryptographic oracle design (Approach B)

Status: design, following the working secure-mode bring-up. This
supersedes the seal/unseal/derive interface in `keyroot.md` for
authentication private keys. The seal/unseal path remains only for
opaque secrets such as the `secstore` password.

## 1. The idea

`factotum` already keeps secrets away from client programs: clients
speak an authentication protocol over `/mnt/factotum/rpc`. This design
extends that boundary one step further:

- `factotum` holds public parameters: public keys, key labels, protocol
  state, and certificates.
- The trusted application holds the private keys and performs the
  cryptographic math inside secure mode.
- `factotum` sends a challenge, a digest, or a ciphertext; the trusted
  application returns a signature, a ticket, an authenticator, a MAC,
  or a decrypted session key.
- The long-term private key never exists in normal-world memory at any
  point.

The same model covers two very different cases:

- **Shared-secret authentication** (p9sk1 / dp9ik): the trusted
  application holds the shared secret, computes the ticket key and the
  authenticator, and returns only the completed protocol messages.
- **Asymmetric authentication** (SSH, TLS, Yggdrasil): the trusted
  application generates and holds the private key, exports only the
  public key, and signs or decrypts on request.

## 2. Architecture

```
client program
    |
    | auth protocol
    v
factotum  (/mnt/factotum/rpc)
    |
    | oracle RPC (9P)
    v
keyroot  (/mnt/keyroot)
    |
    | OP-TEE invoke
    v
trusted application  (secure mode)
    |
    | sealed key blob
    v
normal-world storage
```

- `keyroot` is a thin 9P server. It stores sealed blobs and forwards
  operations to the trusted application. It never sees a private key.
- The trusted application unseals a private key only inside secure
  memory, performs the operation, and wipes it.
- The sealed blob is opaque. It can live in a file, on the eMMC, or in
  `secstore`. A stolen blob is useless without the chip key (and the
  user passphrase, if the policy requires one).

## 3. Key lifecycle

- **Generate.** The trusted application generates the key pair
  internally. It returns the public key and a sealed private-key blob.
  The private key is never exported in plaintext.
- **Import.** For provisioned shared secrets (p9sk1), the secret is
  passed into the trusted application once and sealed. It is never
  returned.
- **Export public.** Anyone may read the public key; that is how
  `factotum` and `authorized_keys` learn it.
- **Use.** Sign, decrypt, MAC, ECDH, or a protocol-specific operation.
  The sealed blob is unsealed inside secure memory for the operation and
  wiped afterwards.
- **Delete.** Remove the blob. The key is gone unless a copy exists.
- **Deterministic keys.** For Ed25519 and X25519, the private key can be
  derived from the chip key and a label, so no blob is needed. The same
  label always yields the same key; the private key never exists outside
  secure memory.

## 4. Key blob

```
offset  size  field
0       6     magic "KROOT2"
6       1     version
7       1     algorithm (RSA, ECDSA, Ed25519, DES, AES, ...)
8       1     usage (SSH, TLS, p9sk1, Yggdrasil, ...)
9       1     policy (passphrase required, lock after use, ...)
10      2     reserved
12      4     label length
16      4     public length
20      4     private length (ciphertext)
24      12    nonce
36      16    tag
52      ...   label
        ...     public key
        ...     sealed private key
```

The private key is encrypted with AES-256-GCM under a key derived from:

```
wrapping key = KDF(chip key, "keyroot/oracle/v1/" || label || usage
                   [|| passphrase])
```

The label and usage are authenticated. A blob generated for SSH signing
cannot be used as a TLS decryption key, even if the ciphertext is
otherwise valid.

If the policy requires a passphrase, the passphrase is supplied to the
trusted application at unlock time and is never stored. Without it, the
blob cannot be unsealed. This is what makes a stolen, powered-off board
unable to use the key. A running board with an unlocked oracle can still
use the key; a lock-after-use or per-operation policy is the next step.

## 5. The 9P interface

A single request/response file keeps the interface small and extensible:

```
/mnt/keyroot/status          read: backend, lock state, capabilities
/mnt/keyroot/ctl             write: lock, unlock, reload
/mnt/keyroot/rpc             write a request, read the response
/mnt/keyroot/keys/<label>/public   read the public key
```

The request and response are length-prefixed binary structures:

```
struct OracleReq {
    u32 version;
    u32 op;          /* KEYGEN, IMPORT, PUBLIC, SIGN, ... */
    u32 algorithm;
    u32 flags;       /* passphrase present, lock after use, ... */
    u32 label_len;
    u32 data_len;
    u8  label[];
    u8  data[];
};

struct OracleResp {
    u32 version;
    u32 result;      /* 0 or a TEE error */
    u32 public_len;
    u32 data_len;
    u8  public[];
    u8  data[];
};
```

Operations:

| op | name | input | output |
|---|---|---|---|
| 1 | KEYGEN | algorithm, label, policy | public key, sealed blob |
| 2 | IMPORT | algorithm, label, secret | public key (if any), sealed blob |
| 3 | PUBLIC | label or blob | public key |
| 4 | SIGN | blob, mechanism, message/digest | signature |
| 5 | DECRYPT | blob, mechanism, ciphertext | plaintext or session key |
| 6 | ECDH | blob, peer public key | shared secret |
| 7 | MAC | blob, mechanism, data | MAC |
| 8 | P9SK1_TICKET | label, server challenge, ticket request | ticket |
| 9 | P9SK1_AUTH | label, challenge | authenticator |
| 10 | DELETE | label | none |
| 11 | LOCK / UNLOCK | passphrase (unlock only) | none |

The p9sk1/dp9ik operations are deliberately protocol-shaped, not a raw
"encrypt with the shared key" operation. Returning a raw ciphertext
would let a caller use the shared secret as an oracle for other
purposes. The trusted application checks the protocol fields and returns
only the ticket or authenticator the protocol defines.

## 6. Trusted-application commands

The 9P server maps each operation to one invoke command:

| command | purpose |
|---|---|
| 0 | INFO: version, algorithms, lock state |
| 1 | KEYGEN |
| 2 | IMPORT |
| 3 | PUBLIC |
| 4 | SIGN |
| 5 | DECRYPT |
| 6 | ECDH |
| 7 | MAC |
| 8 | P9SK1_TICKET |
| 9 | P9SK1_AUTH |
| 10 | DELETE |
| 11 | LOCK |
| 12 | UNLOCK |

The trusted application uses the vendor Rockchip crypto helpers for
RSA, ECDSA, ECDH, AES, DES/3DES and HMAC. Ed25519/X25519 for Yggdrasil
can use the `monocypher` source already in the tree if the vendor
helpers do not provide it.

All private-key buffers, derived wrapping keys and intermediate values
are wiped with `TEE_MemFill` before release. The trusted application
checks labels, usage, algorithm and policy on every operation.

## 7. factotum integration

The key list gains a new kind of entry:

```
key proto=ssh role=client user=... !oracle=ssh/host-key
key proto=p9sk1 user=... dom=... !oracle=p9sk1/server-key
```

- `!oracle=<label>` replaces `!rsa=...`, `!hex=...` and other private
  material in the key list.
- Public parameters (public key, public exponent, certificate) may stay
  in the key list or be fetched from `/mnt/keyroot/keys/<label>/public`.
- A new `libkeyroot` client carries the request/response protocol.

Protocol changes:

- `rsa.c` (SSH): compute the session hash, send it with `SIGN`, receive
  the signature. The private key is never loaded.
- `p9sk1.c` / `p9cr.c`: on a ticket request, send the challenge and
  ticket fields with `P9SK1_TICKET`; on an authenticator, send the
  challenge with `P9SK1_AUTH`; receive the completed bytes.
- TLS: the TLS stack in `libsec` needs a signing/decryption callback.
  This is a larger change; start with SSH and p9sk1, add TLS after.
- `factotum` still confirms keys and enforces its own policy. The oracle
  is an additional boundary, not a replacement for factotum's controls.

## 8. secstore

The `secstore` password is not an authentication private key; it
protects the remote key file. Two options:

1. **Seal/unseal the password** with the chip key (the existing
   `keyroot` seal path). The password is in normal-world memory only
   during the PAK exchange. This is simple and keeps the current
   `factotum -S` flow.
2. **PAK inside the trusted application.** The trusted application
   performs the password-derived exponentiation and returns the PAK
   messages. This keeps the password out of normal memory, but requires
   implementing the PAK client in the trusted application and a way for
   it to exchange messages. It is a larger project.

The factotum key file fetched from `secstore` can contain only public
parameters and labels once the private keys live in the oracle. That
makes the remote file much less sensitive.

## 9. Yggdrasil

The Yggdrasil node key is a long-lived Ed25519 key. Two layers:

- The trusted application derives or seals the Ed25519 private key and
  exposes only the public key.
- A small proxy exposes `SIGN` and `ECDH` to the Yggdrasil daemon. The
  daemon never sees the private key; it signs announcements and
  handshakes through the proxy.

If the key must be unusable on a stolen, powered-off board, the trusted
application requires a passphrase to unlock the Yggdrasil key. A running
board with an unlocked proxy can still sign; a lock-after-use policy
limits that. A stolen board without the passphrase cannot sign.

The public key/address is exported once and can be written into the
Yggdrasil configuration. The private key never enters normal memory.

## 10. Security notes

- **The oracle is an oracle.** A malicious process with
  administrator-equivalent rights on the running board can ask the
  trusted application to sign or decrypt. It cannot copy the private
  key, but it can impersonate the key while the oracle is unlocked.
  Lock-after-use, a passphrase per operation, or a separate security
  chip are the mitigations.
- **Unattended boot and stolen-board protection conflict.** If the board
  can unlock the oracle by itself, a thief can too. Requiring a
  passphrase at boot protects a powered-off stolen board; it means the
  board cannot boot unattended.
- **The signing key is public.** Rockchip's OEM key is in a public
  software kit. A physical attacker who can reflash the secure firmware
  can load a malicious trusted application. Secure boot with our own key
  is needed for a strong firmware boundary.
- **The chip key is lot-bound.** The hardware unique key on this board
  comes from a manufacturing lot code. A board from the same lot can
  derive the same wrapping key. A passphrase or a separate security chip
  is needed for per-board protection.
- **Side channels.** The trusted application's crypto must be
  constant-time. Physical power/timing attacks are out of scope unless
  explicitly added.
- **No raw private-key export.** There is no operation that returns a
  private key. The public key, signature, ticket, authenticator, MAC and
  session key are the only outputs.

## 11. Implementation phases

1. **Oracle core in the trusted application.** KEYGEN, PUBLIC, SIGN
   (RSA), sealed blob, lock/unlock. Test with a host client and the
   existing probe.
2. **keyroot 9P server.** `/mnt/keyroot/rpc`, key storage, libkeyroot.
3. **factotum SSH/RSA.** `!oracle=` key entries and `rsa.c` integration.
4. **factotum p9sk1/dp9ik.** Ticket and authenticator operations and
   `p9sk1.c` integration.
5. **secstore password policy.** Seal/unseal first; PAK-in-TA later.
6. **Ed25519/Yggdrasil proxy.** Monocypher in the trusted application,
   signing proxy, public key export.
7. **TPM backend.** Map the same oracle operations to TPM2_Sign,
   TPM2_RSA_Decrypt, TPM2_HMAC and TPM2_ECDH for x86/VMX.

## 12. Post-quantum options (ML-KEM / Kyber, ML-DSA / Dilithium)

ML-KEM (formerly Kyber) is a **key encapsulation mechanism**. It lets
two parties agree on a shared session key; it is not a signature scheme
and it is not a local storage cipher. The distinction matters:

- **Local sealed storage.** The private-key blob is encrypted with
  AES-256-GCM under a key derived from the board's chip key. AES-256 is
  already adequate against a future quantum attacker for
  confidentiality (Grover's search leaves roughly 128-bit security).
  Adding ML-KEM to the local seal does not improve the threat model: the
  private key would be on the same board, so the encapsulation and
  decapsulation both happen where the attacker already is. Keep AES-256
  for the blob.

- **Network session establishment.** ML-KEM is useful here, in a hybrid
  with X25519 (the standard "X25519 + ML-KEM" key schedule). The trusted
  application holds the ML-KEM private key and performs decapsulation;
  it returns only the session key or the completed handshake. Both ends
  must support the hybrid; Plan 9's p9sk1/dp9ik and current SSH/TLS
  peers do not, so this is a protocol extension, not a drop-in.

- **Long-term identity and signatures.** ML-KEM is the wrong primitive.
  Use ML-DSA (formerly Dilithium) or a hybrid Ed25519 + ML-DSA. The
  trusted application holds the ML-DSA private key and signs; only the
  public key and signature are exported. Yggdrasil currently uses
  Ed25519, so adding ML-DSA means a Yggdrasil protocol extension or a
  fork.

- **Library support.** None of the vendor Rockchip helpers, OP-TEE 3.13,
  or the `monocypher` code in this tree currently provides ML-KEM or
  ML-DSA. Adding one means vendoring a post-quantum library such as
  PQClean or liboqs into the trusted application. The sizes grow:
  ML-KEM-768 uses a 1,184-byte public key, 1,088-byte ciphertext and
  2,400-byte private key; ML-DSA-65 uses a 1,952-byte public key and a
  3,309-byte signature. The trusted application's heap and the key blob
  format must grow; the 2 MiB shared-memory region is sufficient.

- **Interface.** The oracle request already carries an algorithm field.
  Add `KEM_ENCAPS` (public operation), `KEM_DECAPS` (private operation)
  and ML-DSA mechanisms to `SIGN`. The private key stays in the trusted
  application; the normal world receives the ciphertext/shared secret or
  the signature, never the private key.

In short: ML-KEM is a good **session-establishment** addition and a poor
**local-sealing** replacement; identity needs ML-DSA, not ML-KEM. The
first oracle version should use AES-256 for blobs and classical
signatures, with the algorithm field left open for a post-quantum phase.

Which primitive fits which job:

| Job | Right primitive | Notes |
|---|---|---|
| Local sealed key blob | AES-256-GCM | Key derived from the chip key and label; already quantum-adequate for confidentiality. |
| Session key agreement | X25519 + ML-KEM | Hybrid; both peers must support it. |
| Long-term identity / signatures | Ed25519 + ML-DSA (hybrid), or ML-DSA alone | ML-DSA (Dilithium) is the signature scheme; it is the better fit for the permanent Yggdrasil key. |
| Shared-secret authentication (p9sk1/dp9ik) | DES/3DES/AES with the shared secret in the trusted application | The secret never leaves secure memory. |

For Yggdrasil specifically, a **hybrid Ed25519 + ML-DSA** identity is the
practical post-quantum path: the current Ed25519 public key keeps the
existing address and peer compatibility, while the ML-DSA signature adds
post-quantum authentication. A pure ML-DSA identity changes the address
format and requires a larger Yggdrasil change. ML-DSA signatures are
larger (for example, ML-DSA-44 is about 2.4 KiB and ML-DSA-65 about
3.3 KiB), so packet sizes and the trusted application's buffers must
grow.

## 12b. Layered plan (decided)

The intended split is:

- **ML-DSA (Dilithium) for identity.** The trusted application holds an
  ML-DSA private key, sealed under the chip key. The operating system
  sends a random challenge; the trusted application signs it; the
  operating system verifies the signature with the stored public key.
  This binds the oracle's identity to the chip key. It is a software
  identity, not a hardware measurement: because Rockchip's signing key
  is public, an attacker who can replace the trusted application could
  load a same-identifier application and ask the system service for the
  same derived key. A passphrase or a separate security chip is still
  required for stolen-board protection.
- **ML-KEM (Kyber) for session establishment.** `secstore` and
  `factotum` use a hybrid handshake: the existing dp9ik exchange plus an
  ML-KEM encapsulation. The trusted application holds the ML-KEM private
  key and the dp9ik shared secret; it computes the hybrid session key and
  returns only that key. Legacy dp9ik remains as the fallback when the
  peer does not offer ML-KEM.
- **Yggdrasil identity.** A hybrid Ed25519 + ML-DSA identity keeps the
  current address and compatibility while adding a post-quantum
  signature. ML-KEM can be added to Yggdrasil sessions later.
- **Local blobs.** AES-256-GCM under the chip-derived key; no change.

The hybrid session key is:

```
session key = KDF(ML-KEM shared secret || dp9ik shared secret
                  || protocol transcript)
```

The trusted application performs the KDF and returns the session key,
not the long-term shared secret. The normal world sends and receives the
encrypted protocol messages.

**Downgrade protection is mandatory.** If a peer can fall back to legacy
dp9ik, an active attacker can strip the ML-KEM offer and force the weak
path. The trusted application signs the handshake transcript (the
algorithm list, the ML-KEM ciphertext and the dp9ik messages) with the
ML-DSA or Ed25519 identity key, and the peer verifies that signature.
Legacy fallback is allowed only when the peer does not offer ML-KEM and
the authenticated transcript says so.

Implementation order:

1. **Classical oracle core.** Key import, public export, sign, MAC,
   p9sk1/dp9ik ticket and authenticator; the keyroot request service.
2. **ML-DSA identity.** Add ML-DSA key generation/signing to the trusted
   application and the operating-system challenge/response.
3. **ML-KEM hybrid.** Add encapsulation/decapsulation and the hybrid key
   schedule to `secstore`/`factotum`, with legacy fallback.
4. **Yggdrasil hybrid identity.** Ed25519 + ML-DSA signing proxy.
5. **TPM backend.** The same operations on the x86/VMX target.

## 12c. Measured ML-DSA milestone (2026-10-30)

The first oracle step works on the Orange Pi:

```
session 1 opened
mldsa keygen: pk=1312 blob=3938
mldsa sign: sig=2420
mldsa verify: ret 0x0
mldsa user verify: 0
mldsa file verify: 0
mldsa selftest: ret 0x0
```

What this proves:

- The ML-DSA-44 key pair is generated inside the trusted application.
- The private key is sealed with AES-256-GCM under a wrapping key
  derived from the chip key and the label
  (`PTA_SYSTEM_DERIVE_TA_UNIQUE_KEY`).
- The sealed blob (`KROOT2`, 3,938 bytes for a 14-byte label) is the only
  private-key representation that leaves secure memory. It is opaque.
- The public key (1,312 bytes) is exported; the signature (2,420 bytes)
  is computed inside the trusted application.
- The TA image is 109,228 bytes, type-0 signed with the Rockchip OEM
  key; stack 64 KiB, heap 256 KiB.
- The signature verifies **independently in normal-world Harvey** with
  the vendored Dilithium verifier (`mldsa user verify: 0`), not only
  inside the trusted application.
- The sealed blob, public key, challenge and signature are stored under
  `/mnt/keyroot/` (`identity.blob`, `identity.pub`, `challenge`,
  `signature`) and the signature verifies from those files
  (`mldsa file verify: 0`). This is the first file-store form; the
  9P `keyroot` service is the production wrapper.

Two implementation lessons:

1. The core requires the AES-GCM **decryption tag** to be in
   trusted-application-owned memory. A tag pointer into shared memory is
   rejected with `TEE_ERROR_ACCESS_DENIED`. Copy the tag into a local
   buffer before `TEE_AEDecryptFinal`.
2. The OP-TEE development kit's `stdint.h` defines a macro named `L`,
   which collides with the Dilithium parameter `L`. The vendored PQClean
   parameters are prefixed (`MLDSA_L`, `MLDSA_K`, ...).

The identity cycle is complete at the test level. Still to do:

- The production `keyroot` 9P service. The current `/mnt/keyroot` files
  are the first store form; the 9P request interface is the wrapper.
- The passphrase policy for stolen-board protection. The current blob is
  sealed under the chip key only, which is lot-bound, not board-bound.
- The ticket and authenticator operations, which are the next phase
  (done for p9sk1, see below).

## 12d. Measured p9sk1 ticket and authenticator milestone (2026-10-30)

This tree implements `p9sk1`/`p9sk2`, not 9front's `dp9ik`. `factotum`
negotiates `p9sk1` and `p9sk2` only, and the wire format is the Plan 9
fourth-edition one: a 72-byte `Ticket`, a 13-byte `Authenticator`, and
the traditional 7-byte-chained DES `encrypt`/`decrypt`. The oracle was
therefore built for that protocol. `dp9ik` would be a separate protocol
module if it is ever wanted.

Measured on the Orange Pi:

```
p9 import: host blob 66 auth blob 66
p9 tickets: 144 bytes
p9 authenticator: 13 bytes
p9 server: ticket and authenticator verified
p9 ta verify server reply: ret 0x0
p9 selftest: ret 0x0
```

What this proves:

- The two DES keys (host key and auth-server key) are imported into the
  trusted application and sealed as `KROOT2` blobs, 66 bytes each for
  a 7-byte label. The keys never leave secure mode in the real path.
- The trusted application builds both tickets from a `Ticketreq`:
  the client ticket (`AuthTc`, encrypted under the host key) and the
  server ticket (`AuthTs`, encrypted under the auth-server key). The
  session key is generated inside the trusted application and is never
  returned.
- The trusted application builds the client authenticator (`AuthAc`,
  13 bytes) from the client ticket and the server challenge.
- Normal-world code can simulate the server, verify the ticket and
  authenticator, and produce the server reply (`AuthAs`). The trusted
  application verifies that reply with `P9SK1_VERIFY`.
- The self-test runs ticket, authenticator, server reply and reply
  verification end to end inside secure mode.

Implemented invoke commands in the bring-up TA:

| command | name |
|---|---|
| 0 | PING |
| 1 | INFO |
| 2 | DERIVE |
| 3 | MLDSA_KEYGEN |
| 4 | MLDSA_PUBLIC |
| 5 | MLDSA_SIGN |
| 6 | MLDSA_VERIFY |
| 7 | MLDSA_SELFTEST |
| 8 | P9_IMPORT |
| 9 | P9_TICKET |
| 10 | P9_AUTH |
| 11 | P9_VERIFY |
| 12 | P9_SELFTEST |

Implementation notes:

- The DES code is vendored from `sys/src/libsec/port/des.c` and
  `crypt.c` into `sys/src/cmd/auth/keyroot/ta/des/`, because the trusted
  application does not link `libsec`. A host-side known-answer test
  matches OpenSSL `des-ecb` for the same key and block:
  key `0123456789abcd` (7-byte form), block `12345678`, ciphertext
  `05e450c909c7dcf4`.
- The blob format was generalized: the header now carries `alg`
  (`1` = ML-DSA-44, `2` = p9sk1 DES) and `usage`
  (`1` = identity, `2` = p9sk1 host key, `3` = p9sk1 auth-server key).
  A p9sk1 blob has no public part, so `pub len` is zero.
- The probe's fixed test keys are test data only. They are used to let
  normal-world code play the server and check the wire format. The
  production `keyroot` service never exports them.

Open security work in this area:

- **UID policy.** The current `P9_TICKET` blindly copies the requested
  `uid` into `cuid` and `suid`. A production oracle must reject a
  request for a user the calling process may not speak for, or the
  oracle becomes a ticket mint for any local root process.
- **Authenticator replay range.** The server-side checks in this tree
  also enforce the authenticator-id window. The oracle's client-side
  check only requires `id == 0`; the server side is not yet an oracle
  command.
- **Host/auth key separation.** The test uses two keys. A deployment
  must decide whether the same identity can be both host and auth
  server, and enforce that decision in the blob usage field.

## 12e. Measured dp9ik core milestone (2026-10-30)

This tree had only the fourth-edition `p9sk1`/`p9sk2` protocols. The
modern 9front `dp9ik` protocol is now ported at the library level.

Measured on the Orange Pi with `dp9iktest`:

```
dp9ik: password derivation ok
dp9ik: sha256 ok
dp9ik: hkdf ok
dp9ik: form1 ok
dp9ik: authenticator ok
dp9ik: authpak hash ok
dp9ik: authpak exchange ok
dp9ik: ticket ok
dp9ik authsrv api ok
```

What was added:

- `sys/include/dp9ik.h`: `Authkey`, `PAKpriv`, `Dp9ikTicket`,
  `Dp9ikAuthenticator`, and the dp9ik constants (`AESKEYLEN`,
  `NONCELEN`, `PAK*`, `AuthPAK`, `MAXTICKETLEN`, `MAXAUTHENTLEN`).
- `sys/src/libauthsrv/dp9ik/`: the AuthPAK exchange (SPAKE2 over
  Ed448), the form1 ChaCha20-Poly1305 wire protection, the dp9ik
  ticket/authenticator conversions, and the password derivation
  (`DES` fallback plus AES via PBKDF2-HMAC-SHA1, 9001 rounds).
- `sys/src/libsec/port/`: ChaCha20, ChaCha block function,
  ChaCha20-Poly1305 (`ccpoly`), Poly1305, HKDF, PBKDF2 and a
  timing-safe `tsmemcmp`.
- `sys/src/libmp/port/`: the modular operations and helpers required by
  the generated Ed448/SPAKE2 code (`mpmodadd`, `mpmodsub`, `mpmodmul`,
  `mpsel`, `mpnrand`, `mptober`, `mpfield`), plus the `MPnorm`,
  `MPtimesafe` and `MPfield` flags.
- `sys/src/cmd/auth/dp9iktest.c`: the reference vectors from the working
  plan9port dp9ik implementation, including the AuthPAK hash vector and
  a full client/server AuthPAK exchange.

The test vectors cover:

- Password-derived AES key for `testpassword`:
  `396361fccb1de42329619af100eddba3`.
- SHA-256 of `abc` and the RFC 5869 HKDF vector.
- form1 roundtrip and tamper rejection.
- Authenticator and ticket roundtrip in the form1 encoding.
- The 448-byte AuthPAK hash vector for user `testuser`.
- A client/server AuthPAK exchange producing the same 32-byte
  `pakkey` on both sides.

The `dp9ik` protocol is now wired into `factotum`:

- `sys/src/cmd/auth/factotum/dp9ik.c` implements the client and server
  phases, the relay of the server's AuthPAK public point, the
  auth-server exchange, ticket and authenticator handling, and the
  256-byte session secret via HKDF.
- `dp9ik` is in the protocol table, in `p9any` negotiation (after
  `p9sk1`, for the drawterm compatibility reason), and in the
  `factotum.json` source list.
- A `dp9ik` key can be added with `proto=dp9ik` and `!password` or
  `!hex`; `dp9ikaddkey` derives the `Authkey` and the AuthPAK hash and
  stores it in `Key->priv`.

Measured on the board after adding `factotum` to the ARM64 image:

```
factotum -n -s factotum
factotumkey
ctl:
key proto=dp9ik dom=test user=glenda !password?
```

That confirms the running OS accepts a dp9ik key, derives the
password key and AuthPAK hash, and lists the key with the secret
hidden. The actual auth-server handshake is not yet exercised because
this tree has no dp9ik auth server; the protocol state machine is
compiled and the crypto is covered by `dp9iktest`.

Still to do for dp9ik:

- Run a client/server handshake against a real dp9ik auth server and
  a dp9ik file server. The AS side is not in this tree.
- The oracle integration: the AuthPAK scalar and the ticket keys must
  move into the trusted application so normal world never holds the
  password-derived `Authkey` or the session secret.
- Investigate the `validname* called from ... with user pointer`
  kernel warning seen while serving the factotum mount. The file
  server works, but the warning should not be ignored.

## 12f. Measured dp9ik oracle milestone (2026-10-30)

The dp9ik cryptography now runs **inside the trusted application**.
Measured on the Orange Pi:

```
dp9ik import: blob 565 bytes
dp9ik blob: 4b524f4f54320103040000000000000a
dp9ik init: clientY 56 bytes
dp9ik clientY: 53a6b60ae65d18fb
dp9ik finish with zero AS reply: ret 0xffff0005
dp9ik selftest: ret 0x0
```

What this proves:

- The password-derived `Authkey` is sealed as a `KROOT2` blob with
  algorithm `3` (dp9ik) and usage `4` (dp9ik identity). The 565-byte
  blob is 52-byte header + 10-byte label + 503-byte `Authkey`.
- The trusted application unseals that blob and runs the client side
  of AuthPAK: it generates the Ed448/SPAKE2 ephemeral scalar and
  returns only the 56-byte public point.
- A malformed auth-server reply is rejected with
  `TEE_ERROR_BAD_FORMAT`, not a panic.
- The self-test runs AuthPAK client and server, compares the 32-byte
  shared key, and round-trips a form1 ticket and authenticator. This
  exercises libmp, Ed448/SPAKE2, ChaCha20-Poly1305, HMAC-SHA256 and
  HKDF entirely in secure mode.

What was added to the trusted application:

- `sys/src/cmd/auth/keyroot/ta/mp/`: a trusted-application build of
  the libmp big-integer code needed by Ed448/SPAKE2. The compatibility
  layer maps `mallocz` to the OP-TEE allocator and `genrandom` to
  `TEE_GenerateRandom`.
- `sys/src/cmd/auth/keyroot/ta/dp9ik/`: AuthPAK, form1, the ticket and
  authenticator conversions, ChaCha20, Poly1305, and a small
  SHA-256/HMAC-SHA256/HKDF layer over the OP-TEE digest API.
- Trusted-application commands:
  - 13 `DP9IK_IMPORT`
  - 14 `DP9IK_SELFTEST`
  - 15 `DP9IK_INIT`
  - 16 `DP9IK_FINISH`
  - 17 `DP9IK_REPLY`
- The trusted application grew to 160,980 bytes, with 128 KiB stack
  and 512 KiB heap. The probe's TA-size limit is now 512 KiB.

The oracle client flow:

1. `DP9IK_INIT`: unseal the `Authkey`, take the server's ticket
   request and public point, return the client's public point.
2. Normal world relays both points through the auth server and gets
   the two server points plus the ticket pair.
3. `DP9IK_FINISH`: derive the ticket key, decrypt the client ticket,
   and return the server ticket plus the client authenticator. The
   session key never leaves secure mode.
4. `DP9IK_REPLY`: verify the server authenticator and derive the
   256-byte session secret for the OS.

Still to do:

- The normal-world `keyroot` client and 9P service that call these
  commands, so `factotum`'s dp9ik protocol uses the oracle instead of
  its local `Authkey` path.
- A real auth-server handshake. The current hardware test uses the
  trusted application's self-test and a malformed reply check.
- The server-side oracle commands (the file-server role).

## 12g. keyroot 9P service (2026-10-30)

A first `keyroot` 9P service now exists and runs on the board:

- `keyroot -m /mnt/keyroot -t /boot/optee-keyroot.ta`
- Files: `ctl`, `dp9ik/blob`, `dp9ik/import`, `dp9ik/init`,
  `dp9ik/finish`, `dp9ik/reply`.
- The service loads the TA through the REE-FS supplicant, opens one
  TA session, and forwards the dp9ik operations.

Measured:

```
keyroot: import blob 565
keyroottest: write /mnt/keyroot/dp9ik/import r=76 n=76
keyroot ctl: keyroot dp9ik blob=565
keyroot blob: 565 bytes
keyroottest: write /mnt/keyroot/dp9ik/init r=197 n=197
keyroot init: clientY 56 bytes
keyroot: write tag 4 n 360 failed
keyroottest: write /mnt/keyroot/dp9ik/finish r=-1 n=360
keyroot finish rejected malformed AS reply
```

The 9P service now works end to end for the operations that do not
need a live auth server:

- `dp9ik/import` derives and seals the `Authkey` in the TA and stores
  the 565-byte blob.
- `ctl` reports `blob=565`.
- `dp9ik/blob` returns the sealed blob.
- `dp9ik/init` accepts the ticket request plus server point and
  returns the 56-byte client point.
- `dp9ik/finish` rejects a malformed auth-server reply cleanly instead
  of crashing.

Three 9P details were required:

1. Static files need a non-zero `Dir.length` in the tree. A zero
   length makes the client return EOF without asking the server.
2. A write handler must set `r->ofcall.count = r->ifcall.count` before
   `respond`. Otherwise the client sees a zero-byte write.
3. A read handler must call `respond(r, nil)` after `readstr` or
   `readbuf`. Those helpers fill the reply but do not send it.

Not yet done:

- Switch `factotum`'s `dp9ik` protocol to the keyroot oracle. It still
  uses its local `Authkey` path.
- Persist the sealed blob across reboots.
- Exercise `finish` and `reply` with a real auth server.

## 12h. Optional oracle fallback in factotum (2026-10-30)

The oracle is optional.  `factotum` must run on a system with no
keyroot service and use keyroot when it is present.

Implemented:

- **Default-on when detected.**  If `/mnt/keyroot/ctl` can be opened,
  factotum uses the keyroot oracle for dp9ik by default.  No per-key
  attribute is needed.  If keyroot is not mounted, factotum behaves
  exactly as before and uses the local `Authkey`.
- A dp9ik key may still carry `!oracle=<label>` for future label
  routing:
  ```
  key proto=dp9ik dom=test user=oracle !oracle=dp9ik/test
  ```
- A key may carry `!local` to force the old local path even when
  keyroot is present.
- `dp9ikaddkey` accepts an oracle-only key (no `!password` and no
  `!hex`).  If a password is also present, the local `Authkey` is still
  derived as a fallback.
- On `CNeedPAKreq`, factotum checks `/mnt/keyroot/ctl`.  If keyroot is
  available and the key does not say `!local`, it calls
  `dp9ik/init`, performs the auth-server exchange, and calls
  `dp9ik/finish`.  If any step fails, it falls back to the local path.
- On `CNeedAuth`, an oracle completed exchange calls `dp9ik/reply` and
  installs the returned 256-byte session secret.
- If an oracle-only key is used when keyroot is unavailable, auth fails
  with `keyroot unavailable`; keys with `!password` continue to work
  locally.

Installation policy:

- The installer detects whether the board has the OP-TEE keyroot TA and
  the keyroot service.  If both are present, it provisions the sealed
  dp9ik blob, starts `keyroot` before `factotum`, and the default-on
  detection enables the oracle without any user action.
- If either piece is missing, the installer leaves the system on the
  local `factotum` path.  Nothing in the base system requires the
  oracle.
- The install order matters: `keyroot` must be mounted at
  `/mnt/keyroot` before `factotum` starts so the detection sees it.

Measured without keyroot running:

```
factotum -n -s factotum
factotumkey
ctl:
key proto=dp9ik dom=test user=oracle !oracle?
```

That proves the daemon starts, parses the oracle key, and does not
depend on keyroot being present.

`secstore` is not changed yet.  It continues to use its existing
password/secstore path when keyroot is absent.  To use keyroot when
present, the service needs a sealed `secstore` secret operation; the
current service only exposes dp9ik blobs and operations.

## 12i. ML-KEM in the TA (2026-10-30)

PQClean ML-KEM-768 is now vendored into the trusted application.  The
TA commands are:

| command | purpose |
|---|---|
| 18 | `MLKEM_KEYGEN` |
| 19 | `MLKEM_ENCAPS` |
| 20 | `MLKEM_DECAPS` |
| 21 | `MLKEM_SELFTEST` |

Measured on the board:

```
mlkem keygen: pk=1184 blob=3646
mlkem encaps: ct=1088 ss=32
mlkem decaps: ss=32
mlkem shared match: 1
mlkem selftest: ret 0x0
```

The private key is sealed as a `KROOT2` blob with algorithm `4`
(ML-KEM-768) and usage `5`.  The 3,646-byte blob is the only private
form that leaves secure mode.

### Corrected secstore shape

secstore stores the users' **private keys**, not password verifiers.
There are two modes:

1. **Legacy mode.** The existing PAK/password flow and AES-CBC file
   encryption stay outside the TA.  Existing accounts and clients keep
   working.
2. **TA mode.** The account has an ML-DSA (and ML-KEM) public key.
   Login is a nonce challenge signed by the TA.  The stored private
   keys are sealed by the TA and used only through the TA; they are
   never exported in plaintext.

An account file may carry `PAK-Hi` for legacy mode and `MLDSA`/`MLKEM`
for TA mode.  The server chooses the mode from the client's first
message, so both can coexist.

The TA-mode session is hybrid:

```
K = HKDF(X25519 shared || ML-KEM shared || transcript)
```

The ML-DSA signature authenticates the transcript.  The vault key is
derived inside the TA from its ML-DSA secret with a domain-separated
HKDF:

```
vaultkey = HKDF(ML-DSA secret, "secstore vault v1")
```

The vault key never leaves the TA.  Stored private keys are imported
into TA slots and used through oracle operations; they are not exported
to normal world.  Legacy password-sealed files remain outside the TA.

Next steps:

- keyroot files for `mldsa/pub`, `mldsa/sign`, `mlkem/pub`,
  `mlkem/decaps`, and a TA-only vault seal/open operation.
- The key-mode secstore handshake and account parsing, with the legacy
  PAK path untouched.
- `secuser -k` enrollment and install-time provisioning.

## 12j. TA-generated user keys (2026-10-30)

The TA now generates the private keys that secstore stores.  The
private key is created inside secure mode and leaves only as a
`KUSER1` sealed blob plus its public key.

Measured on the board:

```
userkey identity: pk=1312 blob=3934
userkey keygen: blob=3932
userkey blob: 4b555345523101010c0005200a000000
userkey public: 1312 bytes
userkey selftest: ret 0x0
```

The `KUSER1` header decodes as:

```
magic     KUSER1
version   1
type      1 (ML-DSA-44; 2 = ML-KEM-768)
label len 12
publen    0x0520 = 1312
privlen   0x0a00 = 2560
```

The vault key is derived inside the TA:

```
vaultkey = HKDF(ML-DSA secret, "secstore vault v1", label)
```

The private key is sealed with AES-256-GCM under `vaultkey`, with the
header, label and public key as additional authenticated data.  The
public key stays in clear so it can be enrolled; the private key never
exists outside the TA in plaintext.

TA commands:

| command | purpose |
|---|---|
| 22 | `USERKEY_KEYGEN` — generate and seal a user private key |
| 23 | `USERKEY_PUBLIC` — extract the public key from a `KUSER1` blob |
| 24 | `USERKEY_SELFTEST` — generate/open both ML-DSA and ML-KEM user keys |

The self-test generates an ML-DSA identity, then for each supported
user-key type generates, seals, opens and compares the private key.
Both types pass.

This is the secstore payload: secstore stores `KUSER1` blobs, the
account stores the corresponding public key, and the TA imports a
blob back into secure memory when the key is needed.  Legacy
password-sealed files remain outside the TA.

Next:

- Keyroot files for `userkey/keygen`, `userkey/pub`, `mldsa/sign`,
  `mlkem/decaps`.
- The secstore key-mode handshake.
- `secuser -k` enrollment and install-time provisioning.

## 13. Open decisions

1. **First protocol.** Decided: the classical oracle core first
   (import, public, sign, MAC, p9sk1/dp9ik), then ML-DSA identity, then
   the ML-KEM hybrid. SSH/RSA can be added after p9sk1.
2. **Passphrase policy.** Required at boot (stolen-board protection, no
   unattended boot) or optional (unattended boot, batch-bound only)?
3. **Ed25519 source.** Use `monocypher` in the trusted application for
   the classical Yggdrasil/SSH identity.
4. **TLS integration.** Callback in `libsec` or a separate proxy; later.
5. **secstore.** Hybrid ML-KEM + dp9ik with legacy fallback, as decided;
   the remaining question is whether the password PAK also moves into
   the trusted application.
6. **Post-quantum library.** Choose PQClean or liboqs, and the parameter
   sets: ML-DSA-44 or ML-DSA-65 for identity, ML-KEM-768 for sessions.
