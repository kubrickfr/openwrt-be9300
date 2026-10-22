# PPE flow offload for the GL-BE9300: bench plan

Bench test of the PPE flowtable offload with PPPoE: qualcommbe patches
0433-0439 and 0441, generic pending-6.18/714. The gates are DESIGN.md
section 9. Order is fixed: the fake-ISP rig first (section 5), the live line
only after every rig phase passed (section 6). The owner runs every step
that changes state; an assistant only reads. On any stop line (section 2):
stop, recover (section 1.3), capture (section 7). Nothing here has run yet.

Names: PPE port 1 is the conduit `lan` (to the RTL8372N), port 2 is `wan`;
the session is `pppoe-wan` on `wan.835`. br-lan is VLAN-aware; on this
gateway lan1 and lan4 are tagged members of VLAN 1, lan2 and lan3 carry
VLANs 10/20 only, and br-lan's MAC differs from the conduit's (read-only
check, 2026-10-22).

## 0. Bench record

(none yet)

## 1. Safety envelope

### 1.1 Pre-flight, before every session

```sh
ls -l /root/rollback.armed; grep -n rollback /etc/rc.local     # disarm leftovers first
uci get firewall.@defaults[0].flow_offloading_hw                # 0
cat /sys/module/qcom_ppe/parameters/flow_offload                # N
cat /sys/module/qcom_ppe/parameters/pppoe_offload               # N
grep qcom_ppe /etc/modules.conf                                 # nothing
cat /sys/kernel/debug/ppe/offload_flows | head -3               # enabled 0 degraded 0 tripped 0
dmesg | grep -c 'disable incomplete\|restoring .* failed\|new flows declined'   # 0
ubus call system watchdog                                       # status running
sysctl net.netfilter.nf_conntrack_acct                          # 1 (for conntrack counters)
```

A leftover rollback from earlier work (`/root/rollback.armed` and its
rc.local line) is disarmed before anything new is armed: `rm -f
/root/rollback.armed; sed -i '/rollback.armed/d' /etc/rc.local`. A second
path to the router is checked before each phase: Wi-Fi (`ap-mld0`) when the
PC is on the LAN, the LAN when the operator is on Wi-Fi.

### 1.2 Timers

Every step that turns something on first arms its own rollback, with its own
flag file and a duration longer than the whole step (its stated time plus
at least five minutes; never a timer that can fire in the middle of a
gate). The timer writes its own PID, so that it is killed by PID (BusyBox
`pkill` can match itself):

```sh
N=r2a; T=1200                            # unique per timer, never reused
touch /tmp/ppe-rb-$N
setsid sh -c "echo \$\$ > /tmp/ppe-rb-$N.pid; sleep $T; [ -e /tmp/ppe-rb-$N ] && { echo 0 > /sys/module/qcom_ppe/parameters/pppoe_offload; echo 0 > /sys/module/qcom_ppe/parameters/flow_offload; uci revert firewall; fw4 reload; logger ppe-rb-$N fired; }" </dev/null >/dev/null 2>&1 &
sleep 1; kill -0 "$(cat /tmp/ppe-rb-$N.pid)" && echo armed
# step passed:   rm -f /tmp/ppe-rb-$N; kill "$(cat /tmp/ppe-rb-$N.pid)"
```

A long step (L3, the soak) is renewed before it can fire: arm the next
timer under a new name, check it is armed, then disarm the old one. A timer
that fired is a failed step, never a passed one.

A hard kernel hang is covered by the hardware watchdog, which procd feeds
(`ubus call system watchdog`: 30 s timeout here): the router resets by
itself. A soft wedge (a write to the driver that never returns, the rest of
the system alive) is covered for the risky short steps (R1, R2, R6, L1, L2)
by a dead-man that panics the kernel, so that ramoops keeps a record and
`kernel.panic=3` reboots:

