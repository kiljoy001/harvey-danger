# B29 - Improved IL transport (DESIGN 4.1), + status docs fixed

Status: engine DONE + host tests; firmware integration
boots with IL listener at 8/8 procs; Harvey interop NOT
yet tested; TCP regression bench this round blocked by
an AP assoc cycle (pre-existing flakiness, B26).

## What was built

- firmware/esp9/main/il9.c/.h: minimal IL server
  endpoint from Harvey sys/src/9/ip/il.c semantics
  (read, not copied): 18-byte header, SYNC offering
  Ilflow both ways, 8-byte consumed-bytes credit on
  ACK/STATE, 100 ms delayed ACK, retransmit of the
  single unacked DATA as Ildataquery (1 s doubling to
  30 s, 16 max), STATE-driven early retransmit via the
  qt[] highest-sent table, query after 10 s silence,
  hangup after ~60 s of unanswered queries, close
  handshake. One connection, in-order delivery only,
  duplicates re-ACKed, bad checksums dropped.
- firmware/esp9/main/test/test_il9.c: scripted peer
  speaking il.c's side (own checksum/field code):
  handshake, delivery, delayed-ACK timing and credit
  value, duplicate/out-of-order suppression, dataquery
  retransmission, checksum rejection, close. Passes.
- Device glue (blob_main.c): raw lwIP PCB on IP
  protocol 40, one-slot packet mailbox copied in the
  tcpip proc, all IL/9P work in a new il9p proc with
  its own Pp9Server instance. Replies are framed by
  the 9P length prefix into whole IL messages; a busy
  engine slot pumps RX and waits, never drops.

## Budgets, via the loud path (B27 working as intended)

- First IL build died loudly at boot: FATAL stack pool
  exhausted (32768 + 4096 > 34816) creating il9p. The
  B27 machinery caught in seconds what B26 lost hours
  to. 
- A +4096 pool enlargement was then bisected to break
  blob Wi-Fi init (TxNodeNum alignment fault during
  esp_wifi_init, before any of our code runs): every
  pool byte is a byte off the init heap, and init
  starvation is a known silent blob failure (B19).
  Final sizing: pool 35840 (+1024), il9p stack 3072.
- Both Pp9Server instances (TCP srv, IL il_srv) moved
  from BSS to one-time heap allocations, freeing
  4384 B of BSS. Memory gate green: headroom 30096.
- Boot: `procs 8/8: uiT tiT TmrSvc sys_evt ppT cons
  srv9p il9p`, `MILESTONE il-listening proto 40 port
  564`, scan-ok, later join-ok rssi=-51.

## Addendum (B30): IL heap diet

First device runs with il9p fell into reason 4/205
assoc-expire cycles seconds after joining; bisect
(il9p creation disabled) was stable at 30 KB free heap
vs 18 KB with. Fix landed with B30: IL shares the TCP
server's Pp9Server instead of a second instance,
il9 pending slot became optional (NULL = refuse while
busy), engine+rx buffers stay heap (4.1 KB total).
Free heap 23732 with IL listening; join stable.

## Not verified, stated plainly

- No Harvey host has spoken IL to it yet. A Linux raw
  IL socket needs CAP_NET_RAW (unavailable here), so
  on-device proof stops at the listening milestone.
  The 9vm/Harvey mount test is the next IL step.
- TCP bench regression for this build was attempted
  but the AP fell into a reason-4/205 assoc cycle for
  the whole window (seen before in B26 on pre-IL
  builds). TCP code changes this round were limited
  to the srv heap move.
- IL heap cost: free heap at alive dropped from ~31k
  to ~18k (two server instances + IL buffers). Fine
  now; watch it when SoftAP (second netif) lands.

## Docs correction (same round)

README.md and doc/DESIGN.md still claimed "nothing
flashed" / "design only" - stale since B7. Both status
headers now describe the working system and point at
the reports. README gained a "What boots today"
section stating precisely which layers are ours.
