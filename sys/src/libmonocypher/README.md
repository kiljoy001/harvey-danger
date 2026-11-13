# libmonocypher

Monocypher 4.0.2, vendored from the Objective-9C tree (which in turn
carries the upstream release).  Upstream: <https://monocypher.org>.
Licence: BSD-2-Clause OR CC0-1.0; the licence text is in the source
header.

Harvey's `libsec` provides the classic Plan 9 primitives (AES, DES,
RSA, DSA, ElGamal, SHA-1/SHA-2, MD5, RC4).  It has no ChaCha20,
Poly1305, X25519, Ed25519 or BLAKE2b.  Monocypher supplies those in
portable C with no libc dependencies beyond the prototypes it does not
call.

## Build

```sh
python3 9vm/build_libmonocypher.py
```

This produces `build/libmonocypher.a` and `build/aspera-mctest`.  The
test binary runs the RFC 7693/8439/7748 known-answer vectors plus
EdDSA and AEAD round trips; `build_aspera_tools.py` and the Halatha
guest staging copy it to `/bin/mctest`.

## Use

Include `<u.h>` and `<libc.h>` first, then `<monocypher.h>`:

```c
#include <u.h>
#include <libc.h>
#include <monocypher.h>

uchar key[32], nonce[24], mac[16];
crypto_aead_lock(cipher, mac, key, nonce, ad, adlen, plain, plainlen);
```

The Harvey header defines `MONO_PLAN9`, so the fixed-width types come
from `u.h` (`u8int`, `u32int`, ...) rather than `stdint.h`.  There is
no `stdint.h` or `stddef.h` in `sys/include`; do not add them just for
this library.

Link with `build/libmonocypher.a` before `build/libc.a`.

## Deliberate omissions

`monocypher-ed25519.c` (SHA-512 EdDSA, as opposed to Monocypher's
BLAKE2b EdDSA) is not vendored.  Add it if a protocol needs
RFC 8032-compatible signatures rather than the Monocypher variant.