```sh
touch /tmp/ppe-dm
setsid sh -c 'echo $$ > /tmp/ppe-dm.pid; sleep 2400; [ -e /tmp/ppe-dm ] && echo c > /proc/sysrq-trigger' </dev/null >/dev/null 2>&1 &
# step done: rm -f /tmp/ppe-dm; kill "$(cat /tmp/ppe-dm.pid)"
```

Nothing stays armed across a break.

### 1.3 Recovery ladder

1. `echo 0 > /sys/module/qcom_ppe/parameters/flow_offload`: every flow entry
   deleted, registers written back; flows continue in software. Check
   `dmesg | tail` for `disabled: N flow(s)`; `disable incomplete` means
   entries may still forward: go to 3.
2. `uci revert firewall; fw4 reload` (or `uci set
   firewall.@defaults[0].flow_offloading_hw=0; fw4 reload`): software
   flowtable.
3. `reboot` (the PPE is reset at probe). On a hang, wait 90 s for the panic
   reboot before power-cycling, then read `/sys/fs/pstore/` first.
4. Flash the previous image (kept, with its `sha256sum`, on the PC).

Neither switch is persisted (no `uci commit firewall`, nothing in
`/etc/modules.conf`) before section 6.5: a reboot always comes back off.
`sysupgrade -c` keeps `/etc/modules.conf`: check it after every upgrade.

### 1.4 Reading state under load

Allowed: `/proc/stat`, `/proc/softirqs`, `/proc/interrupts`, `cat
/sys/kernel/debug/ppe/offload_flows` and `offload_profile` (driver memory,
no register read), `conntrack -L`, `nft list flowtables`, and a bounded,
filtered `tcpdump` (`timeout 70 tcpdump -c N ...`) on an interface whose
traffic reaches the CPU only at a low rate. Not under load (register
reads): `offload_flows_hw`, the PPE debugfs counters (`cpu_code`,
`port_rx`, ...), `ethtool -S`, `/sys/class/net/*/statistics`, `ip -s link`,
`/proc/net/dev`. Read those before and after a transfer. On an EDMA port,
`/sys/class/net/*/statistics`, `ip -s link` and `/proc/net/dev` are the
port MAC's counters, every frame on the wire, offloaded or not; only
`ethtool -S <port> | grep -w rx_packets` (and `tx_packets`) counts what
EDMA exchanged with the CPU. "Absent from the CPU path" is always read
from the latter.

## 2. Stop lines

Stop, recover, capture on any of: the PPPoE session dropping or pppd logging
echo failures; IPv4 or IPv6 loss from the LAN; a frame of a flow on any LAN
port other than the one it names, or leaving with the wrong VLAN, source MAC
or a 0x8899/0x88a8 tag; a hand-built case frame of 5.4 (not a control,
which hardware forwards by design, nor the recorded residual) reaching the
client without appearing in the router's inbound `wan` capture (software
forwards some of them itself: 5.4's baseline); a CPU-idle collapse; or any
line from:

```sh
dmesg | grep -E 'ppe_offload: (not enabled|.*read back|.*not cleared|.*could not be deleted|disable incomplete|.*restor.* failed|.*not set up|.*turning offload off|.*PPPoE stays in software|.* [1-9][0-9]* stuck|IP frame from|profile A kept)'
dmesg | grep -E 'Call trace|Unable to handle|BUG:|WARNING:'
```

Recorded, not stops: `declined - <reason>` (once per reason: uploads,
Wi-Fi, VLAN 10/20, IPv6 are expected), `re-armed` (debug), `table full`,
`flowtable unbound`, `bridge change`.

## 3. Build and flash

Build on geekom from the gateway's `.config`, adding
`CONFIG_PACKAGE_kmod-ramoops=y`, `CONFIG_PACKAGE_conntrack=y`,
`CONFIG_PACKAGE_tcpdump=y`, `CONFIG_PACKAGE_iperf3=y`. Offload is off in the
image; nothing else changes.

