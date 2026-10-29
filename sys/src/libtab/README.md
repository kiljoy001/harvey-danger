# libtab

Tabular storage in Plan 9 `ndb` syntax, with hashed and signed cells
backed by [Monocypher](https://monocypher.org/).  Vendored from the
upstream `kiljoy001/libtab` source carried in the Objective-9C tree.

A table is plain text:

```
schema=config
	col=key
	col=value
	col=secret type=HASHED algo=argon2id
	col=sig type=SIGNED

key=host
	value=example.org
	secret=hashed:...
	sig=signed:...
```

Rows are keyed by the hash of their canonical bytes, not by a column.
`tab_get` returns cell text; `tab_search` returns every matching row.

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
target in place, the same as native Plan 9 upstream.  `tab_open_dial`
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
tab keygen SECRETKEY PUBLICKEY
```

`COL=VALUE` may also be written as two words because rc treats a bare
word containing `=` as an environment assignment.  Key files hold raw
Monocypher bytes: 64-byte EdDSA secret key, 32-byte public key.

## Security model

- `HASHED` cells store BLAKE2b-256 or Argon2id digests (schema `algo=`);
  the preimage is not recoverable.
- `SIGNED` cells store `signed:<b64 body>:<b64 signature>` and are
  verified at read time with the caller's public key.
- Cells are ordinary text, so config can be inspected with `cat` and
  `grep`, but anyone who can write the file can replace plain cells.
  Integrity of a row needs a SIGNED column covering the security-
  relevant fields.
- Key handling is deliberately out of scope: libtab never opens key
  files or resolves principals.
