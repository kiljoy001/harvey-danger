# mock9p - host mock of the esp9 namespace

Phase 1 (DESIGN section 9): serves the frozen v1 namespace from
`../doc/DESIGN.md` section 5 over TCP, with mock state, so the
design could be validated with real 9P clients before firmware
code was written against it. 9P2000, Go stdlib only.

    go run . -listen :1564

Validated with an independent Python 9P2000 client (not part of
this repo): version negotiation, attach, root listing, nested
walk to /wifi/status, stat, /wifi/ctl write (`scan` accepted,
`bogus` rejected with `Rerror: bad wifi ctl verb`), and walk to
a nonexistent file rejected. All passed. The Go server and the
Python client share no code, so the freeze rests on two
independent implementations of the protocol agreeing.

The firmware server (firmware/srv9p) implements the same frozen
namespace; this mock remains the reference for host-side tests.