```sh
make defconfig && make -j"$(nproc)"
grep -c ppe_offload_unbind build_dir/target-*/linux-qualcommbe_ipq53xx/linux-*/drivers/net/ethernet/qualcomm/ppe/ppe_offload.c   # >= 1
grep -c FLOW_DISSECTOR_KEY_PPPOE build_dir/target-*/linux-qualcommbe_ipq53xx/linux-*/net/netfilter/nf_flow_table_offload.c    # >= 1
```

Before flashing: `sysupgrade -b /tmp/backup.tar.gz` and copy it off (`scp
-O`); keep the running image and its `sha256sum` on the PC. The owner
flashes. After boot: section 1.1 reads clean, `ppe_offload` prints nothing,
and everything works as on the old image for a day (this is the image kept
if every later phase fails). Recheck the https-dns-proxy bootstrap padding
after the flash.

## 4. The rig

- WAN: the PC's 2.5G NIC `enp7s0u2` on the router's WAN jack, MTU 1508
  or more, `ip link add link enp7s0u2 name enp7s0u2.835 mtu 1508 type vlan
  id 835`, a PPPoE concentrator on `enp7s0u2.835` (accel-ppp or rp-pppoe in
  kernel mode) that **does not ask for authentication** (the router never
  sends its real credentials), MTU and MRU 1500 with RFC 4638
  (PPP-Max-Payload 1500), `lcp-echo-interval 1 lcp-echo-failure 3`, local
  10.64.0.1. `iperf3 -s`, an HTTP server with a large file, `scapy` and
  `tcpdump` on the PC. The PC's own IP traffic on that device comes from
  the concentrator's MAC: `sysctl -w
  net.ipv6.conf.enp7s0u2/835.disable_ipv6=1`, no DHCP client, no
  NetworkManager (`nmcli dev set enp7s0u2.835 managed no`), no mDNS on it;
  `timeout 600 tcpdump -n -e -i enp7s0u2.835 'ip or ip6'` on the PC
  captures nothing before R2.
- LAN: geekom on lan1 as the client (tagged VLAN 1, as configured), the
  operator on Wi-Fi.
- The household is offline for the whole rig session. Afterwards the ONT
  goes back, `ifup wan`, and a real session, a default route and IPv4/IPv6
  from a LAN host are checked.
- Every frame the PC builds by hand (HD, HE2b's DEI case, every R4 case and
  the watch frame) has TTL 200 and is unicast to the router's WAN MAC (`cat
  /sys/class/net/wan/address`, which wan.835 shares): `Ether(src=<as the
  case says>, dst=<wan MAC>)`, the Dot1Q layer written out where the case
  has a tag, sent with `sendp(..., iface="enp7s0u2")`. scapy's default
  destination for these stacks is broadcast, which the session entry
  (unicast only), the L3 interface (MY_MAC) and the watch (PACKET_HOST) all
  ignore, so such a frame tests nothing; the router's `tcpdump -e` shows
  each frame's destination, and a case sent to ff:ff:ff:ff:ff:ff is sent
  again.
- Offload restricted to the test flows: run one client, and start every
  measured transfer with a fixed client port (`--cport` / `--local-port`)
  so the flow is the same 5-tuple in every capture filter.

## 5. Rig phases

### 5.1 R1, HE1: profile E alone

Arm `r1` (1200 s) and the dead-man. fw4 hardware offload stays **off** (no
flow is offered), then `echo 1 >
/sys/module/qcom_ppe/parameters/flow_offload`.

- Expect `ppe_offload: enabled: TCP exceptions ok, header exceptions ok,
  PPPoE exceptions ok, profile E ok (port 1)` and `rtl8_4 head tag emulation
  on PPE port 1`.
- 10 minutes: ARP and DHCP from geekom, IPv6 RA/ND, PPPoE discovery and
  session (`ifup wan`: a redial completes), LCP echo (`logread | grep
  pppd`), `ping -s 1472` both ways, an 8-stream download and upload with
  fixed cports while `/proc/interrupts` (EDMA Rx IRQs) and `/proc/stat`
  are read before and after: the spread matches the offload-off baseline
  taken just before.
- `echo 0 > .../flow_offload`: `disabled: 0 flow(s)`, `offload_flows` shows
  `degraded 0`. Enable and disable once more.

### 5.2 R2, HS1: a valid session entry and the rest of the session

Arm `r2a` (1200 s) and the dead-man. fw4 hardware offload still off,
offload on (R1); then the owner accepts the VLAN residual for the rig:
`echo 1 > /sys/module/qcom_ppe/parameters/pppoe_offload`.

First the session entry alone: `echo "pppoe-wan 600" >
/sys/kernel/debug/ppe/offload_pppoe_pin`. Expect `PPPoE session <S> from
<AC> (port 2, VLAN 835, MTU 1500) in hardware: session entry 0, L3
interface 16` and `pinned in hardware for 600s`. Run the checks below for 5
minutes, then `echo - > .../offload_pppoe_pin`.

Then with a flow: arm `r2b` (1200 s), `uci set
firewall.@defaults[0].flow_offloading_hw=1; fw4 reload` (no commit), start
one download from 10.64.0.1 with a fixed port; the session comes back with
the flow. Run the checks for 5 minutes, then again after `ifup wan`
(redial) with the next session.

Before any check or measurement in R2 to R5 taken with offload on,
`offload_flows` shows `pppoe_blocked 0` and the session `live 1`; a result
taken otherwise is void and taken again. The steps that turn offload off or
take the session out on purpose (R4's filter self-check, software baseline
and watch; R5's disable, `fw4 reload`, session loss and breaker) are exempt
while they run; after each, `flow_offload` goes back to 1 (0 then 1 when
`pppoe_blocked` reads 1), the download is re-offloaded, and `offload_flows`
is checked again before the next offload-on step.

The checks:
- On the router, `timeout 70 tcpdump -e -n -i wan -c 100 'not port <flow
  port>'` shows every inbound frame as `vlan 835 ... PPPoE [ses S]`;
  `timeout 70 tcpdump -n -i wan.835 -c 20 'ip or ip6'` captures nothing;
  `timeout 70 tcpdump -n -i pppoe-wan -c 20` shows the SYN-ACKs and ping
  replies below. No `IP frame from` line.
