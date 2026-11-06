# B14 - RTOS-substrate server (item 1) and host load benchmark (item 2)

Status: both done and verified. Board restored to stock AT.

## Item 1: same 9P behaviour on the RTOS substrate

New project `firmware/rtossrv/`: the frozen namespace (DESIGN
section 5, including `/mem`) served by the same srv9p core over
lwIP sockets on FreeRTOS, with `esp_wifi` blobs underneath.
SoftAP `ESP9-3968` plus station, same ctl verbs as Phase A.
`build-rtossrv.sh` builds it with the RTOS SDK v3.4 (make
system). The RTOS SDK requires the GCC 8.4 esp-2020r3 toolchain
(the pinned NONOS GCC 5.2 lacks `-lc_nano`); provenance,
including the tools.json SHA-256 match, is in
`tools/PROVENANCE.md`.

Provisioned test build joined `Linksys01314`, took
`192.168.1.146`, and served a Python 9P2000 client over the
LAN in full:

- `/version` -> `esp9 rtos 0.1 ... sdk v3.4`
- `/status` -> same fields as NONOS; heap 45260 idle (NONOS
  Phase A showed ~21800; different heap accounting and a
  larger runtime)
- `/mem` -> 128 samples; samples during the connection show
  `ev=1` and heap down to 42084 (the connection costs ~3 KB
  on this substrate, more than NONOS's ~400 B; larger lwIP
  and task footprint)
- `/wifi/ctl` write `scan` -> scan results including
  `Linksys01314 1 -56 3`; bad verb to `/ctl` ->
  `Rerror: bad ctl verb`

So the namespace, ctl grammar, and error behaviour are
substrate-independent, as designed. The remaining differences
are the version string and memory scale, both expected.

## Item 2: host load benchmark

`firmware/srv9p/test/bench9p.py`: a 9P2000 client that times
version+attach setup, repeated Tread on an open fid, full
readall, walk+open+read+clunk cycles, and ctl writes, reporting
median/p95 latency and ops/s.

First result exposed a transport bug, not a protocol cost:
the C srv9p harness sat at exactly ~41 ms per request
(24 ops/s) while the Go mock ran ~55k ops/s. srv9p sends each
reply as a 4-byte header write plus a body write; with Nagle
on the server and delayed ACK on the client, every reply
stalls ~40 ms. This is the host twin of the on-device espconn
problem from B13. Setting TCP_NODELAY on the accepted socket
in `srv9p_host.c` (and on the RTOS server socket in
`rtossrv/main/rtos_main.c`) fixed it:

- C srv9p: Tread 0.016 ms median, 61635 ops/s; walk cycle
  12113 ops/s - parity with the Go mock (62229 / 11800).
- Go mock: unchanged, confirming the diagnosis.

Against the flashed RTOS device (60 iterations): before
NODELAY, Tread median 57.3 ms (17.5 ops/s); after, 12.0 ms
(83.3 ops/s), walk cycle 15.6 ops/s. Wi-Fi latency dominates
now, as it should.

## Restore

Key removed from `rtos_config.h` (grep-verified), default
RTOS image rebuilt (SHA-256
`4e237e01c796cffe3e65a6870b3ea740f3afc62f920eafc5e6bb4cda7cf3ad6f`),
full stock backup restored hash-verified, AT probe returns
the original strings. Board state: stock AT.

## Next steps

1. Add `/mem` to the Go mock and host harness tables so the
   client test and benchmark cover it everywhere.
2. On-device benchmark of the NONOS Phase A build with the
   same bench9p workload, for a three-way substrate table.
