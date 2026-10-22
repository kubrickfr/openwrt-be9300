# PPE flow offload for PPPoE on the GL-BE9300: design

Branch `be9300-next-ppe-pppoe`, on top of `be9300-next-rtl8372n`, kernel
6.18. Nothing in it has run on hardware. Evidence labels: **[src]** read from
a public source (PROVENANCE.md has path:line), **[meas-P]** measured by Kamil
Bienkiewicz (perceival) on a GL-BE9300 and recorded in his patches,
**[meas-U]** measured on this gateway, **(I)** inference, **(H)** hypothesis
with a bench gate (section 9, TESTING.md).

## 0. Goal and scope

Goal: CPU headroom at line rate on a PPPoE-over-VLAN-835 WAN (2.2/0.8
Gbit/s). Today a single-stream download runs at 2.0-2.16 Gbit/s with one core
about 90% busy in the software flowtable [meas-U]; the upload is not limited
by the router [meas-U]. The download is the target.

In scope: IPv4 NAT flows that the nftables flowtable offers with `flags
offload`; PPPoE over one 802.1Q tag on the PPE WAN port; LAN egress through
the RTL8372N (rtl8_4 tag), including a VLAN-aware br-lan.

Declined, staying in software: IPv6 (perceival's IPv6 path is not ported),
Wi-Fi (not a PPE port), hairpin NAT and plain routing, multicast, a second
DSA conduit, tag_8021q, and every upload (section 1). Changing the
RTL8372N's CPU tag EtherType or using the IPQ5332 athtag stripper would make
uploads possible, but changes the tagging scheme and needs the owner's
go-ahead first.

## 1. What each direction needs

```
ONT -- PPE port 2 "wan" -- wan.835 (802.1Q 835, MTU 1508) -- pppoe-wan (session S, AC MAC A, MTU 1500)
LAN -- RTL8372N lan1..lan4 == rtl8_4 head tag == PPE port 1 (conduit, MTU 1508)
br-lan VLAN-aware: lan1, lan4 tagged in VLAN 1 (lan4 a trunk 1/10/20); lan2, lan3 VLANs 10/20; br-lan MAC != conduit MAC
```

| | Download (reply, DNAT) WAN->LAN | Upload (original, SNAT) LAN->WAN |
| --- | --- | --- |
| PPE ingress | port 2: `8100/835 8864 PPPoE 0021 IPv4` | port 1: `8899 04RR W2 W3 0800 IPv4` |
| Parsed with reset TPIDs | the tag yes [meas-P: 2347 Mbit/s on an IPoE VLAN WAN]; the PPPoE step (H, HP0) | no: 0x8899 is not a TPID [src; meas-P 74.6% idle] |
| Parsed with perceival's ingress alias (S 0x8899 / C 0x0000) | no [meas-P: tagged-WAN flows "never hit"] | yes [meas-P 98.1% idle] |
| Flow key | 5-tuple + SRC_L3_IF = the session's L3 interface | 5-tuple + SRC_L3_IF = conduit's |
| Egress | nexthop S+C tag inserts emulate the head tag [meas-P 2353 Mbit/s] | nexthop C-tag 835 + EG_L3_IF PPPoE encap (H, HP2) |

The two directions need conflicting chip-wide ingress TPIDs. **Decision D1:**
service uses profile **E** (egress only): EG_VLAN_TPID S-TPID 0x8899 (C-TPID
stays 0x8100) and the conduit an S-VLAN egress port. No ingress TPID or port
role changes, so WAN and LAN RSS and Rx checksum stay as they are. Downloads
are offloaded; uploads stay in software. The upload code exists, reachable
only under bench profile **A** (perceival's full alias, section 8), to check
H0 on this gateway and to exercise the PPPoE encapsulation.

Why E leaves the software path alone (I, gate HE1): CPU transmits use EDMA
service code 1, whose egress bypass covers VLAN translation, tag format
control and L2/L3 edit [src: `ppe_servcode_init()`]; the PPE bridges nothing
between ports; port 0's egress mode is not touched; an S-tag is only emitted
where a nexthop inserts one.

## 2. Commits

| # | OpenWrt commit | Kernel patch | Author |
| --- | --- | --- | --- |
| 1 | kernel: flowtable: match the PPPoE session in hardware offload rules | generic `pending-6.18/714` | François Guerraz |
| 2 | qualcommbe: ppe: add Kamil Bienkiewicz's IPv4 flow offload, off by default | `0433` tables, `0434` engine (both Kamil Bienkiewicz, ported), `0435` safety | per patch |
| 3 | qualcommbe: ppe: emulate the rtl8_4 head tag with egress-only TPIDs | `0436` | François Guerraz, from Kamil's 0445/0449 egress half |
| 4 | qualcommbe: ppe: offload towards ports of a VLAN-aware bridge | `0437` | François Guerraz |
| 5 | qualcommbe: ppe: offload PPPoE downloads | `0438` | François Guerraz |
| 6 | qualcommbe: ppe: PPPoE upload encap behind a bench-only ingress alias | `0439` | François Guerraz, alias from Kamil's 0445 |
| 7 | qualcommbe: ppe: let flows into the switch leave with their bridge's MAC | `0441` | François Guerraz |
| 8 | qualcommbe: document the PPE flow offload | DESIGN, PROVENANCE, TESTING | François Guerraz |

`0440` is taken by an unrelated ath12k patch. Every commit builds with `W=1
-Wmaybe-uninitialized` without a new warning (and with `NET_DSA=n`), and
every image is safe to flash: nothing is offloaded until the module
parameter is set, and profile A never applies itself.

`0433`/`0434` are Kamil's code from perceival/openwrt-flint3 (his patches
0411-0456 at `2365932733`) ported onto this tree's EDMA base: IPv6, the
tag_8021q encapsulation and the ingress alias are left out; his parser/TPID
register block moves to `0436`/`0439`, with the egress tag-mode constants
renamed to the register encoding (0 untagged, 1 tagged, 2 unmodified, 3
untouched [src]); the values he measured (2 on the CPU port) are kept. The
exception registers come with `0434`, which programs them. `0434` is inert:
nothing calls it until `0435` wires it in, and paths of his it keeps that
`0435` replaces (the re-arm and flap drop releasing what a still-valid flow
entry points at) are named in its message.

## 3. Safety (0435)

**Off by default, two layers.** fw4's `flow_offloading_hw` and the module
parameter `qcom_ppe.flow_offload` both default to off. With the parameter
off the driver still binds the flowtable's block (a ruleset with `flags
offload` always loads), declines every flow, and writes no offload register:
no L3 interface, MY_MAC, L3_VP_PORT, exception, TPID, MTU command, PPPoE or
flow table write. perceival programmed the per-port L3 interfaces and the
exceptions at probe; here that happens at enable.

**Enable** (`/sys/module/qcom_ppe/parameters/flow_offload` or `options
qcom_ppe flow_offload=1`; under RTNL, then the driver lock): refuse when a
previous disable left state behind (`degraded`) or when L3_ROUTE_CTRL does
not redirect routing failures (MRU, MTU, flow source interface, PPPoE
multicast, flow PMTU) to the CPU; save every register it is about to change;
program the exceptions, each EDMA port's L3 interface, MY_MAC and L3_VP_PORT
entries and its MTU command (below), then profile E if an rtl8_4 conduit
exists; read everything back. A port that does not read back restores
everything and leaves offload off. An exception group that does not read
back leaves offload on with that group's flows declined, and profile E
failing restores its two registers and leaves flows into the switch
declined. The enable line says which: `ppe_offload: enabled: TCP exceptions
ok, header exceptions ok, PPPoE exceptions ok, profile E ok (port 1)`.

**Disable**: decline from now on, delete every flow entry the driver owns
(each delete read back), let go of the sessions and nexthop/host/public IP
entries, and only then write the saved registers back, in reverse order, the
bits another user owns (a port's MTU) left as they are now.

**Degraded.** A flow entry that cannot be deleted (on any path: disable, a
stale entry, a flowtable going away, a bridge change, a session teardown, a
DESTROY) may still forward. It stays tracked with everything it points at;
`degraded` is set; from then on every new flow is declined, and a later
disable keeps the exceptions, L3 interfaces and profile E the entry relies
on, and enable is refused until a reboot (the PPE is reset at probe). A
PPPoE session entry that cannot be cleared, and a saved register that does
not read back as saved when written back, set it too, so a later enable can
never save a changed value as the one to go back to. So does an add that
failed but may have landed (`ppe_flow_entry_add()` returns -EUCLEAN: its
counter clear and the delete after it both failed, or its result never
came). The tracking entry is allocated before the add, so such an entry, or
one whose delete failed after a failed insert, is tracked like any stuck
one; an entry at an index hardware never returned cannot be deleted at all
and keeps disable from writing anything back. Each stuck entry is counted
and logged once; a re-offer no longer retries its delete (a DESTROY, an
unbind and disable do). A source MAC slot (section 7) that does not read
back is only retired until the next enable: no flow points at it, its
ingress entry routes nothing, and disable writes its saved value back.

**Port MTU command.** While offload is on, MRU_MTU_CTRL[port].MTU_CMD of
every EDMA port is redirect-to-CPU instead of the PPE setup's drop (a
masked save and restore: the MTU bits stay the port's, which MTU changes
rewrite under RTNL as enabling does). What the PPE inserts itself (a VLAN
tag, a switch's head tag) can take a full-size frame past the port's MTU
at zero margin, and a drop there would come after the flow hit, out of
sight of every check.

**Exceptions**, armed on a flow hit only (miss and generic enables cleared,
so traffic that was never offloaded is not affected), action redirect to the
CPU:
- TCP FIN and RST (codes 46, 48 via the flag parser): de-accelerate, as
  the software fast path closes the flow on them [meas-P]. SYN (47):
  redirect only, which is what the software fast path does with it on a
  live flow (it tears down only a closing one, whose entry its FIN or RST
  de-accelerated already); a de-accelerating SYN would let any peer that
  knows a tuple push the flow out of hardware at will.
- Header codes, without de-acceleration: unknown L2 (0), IPv4 small IHL (5),
  options (6), header incomplete (7), bad total length (8), data incomplete
  (9), fragment (10), small TTL (12), header checksum (14), TCP header
  incomplete (42), TCP small data offset (45), UDP and UDP-Lite header
  incomplete (55, 61). The software fast path punts options, fragments,
  TTL <= 1 and headers it cannot pull (`nf_flow_table_ip.c`) and leaves the
  rest to the slow path's checks; the hardware must not translate them
  instead. The vendor de-accelerates half of these; here only the packet goes
  to software, as in the software fast path, so a remote peer cannot push a
  flow out of hardware with one crafted packet. The same holds for the L3
  interfaces' TTL-exceeded action, now redirect without de-acceleration.
- PPPoE codes 1-3 (section 6): de-accelerate, as the vendor; only the access
  concentrator can send them.
IPv4 is declined unless every header exception read back; TCP unless the
flag exceptions did; PPPoE unless codes 1-3 did.

**Fail closed on match keys.** A rule with a dissector key the driver does
not interpret is declined. (The flowtable fills the VLAN keys without setting
them in `used_keys`; the driver finds them by their offset, as perceival
measured.)

**Stale entries.** The flowtable re-offers a flow about once a second while
its packets cross the software path. For a flow already in hardware the
driver reads the entry's first two words back, at most once per HZ/2: the
entry is stale when it is superseded, no longer VALID, or DE_ACCE (word 1 bit
28, set by hardware when a de-accelerating exception fires [src]). A stale
entry is deleted (read back) and the flow programmed afresh. Hit counters
are not used: an idle direction does not count, and a hit that hardware
redirects to the CPU counts as one. An entry that is valid but never hits
(gate HP0 failing) is therefore left alone: the software path forwards its
packets, nothing churns. More than 256 entries invalidated by hardware
itself (neither deleted by the driver nor de-accelerated by an exception)
in 10 s trip the driver: it declines everything and turns offload off as
writing 0 to the parameter would (`ppe_offload: ... turning offload off`),
unless offload was disabled and enabled again before the work ran.
De-accelerated and superseded entries are re-armed without counting:
hardware did what it was told, and nothing a remote peer sends must be
able to turn offload off for the whole router.

**hw-tc-offload.** With `ethtool -K <port> hw-tc-offload off` no new flow
is accepted through that port's callback; DESTROY and STATS still reach
the flows already in hardware.

**Fail closed on re-offers.** When a re-offer of a flow already in hardware
is declined (anything the flow relied on no longer holds), its entry is
deleted too. A re-offer that raced a device or bridge change (-EAGAIN) is
left alone: the handler of that change takes out of hardware what it
concerns.

**Flowtable deletion.** Deleting a flowtable (`fw4 reload`) unbinds the
block callbacks before the flowtable sends its flows' DESTROY, to nobody
(`nft_unregister_flowtable_net_hooks()`, then `nf_flow_table_free()`). Each
of the driver's callbacks therefore carries its flow block; each flow
records the block it came through; when the driver's last callback on a
block is released (the release callback runs on every path that frees one),
every flow of that block is deleted from hardware. A device leaving a
flowtable that keeps others is not this case: its flows are destroyed
through the callbacks that remain. Only the ports' own callbacks are bound
(DSA user ports bind through their conduit): Kamil's indirect binding for
bridges and VLAN devices is dropped, since a flowtable reaches it with
TC_SETUP_FT, which it never handled, fw4 resolves bridges and VLANs to their
ports anyway, and its callback list was shared with the direct path under no
common lock.

**Invariant.** A flow's references (host, nexthop, public IP, PPPoE session,
source MAC slot) are let go of only after its hardware entry is confirmed
deleted, or when its slot was overwritten by another flow; a flow whose
entry cannot be deleted stays tracked, with everything it points at, for the
flush at disable to retry.

**Observability.** `/sys/kernel/debug/ppe/offload_flows` (0400) prints the
driver's own state without reading a register, so it is safe under load:
enabled, degraded, tripped, profile E port, profile A, the PPPoE switch,
breaker and pin, the counters (accepted, declined, re-armed by reason,
withdrawn, unbound, stuck), each session and source MAC slot, and each flow
with its last statistics. `offload_flows_hw` (0400) reads each tracked
flow's first two words (VALID, DE_ACCE): registers, so at idle only. dmesg
has one line per decline reason (perceival's `pr_info_once`), one per
session in and out of hardware, and the counts at disable.

## 4. Profile E: the head tag without the ingress alias (0436)

The RTL8372N expects `8899 04 00 W2 W3` before the EtherType on frames from
the CPU. With the egress S-TPID at 0x8899 and the C-TPID at 0x8100, a
nexthop S-tag insert with TCI 0x0400 and a C-tag insert with TCI BIT(port)
produce `8899 0400 8100 BIT(port)`: protocol 0x04, W2 = 0x8100 (EFID_EN,
KEEP=0, VSEL=0), W3 = the forwarding port mask (ALLOW=0). perceival measured
the switch delivering this to the named port at 2353 Mbit/s once the conduit
is an S-VLAN egress port [meas-P].

Registers: EG_VLAN_TPID (0x20040) STPID := 0x8899, CTPID must read 0x8100;
PORT_EG_VLAN[conduit] (0x20020 + 4 port) VLAN_TYPE := 1, PCP_PROP and
DEI_PROP := 0 (they are 0 at reset: nothing from the ingress 835 tag may be
propagated into W1/W3, where PCP and DEI would be the protocol byte's top
bits, ALLOW and mask bits; gate HE2b). Both are saved, written and read back
under the driver lock.

When: at enable if an EDMA port is already an rtl8_4 conduit; otherwise on
the first flow towards one (the switch driver may probe after this one).
One conduit owns it; a failure restores the two registers and declines flows
towards the switch until disable. E is never left while offload is on:
conduit and DSA events do not touch it. Keeping it is harmless to the
software path (section 1), a tagger change needs the conduit and every user
port down (which tears their flows down), and flows into the switch go with
the bridge changes of section 5. It is left only by disable, after every flow
entry is confirmed deleted. Enabled with fw4's hardware offload off, the
driver sits in "E applied, nothing offloaded": the state gate HE1 checks.

Two declines come with E. A flow entering through an rtl8_4 conduit is
declined: with the reset ingress TPIDs the parser stops at the head tag, so
its entry would never hit (0435 still programmed such entries). A flow
towards a DSA port is accepted only when the switch bridges that port in its
Linux bridge (`dsa_port_bridge_dev_get(dp)` is the master): a port the switch
driver leaves to software bridging (a refused join, a VLAN upper) is
standalone in hardware; the FDB follower applies the same test. The packets
being routed through the bridge, its MTU caps the flow's PMTU.

The emulated tag adds 8 bytes on the conduit at zero margin (1508 + 14 =
1522 = 14 + 8 + 1500); an oversize frame goes to the CPU rather than being
dropped (section 3, port MTU command). Whether the PPE counts the inserted
tags at all is gate HM.

The comment in `edma_port.c` (from 0431) that no TPID register is programmed
now says that the parser's TPIDs are not, and that the egress S-TPID is.

## 5. Ports of a VLAN-aware bridge (0437)

The rtl8372n driver keeps the CPU port VLAN-unaware with PVID 1 and puts the
ports of an offloaded VLAN-aware bridge in ORIGINAL egress mode [src:
`rtl8372n_vlan.c`]. The emulated tag has VSEL=0, KEEP=0: the frame is
classified to VLAN 1 and leaves the named port tagged or untagged as VLAN
1's entry says (H, HV1). The switch exempts the CPU's frames from its
egress membership check, so the driver checks membership itself. A flow
towards a port of a bridge is accepted only when:
- the switch offloads that very bridge for the port (section 4) with the
  bridge's VLAN filtering (`dsa_port_is_vlan_filtering(dp)` equals
  `br_vlan_enabled()`, added here).
  A port the switch driver bridges in software (offload off, a VLAN upper, a
  non-802.1Q bridge at join, a second bridge) is standalone in hardware and
  would send the frame untagged whatever the bridge says; DSA also stops
  tracking its STP state. Declined outright.
- the port is forwarding (`dp->stp_state`, accurate for offloaded ports);
- VLAN-aware: the switch is `realtek,rtl8372n`, the bridge protocol 802.1Q,
  and either
  - no VLAN push, the port's PVID is 1 and VLAN 1 is the only VLAN it
    egresses untagged in (then the flow is in VLAN 1 by construction; the
    bridge exports no walk of a port's VLANs, so VIDs 2-4094 are looked
    up, and the answer is kept per port until the bridge-change generation
    moves), or
  - a push of VID 1 only, to a tagged member of VLAN 1 (lan4's trunk).
  Any other VID, a second tag or an 802.1ad bridge is declined. The MTU of
  the bridge's VLAN 1 upper caps the PMTU, as the bridge's does.

Every flow into the switch is in a hash keyed by destination MAC. A port's
VLAN change, STP state change, the bridge's VLAN filtering or protocol
change (switchdev notifications), and a switch port joining or leaving a
bridge or getting a VLAN upper (NETDEV_PRECHANGEUPPER/CHANGEUPPER) drop the
flows towards that port (every port-addressed flow, for a change to a bridge
itself) twice: synchronously in the notifier (under RTNL, in process
context, before or after the bridge applies the change depending on the
change: after it for VLAN filtering and a deleted VLAN's PVID, and for an
STP state, which the bridge notifies deferred, together with DSA's own port
state update; the software flowtable does not consult STP either), and again
from an ordered workqueue once it is complete; a generation counter makes
flows offered in between retry against the final state. perceival's FDB
follower re-points a flow when the bridge learns its client on another port;
it applies the same conditions to the new port and, in a VLAN-aware bridge,
follows only learns in VID 1. It only acts on a move between two ports that
both stay up with no STP change (a client moved between two switches behind
lan1 and lan4): a move that takes the old port's link down drops the port's
flows first, through the flowtable's carrier-loss teardown (pending 709) and
the bridge change above, and the flowtable offers them afresh.

## 6. PPPoE (0438 download, 0439 upload)

**Opt-in.** PPPoE flows need a second parameter, `qcom_ppe.pppoe_offload`
(default off), on top of `flow_offload`: setting it is the owner accepting
the VLAN residual below. Clearing it takes every session out of hardware.

**Kernel (714).** `nf_flow_rule_match()` adds FLOW_DISSECTOR_KEY_PPPOE
{session ID, PPP protocol, 0x8864} to a tuple whose encapsulation has a
PPPoE session, as cls_flower sets the key. Without the key a PPPoE download
is declined as before (the driver's address check). mtk and airoha ignore
the key and are unchanged; that a driver which does not understand a key
must decline it is their business (this driver does, section 3). The VLAN
key's missing `used_keys` bit is a separate upstream bug, not touched here.

**Resolving the session.** For a download entering EDMA port P (with the
VLAN key V) with PPPoE key S: `__ip_dev_find(net, match_dst)` (the rule's
destination before translation, the router's own address) must be a running
`ARPHRD_PPP` device; its `dev_fill_forward_path()` must be exactly
[PPPOE(S, AC MAC A), VLAN(V) on P, ETHERNET(P)] (or [PPPOE, ETHERNET(P)]
without V), and the VLAN device's MAC must be P's (the MY_MAC entry the
session's L3 interface points at).

**Programming.** At most 4 sessions, refcounted by flows, in session entries
0-3 and L3 interfaces 16-19 (above the ports'):
- IN_L3_IF[l]: MRU = MTU = the PPP MTU, IPv4 unicast route, TTL exceeded
  redirect (no de-acceleration), MAC_BITMAP = P's MY_MAC, PPPOE_EN.
- EG_L3_IF[l]: MAC = P's, SESSION_ID = S, PPPOE_EN.
- PPPOE_SESSION[i] (0x306800 + 8i) = S | BIT(P) << 16 | TYPE 0; EXT1
  (0x306c00 + 4i) = A[47:16]; EXT2 (0x306e00 + 4i) = l << 8 | L3_IF_VALID;
  EXT (0x306a00 + 4i) last = UC_VALID | SMAC_VALID | A[15:0] << 16. Cleared
  EXT first. Every word read back; a mismatch clears the slot and declines.
At enable, a session entry 0-3 that is already valid is someone else's and
offload stays off.

The flow is perceival's DNAT entry with SRC_L3_IF = l. A plain IPv4 frame,
another session, another source MAC or another port does not get l and
cannot hit (H, HP1). Residual (I): the session match has no VLAN field, so
session S from MAC A on another VLAN of P, or untagged, also gets l; it
needs the concentrator's MAC spoofed on the WAN segment, and software would
drop such a frame. Whoever can put arbitrary tags on the router's WAN link
can put that frame on the session's own VLAN as well, where software
forwards it into the same connection: for such a sender the residual changes
which frames are forwarded, not who can get a frame forwarded. A sender
whose frames reach the port only on another VLAN, because upstream equipment
(the ONT, the OLT) confines it there, does gain a path. It still breaks the
rule that hardware forwards nothing software would drop, hence its own
switch. Two sessions in hardware differing only by VLAN are refused. Whether
this residual is acceptable is the owner's decision (section 10).

**Teardown.** A session goes out of hardware when its PPP, VLAN or port
device goes down, loses carrier, changes address, MTU or upper, or goes away
(netdevice notifier, under RTNL), when its last flow goes, when
`pppoe_offload` is cleared, and at disable. The session entry is cleared
first (EXT, then the rest), so that no frame gets the session's L3 interface
and its downloads cannot hit; then its flows are deleted, and only then
(from 0439, whose uploads keep hitting on the conduit's interface and leave
through the session's) its L3 interface is cleared and read back. From 0439,
any flow of the session left stuck, download or upload, retires the slot
until a reboot (enabling does not give it back, unlike a source MAC slot),
even once a later retry deletes it; a stuck upload meanwhile meets an MTU of
0 there and is redirected to the CPU (an L3 MTU failure, `route_ctrl_ok`;
that this entry's MTU is the one checked is HB). At 0438 a stuck download
only keeps the slot reserved until its delete succeeds. Otherwise the slot
is reused once its last flow is gone. An entry that cannot be cleared
marks the slot broken for good and sets `degraded`. Events on other devices
are ignored. A generation counter, bumped before the sessions are looked at,
makes a flow resolved against the old devices retry instead of programming a
stale session.

**What a valid session entry does to the rest of the session.** The entry
terminates every unicast frame of S from A on P, not only those that hit a
flow: LCP, IPCP, the router's own traffic, the first packets of every new
connection. No source says how such a frame reaches the CPU when it misses
the flow table (H, HS1). The vendor keeps its session entry valid for the
whole life of the link, which suggests the CPU gets it intact (I). Backstops
in service:
- a watch on the session's lower device (`wan.835`) for IPv4 or IPv6 frames
  from A addressed to this host, which would be session frames handed over
  decapsulated; one such frame takes the session out of hardware and
  declines PPPoE until offload is disabled and enabled again (a per-device
  packet handler counting only frames still on that device, as the core also
  hands it the frames of the device's uppers: normally none; A's own
  multicast, such as router solicitations or MLD, does not count). A session
  is not used unless the watch is in place. It is a backstop, not a proof: a
  decapsulated frame that arrives elsewhere is not seen, and a genuine
  unicast IP frame from A on that device (an ISP running IPoE on the same
  VLAN) trips it, which only costs PPPoE offload;
- a flap breaker: two sessions in a row whose PPP device goes down or away
  within 60 s of going into hardware decline PPPoE until offload is disabled
  and enabled again (a carrier blip, an address or MTU change or clearing
  `pppoe_offload` take a session out of hardware without counting; a session
  programmed again after such a change, once its link is back, counts if it
  then dies: the breaker errs towards keeping PPPoE in software). No session
  goes into hardware while its port or lower device has no carrier, or the
  next offer would undo a no-carrier teardown.

The first session entry is written on the fake-ISP rig, never first on the
live line (TESTING.md). For that bench, `offload_pppoe_pin` (debugfs, 0600)
programs the session of a named PPP device without any flow, for at most
600 s: what a session entry alone does to the session's frames. The pin is
held to the device generation check an offer makes, and goes when its
session is taken out of hardware.

**Exceptions 1-3** (PPPoE wrong version/type, wrong code, PPP protocol other
than IP) redirect and de-accelerate on a flow hit, as the vendor programs
them.

**Upload (0439, profile A only).** FLOW_ACTION_PPPOE_PUSH S plus VLAN_PUSH V
towards P, SNAT to the PPP device's address; same resolver; the destination
MAC must be A; nexthop POST_L3_IF = l, port P, C-tag V, PMTU at most the PPP
MTU. The encapsulation is EG_L3_IF's (H, HP2). Declined in code unless the
flow enters through the conduit under A, whatever the board's ports (boards
with plain EDMA LAN ports, such as the kiwi-dvk, never get it).

**MTU (I).** Port MTU/MRU are the netdev MTU + 14 and the vendor says the PPE
ignores VLAN tags at L2 [src]. A full-size PPPoE frame is 14 + 8 + 1500 =
1522 bytes without the tag: exactly `wan`'s 1508 + 14. Over-MRU ingress
already goes to the CPU; over-MTU egress does while offload is on (section
4). Gate HM sends full-size frames and reads the drop/CPU-code counters
afterwards.

## 7. Flows into the switch with the bridge's MAC (0441)

br-lan's MAC differs from the conduit's on this gateway, and a flow into the
switch leaves with the bridge's MAC as its source (the flowtable's Ethernet
mangle). The egress L3 interface carries the source MAC, so up to 4 such
MACs get their own L3 interfaces 24-27 (EG MAC = the bridge's, IN entry with
the port's MTU, not routed). Accepted only for a DSA egress whose bridge MAC
is the mangle's source. Saved and restored like the others. A slot whose
set-up or clearing does not read back is retired until the next enable.
That such an interface (bound to no port, MAC_BITMAP 0, routing off; the
vendor always sets routing) gives a nexthop its source MAC, with its MTU
the one checked, is hypothesis HB.

## 8. Profile A (bench only, 0439)

perceival's full ingress alias, on top of E: IPR and TPR VLAN_TPID =
0x88990000 (S 0x8899, C 0x0000); PORT_EG_VLAN[0] (CPU port) VLAN_TYPE,
CTAG_MODE = STAG_MODE = 2 (unmodified), VSI tag mode off, by masked update
(TX counting and PCP/DEI kept); PORT_PARSING[conduit] role core, last. His
write order; restored in reverse. Without the CPU-port write the flood path
to the CPU rebuilds a lone 0x8100 tag and the LAN dies [meas-P].

Applied only by writing `A <seconds>` (at most 600) to
`/sys/kernel/debug/ppe/offload_profile` (0600) with offload on and E
applied; `E` reverts it, as do the timer, disable and unload. Every flow is
deleted when A comes and when it goes, and A comes or goes only when all of
them were: an entry stuck in hardware keeps the profile it was programmed
under, with `degraded` set. Offers check under the lock that the profile did
not change since their parsing was judged. Under A, tagged ingress (the
WAN's downloads) is declined: the C-TPID 0x0000 cannot parse 802.1Q (H0,
expected false [meas-P]). An upload is declined too unless the MAC its
client addresses (the port's bridge, or the port) is the conduit's: the
conduit's L3 interface routes only frames to its own MY_MAC entry. While A
has the parser's TPIDs (also when a failed revert keeps them), the EDMA
transmit path stops offloading checksums and segmentation for 802.1Q-tagged
frames (`ppe_offload_tpids_moved()`), as no 802.1Q tag is followed; frames
already in a Tx ring, or requeued after a full ring, may still leave with a
bad checksum at the switch-over. A is refused while `degraded`, a failed
restore having possibly left its values in place. A exists
to confirm H0 on this gateway and to exercise the upload encapsulation
(HP2). Never in service: the WAN would lose RSS and Rx checksum, and the
time limit is a backstop, not a guarantee (TESTING.md arms an independent
dead-man).

## 9. Hypotheses and gates

Procedures, stop lines and rollbacks are in TESTING.md; the order is fixed:
rig first, live line last.

| Id | Hypothesis | Where | If it fails |
| --- | --- | --- | --- |
| HE1 | E (and the port MTU command) alone change nothing for the software path: ARP, DHCP, PPPoE discovery and session, full-size frames, Rx spread | rig, offload on with fw4 HW offload off | disable; DSA egress not used |
| HE2 | The RTL8372N on this tree's driver delivers the emulated frame to the named port only | rig | decline DSA egress |
| HE2b | Ingress PCP/DEI on the 835 tag do not reach W1/W3 | rig, PCP 3/5 and DEI 1 from the fake ISP | decline DSA egress for tagged-WAN ingress |
| HV1 | VLAN-aware: tagged VID 1 on lan1 and lan4; untagged on a port made an untagged PVID-1 member for the rig (lan2); declined on a software-bridged port (lan4 with `lan4.100` created while out of br-lan) | rig | decline VLAN-aware egress |
| HB | An L3 interface bound to no port (no L3_VP_PORT, MAC_BITMAP 0, routing off), used only as a nexthop's POST_L3_IF, gives the frame its EG_L3_IF source MAC, and its IN_L3_IF MTU is the one checked | rig, R3 (br-lan's own MAC, then the conduit's) | enable routing on slots 24-27 as the vendor does; else decline flows whose source MAC is not the conduit's |
| HS1 | A valid session entry leaves the session's other frames intact for the CPU | rig: pinned with no flow, then with one offloaded download, 5 min each, across a redial | keep PPPoE in software |
| HD | Hardware sets DE_ACCE in the flow entry when a de-accelerating exception fires | rig: after a code-0 control that hits, one wrong-code PPPoE frame on an offloaded download idle for 6-20 s; `rearmed_deacce` +1 at once, `rearmed_invalid` flat (software forwards the frame and re-offers the flow); a flat count is judged only with `offload_flows_hw` and the CPU code | staleness falls back to VALID only; a flow so de-accelerated stays in software until destroyed |
| HP0 | Session frames are decapsulated, routed and hit (absent from the CPU path, not counters) | rig | flows stay in software (no churn) |
| HP1 | Plain IPv4, another session, another source MAC, another VLAN do not hit (the last is the residual) | rig | decline PPPoE |
| HM | Full-size frames both ways pass; no MTU drop or redirect (no new CPU codes 79/80 L2 MRU/MTU, 82/91 L3 MTU (DF), 83 L3 MRU, 93/94 flow PMTU (DF) after the load; TESTING 5.3) | rig, then live | lower the flow's PMTU |
| HP2 | The upload encapsulation is correct (835, 0x8864, S, length) | rig, profile A | upload stays bench-only anyway |
| H0 | 802.1Q on the WAN still parsed with C-TPID 0 | rig, profile A | expected false |

Rig: the PC's `enp7s0u2.835` as a PPPoE concentrator without authentication
(the router never sends its real credentials), MTU/MRU 1500 with the VLAN at
1508 (the live zero-margin arithmetic), geekom on lan1 as the client, the
household offline, the operator on Wi-Fi.

## 10. Decisions for the owner

1. **WAN VLAN residuals** (sections 6 and 11): neither the flow key nor the
   PPPoE session match has a VLAN. A plain-IP flow from the WAN port also
   hits its 5-tuple untagged or on another VLAN of that port (perceival's
   SECURITY LIMITATION; `qcom_ppe.flow_offload=1` accepts it, and this
   line's WAN carries no plain-IP flow); a PPPoE flow also hits its session
   from the concentrator's MAC on another VLAN or untagged
   (`qcom_ppe.pppoe_offload=1` accepts it; leaving it off keeps PPPoE in
   software). Both forward frames software would drop, and Codex's
   per-commit review rates both as blockers under that rule. Neither lets
   a sender that can put arbitrary tags on the WAN link get a frame
   forwarded that it could not send on the flow's own VLAN, where software
   forwards it; a sender that upstream equipment confines to another VLAN
   of the line (another service of the ISP's on the same fibre) does gain
   a path, so the decision rests on what else shares the line upstream of
   the ONT. A hardware mitigation (VLAN
   translation into per-VLAN L3 interfaces on the WAN port) would program
   tables this series does not touch and change how every other VLAN on
   that port is received; it is not proposed.
2. **VLAN `used_keys` upstream fix**: a separate pending patch, outside this
   series' scope (it changes upstream netfilter semantics beyond the PPPoE
   match).

## 11. Limitations

- The flow key has the ingress L3 interface only: every LAN port and VLAN
  shares the conduit's, and the session match has no VLAN (perceival's
  SECURITY LIMITATION, unchanged; section 6).
- Downloads only in service; uploads need a different CPU tag or the athtag
  stripper.
- A client re-cabled to another LAN port has its flows torn down (the old
  port's carrier loss) and offered afresh; one moved between two ports that
  both stay up is followed only when the bridge learns it again (VID 1 in a
  VLAN-aware bridge), and until then its downloads stay on the old port.
- Up to 4 PPPoE sessions and 4 bridge MACs; more stay in software.
- A route's own `mtu` metric is not seen: the flow's PMTU is the egress
  port's MTU capped by the PPP device's, the bridge's and its VLAN 1
  upper's.