- pppd: no echo failure, `/proc/net/pppoe` session unchanged.
- From geekom: 20 new TCP connections to 10.64.0.1 complete; a UDP stream
  (iperf3 -u, 30 s) runs.
- From the rig PC: a ping to the router's PPPoE address, a TCP SYN to an
  open and a closed port of it (answers as without offload), a WireGuard
  handshake, DNS through the router.
- A FIN, then an RST, of the offloaded flow reach conntrack (the flow leaves
  `offload_flows`); a TTL=1 packet and a fragment with the flow's 5-tuple
  reach the router's `wan` capture and not geekom.
- HD, best run after R3's HP0: nothing is sent on the offloaded download,
  either way, for 6 to 20 s before each hand-built frame (the first
  statistics read comes 3-4 s after the last software packet; the 30 s TCP
  flowtable timeout stays clear), and `offload_flows` shows the flow's
  `idle` above 1500 ms just before sending. First a control: one frame of
  session S from the concentrator's MAC on VLAN 835, PPPoE code 0x00,
  carrying the flow's 5-tuple as a pure ACK with IP ID 0x5eed (section 4:
  unicast to the WAN MAC). It reaches geekom and does not appear in the
  router's `timeout 70 tcpdump -Q in -e -n -v -i wan -c 5 'vlan 835 and
  pppoes and tcp port <cport> and ip[4:2] = 0x5eed and ip[8] = 200'`
  (hardware forwarded it); until it passes, HD counts for nothing. Wait 6 s
  again, read `rearmed_deacce`, `rearmed_invalid` and `rearmed_superseded`,
  and send the same frame with the code 0x01 (exception 2, de-accelerating).
  Software does not check the PPPoE code: the fast path forwards it to
  geekom and re-offers the flow, and the driver re-reads the entry. HD holds
  if within 2 s `rearmed_deacce` rose by exactly 1 and the other two did
  not, the frame is in the capture and at geekom, and `offload_flows_hw`
  shows the flow `valid 1 de_acce 0` (the new entry). `rearmed_deacce` flat:
  read `offload_flows_hw` before judging. `de_acce 1`: HD holds and the
  re-offer was missed; wait 3 s and send one code-0 copy, which then reaches
  the CPU and re-arms the entry. `de_acce 0` with the hit counted (the PPPoE
  exception's code in `cpu_code`, read before and after): HD false. No hit
  counted: the frame missed, HD untested. `rearmed_invalid` up instead:
  hardware cleared VALID; record it. After R4 has recorded the residual, the
  same frame untagged also reads `de_acce 1` while idle (software misses
  it). (A FIN closes the flow in software, and a SYN only redirects.)

Any failure: stop. PPPoE offload is never enabled on the live line.

### 5.3 R3, HP0, HE2, HB, HV1, HM: one offloaded download

With R2's state:

- HP0, absent from the CPU path: read `ethtool -S wan | grep -w
  rx_packets` (the frames EDMA handed to the CPU) and `cat
  /sys/kernel/debug/ppe/cpu_code` at idle; run a 1 GB download with a fixed
  port; read them again after it ends. The `wan` EDMA delta is under 1% of
  the packets geekom counted; no new counts for CPU codes 79, 80, 82, 83, 88,
  89, 91, 93, 94 (L2/L3 MRU and MTU, de-acceleration, source interface, flow
  MTU); `offload_flows` shows the flow's packets. CPU idle (`/proc/stat`)
  well above the software baseline. Separately, on a rate-limited download
  (`iperf3 -R -b 50M`), `timeout 70 tcpdump -Q out -n -e -i wan -c 5 'vlan
  835 and pppoes and tcp port <cport>'` shows the router's own ACKs (the
  filter matches), and the same with `-Q in -c 50` shows no data segment
  for the whole 70 s. On `wan` the 835 tag sits in metadata inbound and
  inline outbound, so every filter on PPPoE or IP fields there starts with
  `vlan 835 and pppoes`.
