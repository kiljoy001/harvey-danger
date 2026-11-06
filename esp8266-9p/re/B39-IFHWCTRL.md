# B39: if_hwctrl.o replaced (clean-room interface control)

## Result

`libpp.a(if_hwctrl.o)` (12 KB blob, 19 `ic_*` exports) is replaced
by clean-room [if_hwctrl.c](../firmware/libpp/if_hwctrl.c)/
[if_hwctrl.h](../firmware/libpp/if_hwctrl.h) (~260 lines),
transliterated from [inventory/b39](inventory/b39/). Linked image
has 0 `libpp.a(if_hwctrl.o)` references; export set matches the
blob (minus three header-artifact commons `g_cnxMgr`,
`wifi_sta_rx_probe_req`, `__packed__`, which pp.o/other objects
already provide).

Remaining libpp blob members: `lmac.o`, `wdev.o`. libnet80211 and
libphy remain whole blob.

## What if_hwctrl is

Per-interface control: the `if_ctrl` commons (2 interfaces x
16 bytes: +0 addr[6], +8 opmode, +0x0c ptk_alg, +0x0d gtk_alg)
and the `interface_mask` byte. `ic_set_vif` brings interfaces up
(first enable sets MAC RX bit 31 at 0x3ff20004) and down,
`ic_set_sta` registers stations with the rate-control table,
`ic_set_key`/`ic_get_key` forward key programming to wdev.

## On-device proof (ESP8266EX, /dev/ttyUSB2)

- Boot `procs 8/8`, auto-join `join-ok rssi=-59`
- plan9port `9p` /status over TCP 564
- bench9p 60 iters: BENCH9P: DONE
- 9P scan write + AP list read
- 9front (dev9p) IL mount: ilstate 2, ilrx 9 / iltx 8, /status
  and scan write over IL
- Memory gate green, heap headroom 26768 (unchanged)

Host tests: `test_ifhw` (mask/popcount, alg bytes, set_vif MAC
bit, STA counter, RSSI, slot truncation, key-length forwarding).

## The bug: a hidden stack argument (key length)

First flash joined and completed DHCP, then went ARP-silent
forever: associated, GOT_IP, never answered. Hybrid links
(blob object with renamed symbols, forwarded selectively)
narrowed it to `ic_set_key` — yet its instruction stream was
semantically identical to the blob's.

The blob's `ic_set_key` has a store that looks dead:

```c
*(u32 *)(frame + 0) = incoming_stack_arg0;
```

In the call0 ABI that slot is the *outgoing* stack-arg area for
the next call. `wDev_Insert_KeyEntry` reads it:

```c
memcpy(key_entry, keydata /* a7 */, keylen /* stack arg */);
```

So `ic_set_key(ctx, alg, ..., keylen)` forwards its own incoming
stack argument — the key length — to `wDev_Insert_KeyEntry`.
Our version left a saved callee register in that slot, so wDev
copied a garbage number of key bytes (a truncated key, in the
observed case) and the AP could not decrypt anything after the
4-way handshake. DHCP (broadcast, pre/early keys) still worked,
which made the station look joined.

Fix: declare all seven `wDev_Insert_KeyEntry` arguments and pass
`p7` through as the stack argument. Two sibling traps were found
by the same audit and fixed first (both real, neither was the
ARP bug): `ic_set_vif`'s teardown zeroes `wDev_SetRxPolicy`'s
third argument (`movi a4, 0` in the blob; we had passed the MAC
pointer through), and `ic_set_sta`'s slot is byte-truncated by
the blob (`extui a4, a5, 0, 8`) while wl_cnx packs flags above
the low byte — untruncated it indexes `trcs[9]` out of bounds.

Lesson for the remaining blobs (lmac, wdev): when a blob function
calls another with "too few" arguments, check the callee for
flow-through register *and stack* reads before transliterating.
A store that looks dead may be an outgoing-argument write.

Stock firmware restored and verified after the round (AT / AT+GMR).
