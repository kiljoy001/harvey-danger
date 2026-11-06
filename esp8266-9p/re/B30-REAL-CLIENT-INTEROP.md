# B30 - First real 9P client: plan9port finds a protocol bug

Status: DONE. A real, independent 9P client (plan9port
`9p`, /usr/local/plan9) now drives the device over
Wi-Fi: ls, ls -l, read, write, stat all pass. Board
restored to stock (verified). This round is the
milestone that turns "9P-shaped" into "a Plan 9
fileserver", and it immediately earned its keep.

## The bug (our client/server pair shared a dialect)

plan9port against the device:

    $ 9p -a 'tcp!192.168.1.146!564' ls /
    dirstat /: not implemented

A byte-level proxy trace against the host srv9p
harness (same failure locally) showed the conversation
was healthy - version, auth (Rerror, client proceeds),
attach, walk - until the client sent message type 124
and got our default Rerror. Type 124 is Tstat in
canonical 9P2000 (Harvey sys/include/fcall.h: Tstat =
124, Twstat = 126). srv9p had Tstat at 114, which is
Tcreate's slot - its enum had packed stat in early and
never left room. bench9p.py never sends Tstat, so our
server and our client agreed with each other for
months. The Go mock (mock9p/main.go) had the same
wrong constant; both are fixed (srv9p.c, main.go).

Everything else in the enum was already canonical
(Twalk 110, Topen 112, Tread 116, Twrite 118, Tclunk
120) - one wrong constant, not a private protocol.
Tcreate/Tremove/Twstat still return the intended
"not implemented" Rerror.

Lesson, recorded because it is the second time:
a self-authored client is not an interop test. The
first external implementation to speak to us found a
bug in the first minute (cf. B26's warning about
self-confirming checks).

## Also this round: IL heap diet (assoc-expire bisect)

With il9p created, the join fell into reason 4/205
assoc-expire cycles within seconds; without it, rock
stable at 30 KB free heap vs 18 KB with. The second
Pp9Server instance was the main cost. Fix: IL shares
the TCP server's single Pp9Server (DESIGN allows one
client per transport; in practice one client total),
the il9 engine's pending slot became optional
(il9_sendmsg refuses while busy; il_write pumps and
waits, drops nothing). Free heap back to 23732 with
IL listening; join stable, rx climbing. Details in
B29's addendum territory - see il9.h and blob_main.c
comments.

## Device verification (plan9port over Wi-Fi)

    9p ls /            console ctl mem net status version wifi
    9p ls /wifi        creds ctl scan status
    9p read /version   esp9 0.1 / srv9p 9P2000 msize 2048 / ...
    9p read /status    mode station, ip, rssi, heap, clients 1
    9p stat /status    'status' ... q (0000000000000003 0) m 0444
    9p read /wifi/creds  ssid Linksys01314 / key set
    9p write /wifi/ctl ("scan") -> 9p read /wifi/scan lists APs

bench9p 100 iterations still passes (regression).
Host: test_shim/kernel/pp/esf_buf/il9 pass; mock9p
builds. Stock AT verified after restore.

## Remaining interop gaps (honest list)

- Harvey itself (devmnt/9vm) has not mounted it yet;
  plan9port is a real client but not the target one.
  IL interop (B29) is likewise engine-tested only.
- plan9port `9p ls -l` passes against the host
  harness; on device, plain ls/read/write/stat were
  run (ls -l is the same Tstat path per entry).
- fid exhaustion behaviour under a real client that
  holds many fids (PP9_MAXFIDS 16) is untested; srv9p
  returns "no free fids" Rerror, which clients handle.