- HE2: geekom's capture: frames tagged VLAN 1, source MAC br-lan's, no 835,
  0x8899 or 0x88a8 tag. No frame of the flow on lan2, lan3, lan4 (captures
  on hosts there, or the switch's port counters after the run).
- HB: with br-lan's own MAC, `offload_flows` shows `smac 0: <br-lan MAC> refs
  1 l3_if 24` and the flow `smac 0`, dmesg `egress L3 interface 24 rewrites
  the source MAC to <br-lan MAC>`, the capture's source MAC is br-lan's and
  CPU codes 82/91 stay flat. If HE2, HP0 or HM fails, repeat once with
  `ip link set br-lan address $(cat /sys/class/net/lan/address)` (as R6
  does; the flow shows `smac -1`) to tell HB apart, and restore br-lan's
  address after.
- HE2b: the PC sends the download's 835 frames with PCP 3, then PCP 5
  (`egress-qos-map` plus a socket priority), then DEI 1 (scapy): each case
  as HE2, and the RTL8372N CPU-port drop counters stay flat.
- HV1: a client behind lan4 (trunk, VLAN 1 tagged) gets its download tagged
  VLAN 1. If a port can be made an untagged PVID-1 member for the rig
  (`bridge vlan` on lan2), its client gets untagged frames; add a second
  untagged VLAN to it and its flows are declined (`another untagged vlan`).
  The rtl8372n driver refuses a VLAN upper on a port of the offloaded
  bridge, so save `bridge vlan show dev lan4` first, then take lan4 out: `ip
  link set lan4 nomaster; ip link add link lan4 name lan4.100 type vlan id
  100; ip link set lan4 master br-lan` (lan4 is now bridged in software):
  flows towards lan4 are declined (`bridge not offloaded by the switch`).
  Undo: `ip link set lan4 nomaster; ip link del lan4.100; ip link set lan4
  master br-lan`, then put back lan4's bridge VLANs as saved (tagged 1, 10,
  20 on 2026-10-22: `bridge vlan del dev lan4 vid 1; bridge vlan add dev
  lan4 vid 1; bridge vlan add dev lan4 vid 10; bridge vlan add dev lan4 vid
  20`), and check `bridge vlan show dev lan4` against the saved output;
  netifd does not re-apply them.
- HM: a full-size TCP download (MSS 1460) and a UDP download of 1472-byte
  datagrams (`iperf3 -c 10.64.0.1 -u -R -l 1472 -b 200M`, an offloaded flow;
  ICMP is never offloaded) reach geekom whole; the CPU codes of HP0 are read
  before and after.

### 5.4 R4, HP1: what must not hit

The download is rate-limited (`iperf3 -R -b 50M`), so that the router's
captures are allowed (1.4). The download's own segments carry every IP ID in
turn, so a hand-built frame is told apart by two fields: TTL 200 (199 at
geekom, after the hop; the download arrives with 63) and its own IP ID (one
range per case, listed in the bench record; the control 0x5eed). tcpdump
prints both with `-v` (`ttl`, `id N`); for a frame it does not decode as
IPv4 (IHL < 5, a short PPPoE length, another PPP protocol, PFC) they are
bytes 8 and 4-5 of the IP header in the `-x` dump. geekom's captures, the
baseline's and the offload-on run's alike, are read with the filter `ip src
10.64.0.1 and ip[8] >= 190 and ip[8] <= 200` (on its VLAN 1 interface)
before any frame is matched: it drops the download (TTL 63) and LAN traffic
sent with TTL 255, such as mDNS. All the cases are sent from one scapy
script, started after the captures.

First a control, built exactly like the cases below: one frame of session S
from the concentrator's MAC on VLAN 835, unicast to the WAN MAC (section 4),
carrying the flow's 5-tuple. It reaches geekom and does not appear in the
router's `timeout 70 tcpdump -Q in -e -n -v -i wan -c 5 'vlan 835 and pppoes
and tcp port <cport> and ip[4:2] = 0x5eed and ip[8] = 200'` (hardware
forwarded it). With `flow_offload` at 0 (the download going on in software)
the same capture prints the control (`id 24301`): the filter matches. Until
the control passes, the cases below count for nothing. Each case then
changes one field of the control; the 0x88a8-first case carries S-VID 1 (an
S-VID of 0 is stripped by the core and the frame then forwarded through
wan.835).

