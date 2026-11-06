# B34 - The router was never the problem

Status: DONE (addendum to B33). Prompted by "log into
the 2.1 router and see why it's not taking the
packets". Answer: it is taking them; it always was.
The early default-path failures were the B33 glue bug
(ESP dropping 38-byte Syncs), which predated every
router-path attempt and made the router look guilty.

Topology, established from the router itself:

- 192.168.2.1 is OpenWrt 24.10.4 (SSH key login from
  this host worked). br-lan = 192.168.2.1/24 (dev9p,
  this host wired). Its upstream is a Wi-Fi station
  interface, phy1-sta0 = 192.168.1.188/24, associated
  to the Linksys AP - the router is a wireless client
  of the same AP the ESP uses (ESP gateway is the
  Linksys, 192.168.1.1).
- uci firewall: phy1-sta0's network
  (wireless_network) is in the wan zone, masq=1;
  forwarding lan->wan and wan->lan both configured.

Test (ESP freshly flashed + provisioned, ilstate 0;
dev9p route table verified clean - the B33 /128 was
removed with the 4-field form `remove <dst> <mask>
<gate>`, the 3-field form failing "interface not
found" on dev9p's kernel):

    srv il!192.168.1.146!564 esp9rt   -> post...
    mount /srv/esp9rt /n/esp9rt       -> ok
    ls, cat version/status, echo scan > wifi/ctl
    ESP /status over IL: ilrx 36 iltx 35 ilstate 2

Router /proc/net/nf_conntrack during/after:

    ipv4 2 unknown 40 src=192.168.1.146 dst=192.168.2.202
      packets=115 ... src=192.168.2.202 dst=192.168.1.146
      packets=65

So OpenWrt forwards IP protocol 40 between zones and
its masquerade handles it via generic ("unknown")
conntrack; NAT does not break IL here because the
conntrack entry is symmetric and the reply's source
(192.168.1.146) still matches the client's expected
peer. No router change was needed or made (read-only
inspection only: uci show, ip route, conntrack).

Revised B33 statement: the "pure router path
unproven" caveat is closed. Proven paths for IL from
a real 9front kernel: (a) dev9p default route via
OpenWrt NAT, (b) dev9p /128 via this dual-homed host
(ip_forward=1), (c) TCP everywhere. Remaining IL
work is ours, not the network's: syncee timeout in
il9_poll (B33 known limitation).

Board restored to stock after this round (AT
verified), as usual.
