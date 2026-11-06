# B35 - Syncee timeout: a dead handshake can no longer wedge IL

Status: DONE. The B33 known limitation is closed:
il9's SYNCEE state now retransmits the Sync reply on
the kernel's schedule and hangs up after 16
retransmits. Host test and on-device wedge test both
pass. Board restored to stock (AT verified).

## The flaw

Single-connection IL server. A peer that sent a Sync
and then vanished (no ACK, no Close, no more packets)
left the engine in SYNCEE forever: new clients from
other source ports fail the peer check and are
silently ignored, so IL service was dead until reboot
- the failure B27 taught us to make impossible, one
layer down. Harvey's il.c has the answer in its
ackproc (Ilsyncee/Ilsyncer case): on timeout, resend
Sync, back off, hang up after MaxRexmit (16,
MaxTimeout 30 s).

## The fix (firmware/esp9/main/il9.c)

Same numbers as the DATA path (1 s doubling to 30 s,
16 retransmits), reusing the rexmit fields (provably
idle before ESTABLISHED):

- CLOSED -> SYNCEE arms rexmit_due = now + 1 s.
- A peer Sync retransmit re-replies and restarts the
  clock (peer is alive; il.c's syncer retransmits its
  Sync until answered, so this also heals a single
  lost Sync reply without waiting for our timer).
- ACK/DATA establishing clears the bookkeeping.
- il9_poll in SYNCEE: resend Sync on schedule; at 16
  retransmits, hangup() -> CLOSED + hangup callback.

Total wedge lifetime is bounded at ~6.5 minutes
(1+2+4+8+16+30x11 s), after which the listener is
free. il9.h's subset description updated to match.

Test (firmware/esp9/main/test/test_il9.c, same file,
scripted peer): retransmit at exactly the armed due
time, clock restart on peer retransmit, >= 17 Syncs
sent (initial + re-reply + 16), hangup callback
fires, state CLOSED, and a NEW peer port is accepted
afterwards. The test failed before the fix
(nwire==3 assertion) and passes after.

## On-device proof (the interesting part)

A well-behaved kernel peer cannot simulate dying
mid-handshake (its Close carries ack == our start,
which correctly releases us - verified separately:
a manual /net/il conversation closed and the engine
returned to CLOSED at once). So death was simulated
with a temporary, explicitly removed firewall rule
on the OpenWrt router (B34):

    nft insert rule inet fw4 forward position 516
      ip protocol 40 ip saddr 192.168.1.146
      ip daddr 192.168.2.202 counter drop
      # handle 569; deleted after the test

With ESP replies dropped:
- dev9p `srv il!...` timed out; ESP sat in SYNCEE
  (ilstate 1) with iltx climbing 14 -> 16 over 12 s:
  Sync-reply retransmits visible in /status.
- A second srv from a new port timed out: wedge
  reproduced (this is the pre-fix permanent state).
- Rule removed. Polling /status: ilstate reached 0
  after iltx 24, ~6.5 min after the first Sync -
  the designed expiry, observed.
- Fresh `srv il!` + mount + cat then worked
  (ilstate 2), no reboot involved.

bench9p still passes; memory gate green (28040).

## Remaining IL stance

Engine now matches il.c on handshake, data, credit,
query, close, and timeout. Transports proven against
a real kernel on every path the house offers (B33,
B34). Next project items are back in libpp: trc
stage 2 (rcGetSched PER machinery), then pp.o.