Then a software baseline: with `flow_offload` at 0 and fw4's flowtable on
(the flow in the software flowtable), the PC sends every case below, 10
frames each, while geekom captures (`-w`, read with `-e -n -v -x`), and the
set of frames that reach geekom is recorded. Expected through the slow path
(conntrack's reply tuple has no interface, fw4 accepts established flows
first): plain IPv4 untagged and on VLAN 835, and Record Route; through the
flowtable fast path on `wan`, which keys on the VLAN ID and session ID only
and reads neither the source MAC, the PPPoE length nor the IPv4 checksum: S
from another source MAC, a short PPPoE length, a bad header checksum (it
arrives still bad, NAT updating it incrementally) and a truncated TCP
header built by lowering the total length; PFC either way. The recorded
set, not this prediction, is the baseline: a difference from it is recorded
and does not stop the run. Then `flow_offload` back to 1, the download
re-offloaded, and the control passes again.

With offload on, the router captures to a file, `timeout 300 tcpdump -Q in
-e -n -i wan -c 2000 -w /tmp/r4.pcap` (still running after the last frame,
or the run is repeated), geekom captures as in the baseline, and the PC
sends every case again, 10 frames each, carrying the flow's 5-tuple:

- plain IPv4 untagged, and on VLAN 835 without PPPoE, from a source MAC
  that is not the concentrator's;
