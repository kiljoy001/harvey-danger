# B33 - IL proven against a real 9front kernel (+ a key leak, fixed)

Status: DONE. A real 9front machine (dev9p, driven
headless via `drawterm -G -h dev9p -u scott -a dev9p`,
factotum supplying auth) dials the ESP8266 over IL -
IP protocol 40 - mounts it, and does file I/O:

    srv il!192.168.1.146!564 esp9il8     -> post...
    mount /srv/esp9il8 /n/esp9il8        -> (no error)
    cat /n/esp9il8/version               -> esp9 0.1 ...
    cat /n/esp9il8/status                -> ilrx 33 iltx 33 ilstate 2
    echo scan > /n/esp9il8/wifi/ctl      -> (no error)
    cat /n/esp9il8/wifi/scan             -> AP list

ilstate 2 is IL9_ESTABLISHED; rx/tx counts balanced.
TCP from the same machine was re-proven as control
(mount + ls). Board restored to stock after the round
(verified AT+GMR).

## Bug 1: the glue ate exactly the handshake packets

First attempts timed out with zero IL reaching the
engine. Device-side arrival counter (added this round,
/status ilrx) separated arrival from state machine:

- il_raw_recv dropped any packet with
  `p->tot_len <= ihl + IL9_HDRSIZE`. A header-only IL
  packet (Sync, Close, bare Ack) is exactly 20 + 18 =
  38 bytes for IPv4, so every Sync was discarded
  before il9_input saw it. Data packets are longer
  and would have passed. `<=` -> `<`.
- The B29 host test fed the engine directly, so the
  engine was proven and the glue was not - fourth
  confirmation of the B30/B32 rule.

With the fix, ilrx climbed during attempts and the
engine entered SYNCEE and sent SYN-ACKs (iltx).

## The remaining failures were stale-peer wedging

Attempts still intermittently failed after that. The
counters told the story: engine stuck in SYNCEE
(ilstate 1), new Syncs arriving (ilrx climbing) but
replies going to the OLD peer's port. Sequence: an
early attempt fired while the device was still
rebooting ("host unreachable" at dev9p, but its
retransmits landed after the device came up); the
single-connection engine adopted that source port;
later attempts from new ports fail the peer check
(src_port != rport) and are silently ignored - which
is correct IL behaviour for an established peer, and
the kernel's own syncee has the same shape. A dead
syncee has no timeout in our il9_poll (kernel probes
only once established too), so `/ctl reboot` was used
to clear it; the next fresh attempt established on
the first Sync (ilrx 2 / iltx 1) and has run clean
since. Known limitation, recorded: add a syncee
timeout + Sync-reply retransmit to il9_poll.

A misread avoided: the kernel heartbeat line `il=`
is per-proc saved interrupt levels, NOT an IL packet
counter. It read 00000000 throughout and proved
nothing; that is why ilrx/iltx/iltxerr/ilstate are
now explicit fields in /status.

## Bug 2 (security): /status leaked the AP key

During setup, /status showed:

    lastctl wifi:connect Linksys01314 <the AP key>

wr_wifi recorded the raw ctl line into lastctl, which
/status serves to any LAN reader - contradicting the
creds design ("key set", never the key; B25). Fixed:
connect records only `wifi:connect <ssid>`. Verified
after re-provisioning: `lastctl wifi:connect
Linksys01314`. The key itself appears nowhere in any
readable file; grep of the firmware shows no other
logging of it.

## What "through the router" turned out to mean

Topology: dev9p is 192.168.2.202; the ESP is
192.168.1.146 behind a Linksys (its gateway .1); this
host is dual-homed (192.168.2.2 wired, 192.168.1.182
Wi-Fi). Default-path attempts never reached the
device (pre-fix, so protocol vs glue was ambiguous).
A /128 route on dev9p via this host (ip_forward=1)
carried Syncs in; SYN-ACKs returned ESP -> Linksys
-> dev9p and were accepted, so the Linksys passes
protocol 40 at least LAN->upstream. Pure router path
(upstream->LAN forwarding by the 2.1/Linksys chain)
remains unproven: the /128 could not be removed from
dev9p (`echo remove ... > /net/iproute` -> "interface
not found"; it clears at dev9p's next reboot) and the
pre-route attempts all predated the glue fix. snoopy
on dev9p supplied ground truth at each step (Sync
src/dst, retransmits, Close) and is the right tool
here; the kernel's /net/il/stats does not count Sync
traffic (OutMsgs counts data only, il.c:477).

## Files

- firmware/esp9/main/blob_main.c: glue `<` fix;
  ilrx/iltx/iltxerr/ilstate in /status; lastctl
  redaction. firmware/esp9/main/il9.c unchanged -
  the engine matched the kernel all along.
- Host probes: /tmp/il_probe*.py (serial watcher),
  drawterm -G command lines in this report.

bench9p 60 iters against the device still passes;
memory gate green (heap headroom 28040).
