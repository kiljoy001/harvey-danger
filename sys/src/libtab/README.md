# libtab

Tabular storage in Plan 9 `ndb` syntax, with hashed, signed and
encrypted cells backed by [Monocypher](https://monocypher.org/).
Vendored from `kiljoy001/libtab` commit `07d2f4e`
(`add-encrypted-cell-type`), the branch that adds ENCRYPTED cells,
the cell-size cap and commit-time compare-and-swap.

A table is plain text:

```
schema=config
	col=key
	col=value
	col=secret type=HASHED algo=argon2id
	col=sig type=SIGNED
	col=sealed type=ENCRYPTED

key=host
	value=example.org
	secret=hashed:...
	sig=signed:...
	sealed=encrypted:...
```

Rows are keyed by the hash of their canonical bytes, not by a column.
`tab_get` returns cell text; `tab_search` returns every matching row.

Cell types are plain, `HASHED` (BLAKE2b-256 or Argon2id), `SIGNED`
(EdDSA-BLAKE2b) and `ENCRYPTED` (XChaCha20-Poly1305 with a per-cell
random 24-byte nonce).  Encrypted cells carry `ver | nonce | mac |
ciphertext` in one base64 blob, so decryption needs only the cell and
the 32-byte key.  Row identity hashes the ciphertext, so encrypting
the same plaintext twice yields two distinct rows; key the row on a
plain or HASHED column and treat the encrypted cell as payload.

## Build

Monocypher must be built first:

```sh
python3 util/9vm/build_libmonocypher.py
python3 util/9vm/build_libtab.py
```

Outputs: `build/libtab.a`, `build/aspera-tabtest` (core suite),
`build/aspera-tabcryptest` (HASHED/SIGNED suite) and `build/aspera-tab`
(the command).  The suites use `/tmp` for their fixtures.

## Port notes

Compiled without `USE_POSIX`, so `tab_commit` truncates and rewrites the
target in place, the same as native Plan 9 upstream.  Harvey's
`sys/include/ndb.h` had a 256-byte `Ndb.buf` (9front uses 8192); it is
now 8192 so cells up to `TabMaxCell` survive a reload.  Commit-time
compare-and-swap also hashes the file contents with BLAKE2b, because
Harvey's ramfs does not advance `qid.vers` on a rewrite.  `tab_open_dial`
and the lib9pclient write path are not available; the optional stub was
not imported.  `getrandom_compat.c` supplies the Argon2id salt source
from `/dev/random`.  The public header lives in `sys/include/libtab.h`
and the sources include `<libtab.h>`, `<bio.h>`, `<ndb.h>` and
`<monocypher.h>` from the normal Harvey include path.

## Command

```
tab FILE init SCHEMA COL[:TYPE[:ALGO]]...
tab FILE dump
tab FILE get COL VALUE FIELD
tab FILE set COL VALUE FIELD VALUE
tab FILE hash COL=VALUE FIELD                 # preimage on stdin
tab FILE sign COL=VALUE FIELD KEYFILE         # body on stdin
tab FILE verify COL=VALUE FIELD PUBFILE
tab FILE encrypt COL=VALUE FIELD KEYFILE      # plaintext on stdin
tab FILE decrypt COL=VALUE FIELD KEYFILE
tab keygen KEYFILE                           # 32-byte symmetric key
tab keygen SECRETKEY PUBLICKEY               # EdDSA pair
```

`COL=VALUE` may also be written as two words because rc treats a bare
word containing `=` as an environment assignment.  Key files hold raw
Monocypher bytes: 64-byte EdDSA secret key, 32-byte public key.

## Security model

- `HASHED` cells store BLAKE2b-256 or Argon2id digests (schema `algo=`);
  the preimage is not recoverable.
- `SIGNED` cells store `signed:<b64 body>:<b64 signature>` and are
  verified at read time with the caller's public key.
- `ENCRYPTED` cells store `encrypted:<b64 ver|nonce|mac|ciphertext>`;
  a wrong key and a tampered cell both fail authentication
  indistinguishably, because the nonce and MAC travel with the cell.
- Cells are ordinary text, so config can be inspected with `cat` and
  `grep`, but anyone who can write the file can replace plain cells.
  Integrity of a row needs a SIGNED column covering the security-
  relevant fields.
- Key handling is deliberately out of scope: libtab never opens key
  files or resolves principals.