- PPPoE with another session ID; with S from another source MAC;
- S from the concentrator's MAC on VLAN 836 and untagged: **expected to
  hit** (the residual, DESIGN.md section 6); record the result;
- a 0x88a8-first (S-VID 1) and a 0x8899-first frame before the 835 tag;
- a PPP protocol other than IPv4, a short PPPoE length, PFC;
- IPv4 with an option (Record Route), IHL < 5, a bad header checksum, a
  truncated TCP header.

Pass, reading `tcpdump -r /tmp/r4.pcap -e -n -v -x` (then `rm` it): every
frame of every case but the residual appears in the router's inbound capture
(it reached the CPU), and every frame at geekom that passes that filter,
other than the residual's, also appears there (matched by TTL and ID), so
that geekom gets exactly the baseline set plus the residual. A frame at
geekom missing from the router's capture is a hardware hit: HP1 fails. CPU
code 89 (source interface check) may count.

Then the watch: one plain IPv4 frame on VLAN 835 from the concentrator's MAC
to the router's WAN MAC. Expect `IP frame from <AC> on the lower device of
PPPoE session` and the session out of hardware; `echo 0`, then `echo 1` to
`flow_offload` clears it. `dmesg -c` afterwards, so that the deliberate
stop-line message is not read again.

### 5.5 R5, lifecycle

- Disable under load: `disabled: N flow(s)`, the download goes on in
  software without a stall over a second, `degraded 0`.
- `fw4 reload` during a download: `flowtable unbound: N flow(s)`,
  `offload_flows` empty, the download re-offloaded after the reload.
- `bridge vlan add dev lan1 vid 30` during a download: `bridge change`, back
  in hardware within seconds; delete VID 30.
- Client move: re-cabling geekom from lan1 to lan4 during a download takes
  lan1's link down: the flowtable tears the flow down on carrier loss
  (pending 709) and the driver logs `bridge change: N port-addressed
  flow(s) back to software`. Once geekom is up on lan4 the download
  resumes: back in hardware towards lan4 (`offload_flows` names lan4's
  port) if the flow is offered after the bridge learned geekom there,
  otherwise in software for the rest of the connection. No `moved to`
  line is expected; pass is no stall beyond the link-up time. The FDB
  follower itself needs a move with no link or STP change on either
  router port (geekom behind an unmanaged switch on lan1 moved to one
  already up on lan4): only then `moved to lan4: N flow(s) re-pointed`.
  To Wi-Fi: the flow continues in software.
- Session loss: `ifdown wan; sleep 5; ifup wan`: `PPPoE session <S> out of
  hardware (device down): N flow(s) back to software`, then a new session.
  Twice within a minute of each session going into hardware: `PPPoE stays
  in software` (the breaker); `echo 0`, then `echo 1` to `flow_offload`
  clears it; `dmesg -c` afterwards.

### 5.6 R6, profile A (H0, HP2)

Dead-man armed, operator on Wi-Fi. `echo "A 120" >
/sys/kernel/debug/ppe/offload_profile`: `profile A on for PPE port 1`.

- H0: a download through the tagged WAN is declined and runs in software;
  the WAN's Rx spread collapses (expected).
- HP2: profile A declines tagged ingress, and lan1 carries VLAN 1 tagged, so
  the upload comes from a client on an untagged PVID-1 port set up for the
  rig (`bridge vlan` on lan2, as in HV1). The conduit's L3 interface routes
  only frames addressed to the conduit's MAC, and the clients address
  br-lan's, which differs here: for this step `ip link set br-lan address
  $(cat /sys/class/net/lan/address)` (put br-lan's own back afterwards);
  otherwise the upload is declined (`clients address ..., which no MY_MAC
  entry holds`). The upload is offloaded and absent from the CPU path
  (`ethtool -S lan | grep -w rx_packets` at idle before and after: a small
  fraction of the upload's packets); the PC captures `8100 0343 8864 1100
  <S> <len> 0021`, a correct length, the router's WAN MAC as source, the
  PC's as destination; full-size frames pass.
- LAN ARP and DHCP from geekom are answered within the window. While A
  holds, the router's own 802.1Q-tagged transmits go without checksum or
  segmentation offload (expected; no error, no stop line); a few tagged
  frames with a bad checksum right at the switch-over are expected too.
- `echo E > .../offload_profile`, or the timer: `profile A off`.

### 5.7 R7, throughput

8-stream and single-stream downloads at line rate for 5 minutes with fixed
cports and the recorded governor: throughput, per-CPU idle, no stop line,
`offload_flows` occupancy. Uploads in software, unchanged.

## 6. Live phases (only after section 5 passed)

### 6.1 L1, E alone

At a quiet time, arm `l1` (1200 s) and the dead-man, fw4 hardware offload
off, then enable: R1's checks for 10 minutes, then disable.

### 6.2 L2, first live session

Arm `l2` (1200 s) and the dead-man. fw4 hardware offload on (no commit),
enable, `echo 1 > /sys/module/qcom_ppe/parameters/pppoe_offload` (the
owner's acceptance of the VLAN residual), one download with a fixed port.
R2's router-side checks (`wan`, `wan.835`, `pppoe-wan` captures, pppd, a new
connection, a ping to the PPPoE address from a phone on cellular, a
WireGuard handshake from it, the 443 port forward). Disarm only when all
pass.

### 6.3 L3, household traffic

Arm `l3a` (90 min), renewed as section 1.2 says. Normal use for an hour:
R3's HE2 captures once, the stop lines every 10 minutes, `offload_flows`
counters.

### 6.4 L4, acceptance

Single-stream and 8-stream downloads at line rate with fixed cports
(Cloudflare recipe), recorded governor. Accept when both reach line rate
with every core's busy time (100 - idle in `/proc/stat`) under 50%, against
about 90% on one core in software, and with no stop line.

### 6.5 Soak, then keep

24 hours non-persistent, the rollback renewed (`l5a`, `l5b`, ...) as
section 1.2 says. Then `options qcom_ppe flow_offload=1 pppoe_offload=1` in
`/etc/modules.conf` and `uci commit firewall` with `flow_offloading_hw=1`;
after the next reboot, L1's checks once more.

## 7. What to capture

`dmesg`, `logread`, `cat /sys/kernel/debug/ppe/offload_flows`,
`offload_profile`, `nft list flowtables`, `conntrack -L | head`, the tcpdump
files, `/proc/stat`, `/proc/softirqs`, `/proc/interrupts` before and after,
the idle-time counter reads, and after a crash `/sys/fs/pstore/*`.

## 8. Known limitations

- Downloads only: the parser cannot see past the head tag on uploads
  without profile A, which breaks the tagged WAN.
- The PPPoE session match has no VLAN: a frame with the session ID and the
  concentrator's MAC on another VLAN of the WAN port reaches the flow (5.4).
  Nor has the flow key: a plain-IP flow from the WAN port would hit untagged
  or on any VLAN of it (not this line's case; DESIGN.md section 10).
- All LAN ports and VLANs share the conduit's L3 interface; VLAN-aware
  egress is VLAN 1 only.
- IPv6, Wi-Fi clients and VLANs 10/20 stay in software.
