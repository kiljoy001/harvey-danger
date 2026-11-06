# B32 - A real Harvey kernel mounts the device

Status: DONE. A Harvey (9front-derived) kernel, booted
in QEMU, dials the ESP8266 over TCP, posts it with srv,
mounts it, and reads and writes the namespace:

    ip/ipconfig
    srv tcp!192.168.1.146!564 esp9      -> post...
    mount /srv/esp9 /n/esp9             -> (no error)
    ls /n/esp9                          -> console ctl mem net status version wifi
    cat /n/esp9/version                 -> esp9 0.1 ...
    cat /n/esp9/wifi/creds              -> ssid Linksys01314 / key set
    echo scan > /n/esp9/wifi/ctl        -> (no error)
    cat /n/esp9/wifi/scan               -> AP list
    cat /n/esp9/status                  -> ... lastctl wifi:scan

ns(1) in the guest shows both mounts this session
(/n/esp9 and a probe mount on /tmp/mnt). Board
restored to stock after the round (verified AT+GMR).

## The bug: qid field order (vers,type) vs (type,vers)

Before the fix, every mount failed identically:

    mount: mount /n/esp9: inconsistent mount

srv posted fine; plan9port `9p` ls/read/write/stat all
passed (B30). The failure is cmount's first check in
Harvey sys/src/9/port/chan.c:

    if(QTDIR & (old->qid.type ^ (*newp)->qid.type))
        error(Emount);

It compares the mounted root's qid type against the
mount point's. Our root qid parsed as type 0, not
QTDIR (0x80), because srv9p emitted the qid as
vers[4], type[1], path[8]. The canonical 9P2000 wire
order is type[1], vers[4], path[8] - verified in two
independent implementations before editing:

- Harvey: sys/src/libc/9sys/convS2M.c pqid() writes
  type, vers, path; convM2S.c gqid() reads the same.
- plan9port: /usr/local/plan9/src/lib9/convM2S.c
  gqid() reads type, vers, path.

Why nothing caught it earlier: path[8] starts at byte
offset 5 under BOTH orders, so qid.path compared equal
everywhere; vers parsed as 0x80000000 for directories
(harmless - nothing compares vers for our files); and
plan9port's `9p` takes directory-ness from the stat
mode word (DMDIR, which we emit correctly), never from
qid.type. Only a kernel mount reads qid.type. mock9p
(Go) shared the wrong order - the same self-pair trap
as B30's Tstat constant, one layer down: both ends of
our test harness were written from the same wrong
assumption, so they agreed for months.

Fix: one function. srv9p.c wrqid() now writes type,
vers, path; mock9p/main.go qidBytes() likewise. A raw
socket probe (no client library) asserts the Rattach
bytes directly: type=0x80 vers=0 path=1 for the root.

## Second bug found by the same test: ctl newline

With the mount working, `echo scan > /n/esp9/wifi/ctl`
returned `bad wifi ctl verb` through devmnt (write
path and Rerror propagation both proven by that). rc's
echo appends '\n'; the device's copy_verb() did not
strip it, so strcmp(buf, "scan") failed. Every ctl
verb from any shell would have failed the same way -
i.e. the thesis demo (`echo ... > ctl`) was broken for
real clients while bench9p's newline-free Twrite
passed. copy_verb() now strips trailing CR/LF. This is
the Plan 9 convention: ctl parsers accept the newline
that echo inevitably sends.

Re-verified from the guest after the fix: echo scan
succeeds, status shows `lastctl wifi:scan`, scan
results read back. plan9port write with '\n' also
passes; bench9p (100 iters, device) still passes.

## IL from the guest: blocked by the test rig, not the code

The guest kernel has IL (/net/il exists; Harvey
sys/src/9/ip/il.c). But the guest reaches the LAN
through QEMU user networking (slirp), which forwards
TCP/UDP/ICMP only - IP protocol 40 cannot cross it, so
a guest `srv il!...` cannot reach the device. No
CAP_NET_ADMIN/CAP_NET_RAW on the host for a TAP/raw
alternative (established earlier). IL remains:
host-tested engine (test_il9), listener live on the
device sharing the one Pp9Server. Proving IL from a
real kernel needs a bridged/TAP guest or a second
physical Plan 9 machine on the LAN.

## What this proves

- The device is a mountable Plan 9 fileserver for a
  real kernel, not just for plan9port: attach, walk,
  open, read, write, stat, clunk through devmnt, with
  kernel-side qid checks passing.
- Kernel mounts cost the device nothing extra: same
  srv9p core, same single Pp9Server, heap 23116 with
  a Harvey client attached, procs 8/8.
- Interop rule, third confirmation (B30, B31 host
  vectors, now B32): every external implementation
  that speaks to this server finds a bug our own
  harness cannot see. Budget for an external client
  in every future protocol change.

Host harness for this round: /tmp/harvey_probe*.py
(QEMU copy of the 9front image, rc driven over
serial); guest commands are recorded verbatim above.
