# RTL8372N bench plan

Bench test of `rtl8372n_dsa` on a GL-BE9300; what has run so far is in
section 0. Run the phases in order, each only after the previous one passed;
on a failure, capture (section 6) and return to the previous phase's
configuration. Ports: lan1 = 7, lan2 = 6, lan3 = 5, lan4 = 4, CPU = 3 on the
conduit `lan`; WAN is not on the switch. "Debug" lines exist only in the
debug build.

## 0. Bench record

2026-10-20/21, one GL-BE9300 that is also the live gateway, debug image
r37571-89c18ad1cb, hosts: a PC on lan1 and a managed switch on lan4.

- **Phase 1** (offload off): 1.1 passed on five boots (a power cycle among
  them) and two module reloads; TABLE_RST cleared the sentinel in 290-520 us.
  1.2 passed for the PHY IDs, the binding, 2.5G links and EEE off (MMD 7.60
  and 7.62 read 0). 1.3 passed for pings and 1500-byte packets both ways;
  95% of LAN ingress went to one PPE queue. 1.5: VID 1 frames kept their tag.
  1.7: plain `rmmod` failed (each attached PHY pinned the module, fixed since
  by "rtl8372n: let the module unload while its PHYs are attached");
  unbinding through sysfs first, the reload passed. 1.8 passed. Not run:
  forced speeds, cable test, 1.4, 1.6.
- **Phase 2**: 2.1, 2.6 (static entries, ageing), 2.7 (Wi-Fi stations as
  CPU-port entries), 2.8, and 2.9 on idle ports passed; lan1-lan4 unicast
  was switched in hardware. Not run: 2.4, 2.5, 2.10, roaming.
- **Phase 3**, with lan1 PVID 1 and tagged 10/20, lan4 a tagged-only trunk in
  1/10/20: the 3.1 entries matched `bridge vlan show`; untagged lan1 to
  tagged lan4 in hardware; VID 10 tagged to the router both ways; VID 30
  dropped at ingress; 3.7 passed. Not run: link-local frames on a
  tagged-only port, 3.6, 3.8.
- Two days of normal use in that layout: no stop lines, no EDMA errors.

## 1. Prerequisites and recovery

- **Unit.** Prefer a spare unit with the AP profile. On the gateway, keep the
  WAN up: the WAN PHY is not on the switch, so SSH or WireGuard over the WAN
  survives switch, br-lan and VLAN faults that cut Wi-Fi. It still shares the
  kernel, CPUs and EDMA with the conduit: a driver oops reboots the router
  (`PANIC_ON_OOPS`) and a LAN storm loads the same CPUs, so on the gateway
  someone stays at the router through phases 2 and 3 to unplug the LAN. The
  other path is Wi-Fi on network `lan` (the APs sit in br-lan beside
  lan1-lan4). Check both before flashing.
- **Revert.** Before flashing, `sysupgrade -b /tmp/backup.tar.gz`, check it
  with `tar tzf`, and copy it off the router (`scp -O`); keep the running image (sysupgrade and `factory.bin`) and its
  `sha256sum` on the PC. To go back over Wi-Fi or the WAN, copy the
  known-good image to `/tmp` and `sysupgrade -v /tmp/<image>`: the network
  configuration stays valid (same lan1-lan4, same conduit `lan`). During
  phase 3, disarm its rollback first (section 5). Re-check whatever the
  router keeps outside sysupgrade's backup list.
- **Recovery.** No usable serial console (see the ramoops note in
  `ipq5332-gl-be9300.dts`). If the router no longer comes up, use GL.iNet's
  U-Boot web recovery (docs.gl-inet.com/router/en/4/faq/debrick/): PC wired
  to a LAN port at 192.168.1.2/24, reset held while powering on until the LED
  changes, then http://192.168.1.1, with GL's U-Boot stock image for the
  BE9300; this tree's `factory.bin` there is untested. Reach that page once
  before phase 1. The initramfs image needs U-Boot access. On a hang, wait
  about 90 s for the automatic reboot before power-cycling: a power cycle
  loses ramoops. After any crash, read `/sys/fs/pstore/` on the next boot.
  Keep an eMMC backup (top-level `README.md`).
- **Hosts:** three wired hosts, each directly on its own jack (no switch in
  between; 2.5G for rate checks) with `iperf3`, `tcpdump`, `scapy`; a Wi-Fi
  client; a second AP (roaming in 2.7, a VID 10/20 trunk on lan1 in phase 3).
- **Network config:** br-lan with lan1-lan4 and no `bridge-vlan` until phase 3.
- **Build** with `RTL837X_DSA_DEBUG` (most checks are `dev_dbg` lines; the
  target has no dynamic debug), `kmod-ramoops`, `mdio-tools` (1.2) and the
  router tools `ip-full`, `ip-bridge`, `tcpdump`, `iperf3` and `ethtool-full`
  (`--cable-test` needs netlink, which the default `ethtool` lacks). Turn
  `RTL837X_DSA_OFFLOAD` off (on by default): phase 1 runs with bridges in
  software. Rebuild without `RTL837X_DSA_DEBUG` before an image stays in
  service.

```sh
./scripts/feeds update -a && ./scripts/feeds install -a
cp configs/ap.config .config      # spare unit (AP profile: no NAT, no PPPoE)
# gateway: start from its own saved configuration instead; firewall4, ppp
# and the rest stay
cat >> .config <<'EOF'
CONFIG_RTL837X_DSA_DEBUG=y
# CONFIG_RTL837X_DSA_OFFLOAD is not set
CONFIG_PACKAGE_kmod-ramoops=y
CONFIG_PACKAGE_mdio-tools=y
CONFIG_PACKAGE_ip-full=y
CONFIG_PACKAGE_ip-bridge=y
CONFIG_PACKAGE_tcpdump=y
CONFIG_PACKAGE_iperf3=y
CONFIG_PACKAGE_ethtool-full=y
EOF
make defconfig
make -j"$(nproc)"
grep -ac 'joined bridge' build_dir/target-*/linux-qualcommbe_ipq53xx/rtl8372n-*/rtl8372n_dsa.ko  # >= 1
grep -c is_link_local_ether_addr build_dir/target-*/linux-qualcommbe_ipq53xx/linux-*/net/dsa/tag_rtl8_4.c  # 1: 0432
```

- **The `offload` parameter** is read at probe. It defaults to on, or to off
  (every bridge in software) in an image built without
  `CONFIG_RTL837X_DSA_OFFLOAD`. `options rtl8372n_dsa offload=1` (or `=0`)
  in `/etc/modules.conf`, which sysupgrade keeps, overrides either default
  from the next boot. `cat /sys/module/rtl8372n_dsa/parameters/offload`
  reads `N` in phase 1, `Y` in phases 2 and 3: check it after every boot.
  Plain `insmod` ignores `/etc/modules.conf`: reload by hand with
  `rmmod rtl8372n_dsa; insmod rtl8372n_dsa offload=0` (tried in 1.7). The
  kernel command line is not read.

## 2. Stop conditions

Stop, capture and go back to offload off (`options rtl8372n_dsa offload=0` in
`/etc/modules.conf`, reboot) on a LAN-to-LAN leak, an
echoed frame, a storm, or any output from the lines below. On a leak, echo or
storm, unplug the LAN cables first (or set the ports down over Wi-Fi).

```sh
dmesg | grep 'hardware bridging disabled' | grep -v 'offload=0'
dmesg | grep -E 'switch setup failed|unsupported P0|failed to reset switch|regmap init failed|get reset GPIO|unable to (detect|register)|PHY discovery|no PHY driver bound|PHY C45 read failed|MISMATCH|P0 readback .*failed|readback mismatch|table command|clear failed|self-test failed|TABLE_RST|before reset not read|left entries set|not zeroed|reserved bits|non-realtek|unknown realtek|could not find user|Call trace'
dmesg | grep -E 'held DISABLED|may still|still reachable|not linked again|mode unknown|left unlinked|join failed|join undone|VLAN filtering [01] failed|kept VLAN filtering|re-applied|applied on retry|back to VLAN filtering|not restored|not cleared|not disabled|not updated|retried later|failed again|flush failed|flush mode left|failed to set STP|unknown STP|another key|still present|static entry written|: wrote 0x|add failed|delete failed|stale'
```

`hardware bridging disabled: <reason>` with a reason other than
`module parameter offload=0` means the driver turned offload off and every
bridge runs in software: record it and the line before it; no phase 2. To
recover without a reboot after `held DISABLED`, `mode unknown`,
`left unlinked` or `not linked again`, `ip link set lanX down; ip link set
lanX up` retries (`port N: hold released` ends a hold). Recorded, not stops:
DSA's `port N failed to add <mac> vid 0 to fdb: -28` (bucket full) and
`L2 walk from ... (bit 13)` (a short `bridge fdb show`). Runtime fallback
without a reboot, phase 2 only (in phase 3 a port re-added by hand loses its
`bridge-vlan` VLANs and lands in VLAN 1: reboot instead): a port with an
8021q upper is bridged in software (`Port has 8021q uppers, bridged in
software`):

```sh
for p in lan1 lan2 lan3 lan4; do ip link set $p nomaster
  ip link add link $p name $p.4000 type vlan id 4000; ip link set $p master br-lan; done
```

## 3. Phase 1: offload off

Every LAN port is standalone in the switch; br-lan bridges in software.
Setup prints **79** lines starting `P0 readback `; all end in `PASS` except
`SA-moving forbid ... (not checked)`. Per name:

```sh
dmesg | grep -c 'P0 readback'                     # 79
dmesg | grep 'P0 readback' | grep -vc 'PASS$'     # 1
dmesg | grep -o 'P0 readback [^:=]*' | sed 's/ at 0x.*//' | sort | uniq -c
```

`VLAN 0 data0`, `VLAN 1 data0`, `VLAN 2 data0`, `VLAN 4094 data0`,
`VLAN 4095 data0` (1 each); `port isolation`, `learning limit` (10 each,
ports 0-9); `CPU flood destination`, `PVID`, `VLAN egress transparent ports`,
`VLAN egress keep ports` (5 each); `VLAN learning disable` (2);
`RMA link-local trap` (7); 1 each of `VLAN accepted frame types`,
`VLAN ingress filter`, `VLAN egress tag mode`, `CPU tag-aware ports`,
`VLAN filter enable`, `VLAN table reset`, `VLAN reserved VID types`,
`port-based FID enable`, `STP port states`, `unknown unicast action`,
`unknown L2 multicast action`, `unknown IPv4 multicast action`,
`unknown IPv6 multicast action`, `new source address action`,
`unmatched source address action`, `IGMP/MLD snooping`, `RMA/PTP trap CPU`,
`L2 trap CPU`, `L2 lookup-hit isolation action`, `L2 hash-full action`,
`learn-limit action`, `source port permit`, `SA-moving forbid`,
`RMA LLDP trap`, `RMA LLDP_EN`. VLAN 0 expects `0x2000000`, VLAN 1
`0x203e0f8` (mask `0x3ffffff`); VIDs 2, 4094, 4095 expect 0 under `0xffffff`.
Record every value: bits outside the masks were never recorded.

| # | Do | Expect |
| --- | --- | --- |
| 1.1 | Every boot: dmesg. | `chip ID: 0x837270..`; one `hardware bridging disabled: module parameter offload=0`; the 79 lines; `VLAN table before reset: VID 2/4094/4095 ...`; `VLAN table reset in N us: VID 2/4094/4095 a/b/c` with bits 23:0 clear (record words and time); `L2 entry self-test passed (02:52:54:4c:83:72 vid 1 port 3)`. Debug: `VLAN 4094 entry 0x02000008` (sentinel), `L2 table cleared in N us`, `port 3: STP state 3, CIST 3`, `port N: STP state 0, CIST 0` for ports 0, 1, 2, 8. No debug lines: not a debug build, debug checks void. |
| 1.2 | PHYs: dmesg; `cat /sys/bus/mdio_bus/devices/*:1d:0[4-7]/c45_phy_ids/mmd1_device_id`; `ethtool lanN` with no cable; per jack replug, `ethtool -r lanN`, `ethtool --cable-test lanN` (idle jack); a 10, a 100 and a 1000 partner, then `ethtool -s lanN speed 100 duplex full autoneg off` against a partner forced the same, then `ethtool -s lanN autoneg on`; per PHY, MMD 7 registers 60 and 62 (EEE advertisement 1 and 2) with `mdio` (mdio-tools; the switch's bus has addresses 4-7). | Debug `port 4: PHY driver RTL8224 2.5Gbps PHY`, same for 5-7; `0x001ccad0` four times; 2500baseT/Full supported; carrier and speed follow the cable; renegotiation and per-pair cable results work; each partner links at its speed, forced 100 included, and 2500baseT/Full is advertised again after `autoneg on`; MMD 7.60 reads 0 and 7.62 bit 0 (2.5GBASE-T EEE) is clear on all four. |
| 1.3 | LAN through the CPU: 100 pings to the router per jack; `iperf3 -t 30` between two jacks (all six pairs if possible), both ways and `-P 4`, with `top` and `ip -s link show lan` before/after; `iperf3 -s` on the router and `iperf3 -c <router> -R` from a host, then, with the WAN quiet, `iperf3 -c <router> -P 8` with the `rxdesc` lines of `/proc/interrupts` and per-CPU softirq time (`si` in `top`, or `/proc/stat`) read before and after (NAPI masks a busy ring's IRQ, so a low count is not an idle ring; `/proc/softirqs` is spread by RPS and shows nothing here); `ping -s 1472` from the router (BusyBox has no `-M`; 1500-byte packets need no fragmentation) and `ping -M do -s 1472` from the host; a routed LAN-to-WAN download (gateway image; BLOCKED on the AP profile). `ip link set lan2 nomaster; ip link set lan2 master br-lan`. | No loss. Debug `port N: bridged in software: module parameter offload=0` per port, no `joined bridge`; the re-enslave warns `hardware bridging disabled: module parameter offload=0 at setup`. Conduit counters grow by the traffic. Router-sent TCP at full rate, no retransmission storm (0431). 1500-byte packets pass both ways. Record how the `-P 8` upload spreads over the four `rxdesc` IRQs and CPUs: all on one ring is the expected cost of the PPE not parsing past the rtl8_4 tag (section 7); compare a `be9300-next` image if possible. Record all numbers: phase 2's baseline. |
| 1.4 | `ip link set lan2 nomaster`, lan2 on its own subnet; both hosts ping and broadcast with `tcpdump -e` running. Then `ip link set lan1 down; sleep 2; ip link set lan1 up`. Restore lan2. | Neither host sees the other's frames; the lan2 host reaches lan2's address. Debug `port 7: STP state 0, CIST 0`, then `port 7: STP state 3, CIST 3`. |
| 1.5 | VLAN pass-through, lan1 out of br-lan: `ip link add link lan1 name lan1.10 type vlan id 10`, addressed, pinging a host tagging VID 10, also `ping -s 1472` from the router and `ping -M do -s 1472` from the host; frames tagged VID 1 (`eth0.1`) and priority-tagged (`ip link add link eth0 name eth0.0 type vlan id 0 egress-qos-map 0:5`) from the host, under `tcpdump -i lan1 -e`. Delete lan1.10, re-add lan1, then `ip link set br-lan type bridge vlan_filtering 1; bridge vlan add dev lan1 vid 10; bridge vlan add dev lan2 vid 10 pvid untagged`; revert after. | Tagged VID 10 both ways, ARP included, 1500-byte packets pass both ways (1526 bytes on the conduit with the tag; on a loss, record `ethtool -S lan` and `/sys/kernel/debug/ppe/cpu_code`, read once traffic has stopped); VID and PCP unchanged. The tagging lan1 host and untagged lan2 host ping through the software bridge; no driver `VLAN ... added` line. |
| 1.6 | Control frames: lan1 and lan2 out of br-lan; send from the lan1 host (script below); `tcpdump -i lan -Q in -e -xx 'ether[0:4] = 0x0180c200'`. Then lan1 and lan2 back in br-lan, `ip link set br-lan type bridge stp_state 1` (0 after). | Each frame reaches `lan1` once; byte 15 (REASON) non-zero: record it and bytes 16-17. The lan2 host sees none. The lan1 host then receives the router's BPDUs. |
| 1.7 | Three `reboot`s and one power cycle. Then `rmmod rtl8372n_dsa; insmod rtl8372n_dsa offload=0`. | 1.1 holds each time; a ping from each jack. After the reload, lan1-lan4 are back in br-lan and 1.1 and the pings hold again. |
| 1.8 | Rx checksum: from a LAN host, 10 TCP SYNs to the router's port 22 with a wrong checksum (scapy: `send(IP(dst="<router>")/TCP(dport=22, flags="S", chksum=0x1234), count=10)`); `grep Tcp: /proc/net/snmp` on the router before and after. | `InCsumErrors` (last column) grows by 10 and the router sends no SYN-ACK. Not growing: the EDMA claims a checksum it never checked: stop. |

```python
from scapy.all import Ether, Raw, sendp   # BPDU, LACPDU, EAPOL, LLDP; 30 each
for da, et in (("00", 38), ("02", 0x8809), ("03", 0x888e), ("0e", 0x88cc)):
    sendp(Ether(dst="01:80:c2:00:00:" + da, type=et) / Raw(bytes(46)), iface="eth0", count=30)
```

## 4. Phase 2: offload on, VLAN-unaware br-lan

Add `options rtl8372n_dsa offload=1` to `/etc/modules.conf` and reboot, or
flash an image built with `CONFIG_RTL837X_DSA_OFFLOAD=y`, the default (and
no `offload=0` line there).
The default br-lan (lan1-lan4, no `bridge-vlan`) is offloaded at boot.

| # | Do | Expect |
| --- | --- | --- |
| 2.1 | Boot; dmesg. `ip link set lan2 nomaster`, then back. | 1.1 holds, but no `hardware bridging disabled` line at all. Debug `port N joined bridge 1, linked M` per port, the last listing the other three (`0xe0` if lan4); `port N: bridge flags .../..., bridged 1`; `L2 <br-lan MAC> vid 1: static entry for port 3`. `port 6 left bridge 1`; no warning on re-adding. |
| 2.2 | Repeat 1.3's iperf3 runs and the routed download. VID 10 uppers on the lan1 and lan2 hosts ping each other. | Near line rate, router CPU near idle, `ip -s link show lan` nearly still. Tag unchanged. Routed rate as in 1.3. |
| 2.3 | `ip link set lan4 nomaster`, own subnet as in 1.4: ARP, unicast to the lan1 host's MAC (static neighbour), mDNS, UDP to 239.1.1.1. Then `ip link add br-test type bridge; ip link set lan4 master br-test`. Clean up. | `port 4 left bridge 1`. The lan1 and lan4 hosts see nothing of each other; lan4 reaches the router. br-test warns `Range of offloadable bridges exceeded` (DSA) and runs in software; lan4 stays CPU-only. |
| 2.4 | Control frames, lan1 and lan2 in br-lan: script above from the lan1 host, observer on lan2, with `ip link set br-lan type bridge stp_state 0` / `1` and `group_fwd_mask 0` / `0x4008`. Also a few frames to 01:80:c2:00:00:10, :20, :21, 01:00:0c:cc:cc:cc, :cd and 01:1b:19:00:00:00. | STP off: exactly one copy of each BPDU (none: 0432 missing; two: the switch forwarded it too); STP on: none. EAPOL and LLDP: none at mask 0, exactly one at 0x4008. LACPDUs never. The other DAs arrive exactly once; none, with a non-zero REASON on the conduit, means that reset action traps: record the DA. |
| 2.5 | From the lan1 host: unknown unicast (static ARP for an absent MAC), multicast to 239.1.1.1, ff0e::1234 and 03:00:00:00:00:01, ARP broadcast. `bridge link set dev lan2 flood off`, `mcast_flood off`, `bcast_flood off`, then on. `hairpin on`, `isolated on`. | Each class stops reaching the lan2 host exactly while its flag is off; the router sees all of them; nothing returns to its sender. hairpin/isolated fail: `Only learning and flood flags are offloaded`. |
| 2.6 | FDB: `bridge fdb show dev lan2`; `bridge fdb add 02:00:00:00:00:01 dev lan1 master static`, show, `del`; unplug and replug lan2; `bridge link set dev lan2 learning off` with one-way UDP from the lan2 host; `ip link set br-lan type bridge ageing_time` 3000, 0, 100000, 3000 with a lan2 host sending one frame; restore 30000. `stp_state 1`, re-enslave lan2, `stp_state 0`. | The lan2 host is `self`, vlan 1. Debug `L2 02:00:00:00:00:01 vid 1: static entry for port 7`, listed `self` and `static`, then gone. Unplug: `port 6: STP state 0, CIST 0`, `port 6: dynamic L2 entries flushed`, entry gone, relearned after replug; static entries survive. Learning off: learned entry flushed, static kept, UDP still switched in hardware. Debug `L2 ageing time 30 s`, `14 s`, `800 s`; the silent host leaves after about 30-60 s (record). STP: `port 6: STP state 1, CIST 1`, then 2, then 3; no LAN traffic before 3. |
| 2.7 | Wi-Fi client W talks; `iperf3` between the lan1 host and W with `tcpdump 'ether host <W> and not broadcast and not multicast'` on the lan3 host. Roam: W on an AP on lan2, pinged from the lan1 host, moved to the router's Wi-Fi and back. | Debug `L2 <W> vid 1: static entry for port 3`; nothing of the stream on lan3. The ping resumes within seconds each way, not after 300 s. Back on the AP: `L2 <W>: DSA vid 0 released by port 3, entry for port -1`; `bridge fdb show dev lan2` lists W as learned `self`. |
| 2.8 | `ip link add link lan1 name lan1.100 type vlan id 100` with lan1 in br-lan. Then out of br-lan, create it, re-add lan1. Delete it and re-add lan1. | First: EBUSY, `8021q uppers are not supported on ports of the offloaded bridge`. Then `Port has 8021q uppers, bridged in software`, no `port 7 joined bridge`, lan1's LAN traffic crosses the conduit. Last: offloaded again. |
| 2.9 | Remove every port from br-lan, one at a time, then re-add. | `port N left bridge 1` and join lines, no error; each removed port passes 2.3's isolation. |
| 2.10 | lan1 and lan2 hosts exchange ping and `iperf3 -u`; `bridge link set dev lan2 state 0` with the cable in, then `state 3`. | Debug `port 6: STP state 0, CIST 0`. While it holds, no data crosses either way and none from the lan2 host reaches the conduit (trapped link-local frames may: record them); `state 3` restores traffic at once. Data crossing: CIST DISABLED does not block with the link up, and the driver's DISABLED hold of a port it could not unlink does not isolate it: stop. |

## 5. Phase 3: VLAN-aware br-lan

Keep a copy of `/etc/config/network` and arm a rollback that, 10 minutes
after each boot, restores it, disarms itself and reboots, unless `/tmp/keep`
exists. Its flag lives in `/root`, which sysupgrade does not keep:

```sh
cp /etc/config/network /root/network.phase2 && touch /root/rollback.armed
grep -q rollback.armed /etc/rc.local || {
  echo '[ -e /root/rollback.armed ] && (sleep 600; [ -e /tmp/keep ] || { rm /root/rollback.armed; cp /root/network.phase2 /etc/config/network && reboot; }) &'
  cat /etc/rc.local; } > /tmp/rc.local && cp /tmp/rc.local /etc/rc.local
```

Add the sections below (lan1 a trunk to an AP, lan2 and lan3 access ports of
VIDs 10 and 20, lan4 of VID 1), set `option device 'br-lan.1'` in
`interface lan`, add addressed interfaces on `br-lan.10` and `br-lan.20`, and
reboot. Once SSH works over Wi-Fi (or the WAN), `touch /tmp/keep`, within 10
minutes of every boot in phase 3 (3.8 and stop reboots included). Leaving
phase 3 by any route (pass, fail or reflash), disarm:
`rm -f /root/rollback.armed; sed -i '/rollback.armed/d' /etc/rc.local`. Runtime
changes in 3.3-3.8 are not saved, so a reboot undoes them: before each,
`rm -f /tmp/step-ok; setsid sh -c 'sleep 600; [ -e /tmp/step-ok ] || reboot' &`,
and `touch /tmp/step-ok` once it has passed (a separate flag: removing
`/tmp/keep` would let the boot-time rollback fire). At `vlan_filtering 0`, `lan` on
`br-lan.1` loses untagged traffic, Wi-Fi included: run 3.6 detached
(`setsid sh -c '...' &`), ending at 1.

```
config bridge-vlan
	option device 'br-lan'
	option vlan '1'
	list ports 'lan1:u*'
	list ports 'lan4:u*'
config bridge-vlan
	option device 'br-lan'
	option vlan '10'
	list ports 'lan1:t'
	list ports 'lan2:u*'
config bridge-vlan
	option device 'br-lan'
	option vlan '20'
	list ports 'lan1:t'
	list ports 'lan3:u*'
```

| # | Do | Expect |
| --- | --- | --- |
| 3.1 | Boot; dmesg; `bridge vlan show`. | Join lines, then `port N: VLAN filtering 1, bridge PVID P, linked M` per port (the last lists the other three) and `port N: VLAN V added (flags F), bridge PVID P` matching `bridge vlan show`. Last entry words: `VLAN 1 entry 0x02026098` (CPU, lan1, lan4), `VLAN 10 entry 0x020100c8`, `VLAN 20 entry 0x020080a8`. |
| 3.2 | Trunk/access: VID 10 and 20 traffic between the AP and the lan2/lan3 hosts, with iperf3. | Untagged on access ports, tagged with PCP kept on lan1; switched in hardware (router CPU idle). |
| 3.3 | PVID: `bridge vlan add dev lan2 vid 10` (no `pvid`); untagged, priority-tagged and VID 10 frames from the lan2 host; with `stp_state 1`, untagged BPDU, LLDP and EAPOL from it. Restore with `pvid untagged`. | Debug `port 6: VLAN 10 added (...), bridge PVID 0`. Untagged and priority-tagged dropped everywhere, conduit included; VID 10 passes. The link-local frames still reach the conduit; if not, TAG_ONLY acts before the trap: report, and give such ports a PVID. |
| 3.4 | VLAN isolation: lan2 and lan3 hosts on one subnet; VID 20 tagged into lan2, VID 30 into lan1, VID 4095 into lan2, priority-tagged into lan2; VID 40 on lan2 and lan3 only (`bridge vlan add dev lan2 vid 40`, same for lan3). | No lan2-lan3 path. VIDs 20, 30, 4095 dropped everywhere, conduit included. Priority-tagged lands in VID 10. VID 40 switched lan2-lan3, never on the conduit. |
| 3.5 | br-lan.X: hosts and the AP (VID 10) reach the `br-lan.10`/`br-lan.20` addresses; `tcpdump -i lan -e -xx`. | Access-port frames arrive untagged, trunk frames tagged; router frames leave access ports untagged, the trunk tagged. A VID 1 tag on a router frame at lan2/lan3: the KEEP bit does not override ORIGINAL; stop, keep br-lan VLAN-unaware. |
| 3.6 | `ip link set br-lan type bridge vlan_filtering 0`, test, `1`, test; three times; then ten toggles with the AP sending VID 20 broadcasts, capturing on the lan2 host. | Per toggle, `port N: VLAN filtering V, bridge PVID P, linked M` per port with growing masks, then `L2 VID 0/1 entries re-synced for VLAN filtering V`. At 0 all ports are one domain, tags unchanged; at 1, 3.2 and 3.4 hold. The lan2 host never gets an untagged copy, nor VID 20 while at 1. After `VLAN filtering V failed` and `kept VLAN filtering V after a failed change`, repeat the same command (down/up does not heal it). |
| 3.7 | `ip link set lan2 nomaster`, then re-add. | `port 6 left bridge 1`; the next `VLAN 1 entry` includes `0x40` again; 1.4 and 1.5 pass on lan2. Re-adding restores 3.1. |
| 3.8 | `bridge vlan add dev lan3 vid 2`, warm reboot. | `VLAN table before reset` shows VID 2 set if the chip reset keeps the table; `VLAN table reset in N us` shows it clear; no `left entries set`. |

## 6. What to capture

- Build: `git rev-parse HEAD`; `sha256sum` of the sysupgrade image and of the
  `kmod-dsa-rtl8372n` package in `bin/targets/qualcommbe/ipq53xx/packages`;
  the `grep -ac 'joined bridge'` count.
- Per boot: the `offload` parameter, full `dmesg` saved right after boot (the
  128 KiB log must still start at `Booting Linux`), `/sys/fs/pstore/*`.
- Per phase: `ip -d link`, `ip -s link`, `bridge link`, `bridge vlan show`,
  `bridge fdb show`, `ethtool lanN`; around each rate run, the `rxdesc` lines
  of `/proc/interrupts` and `/proc/softirqs`.
- Per check: PASS, FAIL or BLOCKED with commands and output, pcaps for the
  isolation and control-frame checks, iperf3 results with router CPU; for a
  stop line, dmesg from the first error. Redact MACs and serials.

## 7. Known limitations

- One hardware bridge: a second one, or br-lan if another bridge took a LAN
  port first, runs in software.
- LAN-to-LAN traffic switched in hardware bypasses nftables bridge rules;
  `ip -s link show lanN`, LuCI and ethtool count only what crosses the CPU
  (the SDK-based driver read the switch MIB).
- PPE flow offload (`qcom_ppe.flow_offload`, see
  `target/linux/qualcommbe/docs/ppe-flow-offload/`) is on by default in
  kmod-qcom-ppe for this subtarget, and used when fw4's hardware flow
  offloading is on.
  - Routed traffic crosses the CPU unless both are on.
  - With both on, only downloads into the switch are offloaded (the
    nexthop emulates the rtl8_4 tag).
  - Uploads from the switch cross the CPU, except under the PPE offload's
    bench-only profile A.
- The PPE parser does not walk past the 8-byte rtl8_4 tag: frames from the
  LAN ports nearly all land on one EDMA Rx ring and CPU (95% on one PPE queue
  in 1.3; RPS still spreads the protocol work) and get no Rx checksum offload
  (Linux checks it, 1.8); frames to them get no Tx checksum offload or TSO
  (0431). perceival's parser alias (0445 in his tree, built on his PPE
  offload series) reprograms the chip-wide TPIDs (ingress C-TPID 0x0000) and
  the CPU port's egress VLAN mode; his own notes record that flows entering
  through a tagged WAN then never hit, so a VLAN WAN loses its offload and,
  by the same parse, its RSS spread and Rx checksum. Carried only as the PPE
  offload's bench profile A (0439), applied for minutes on a bench.
- In a VLAN-unaware offloaded bridge, unicast tagged with a VID other than 1
  towards the router floods to the bridge ports: use a VLAN-aware br-lan for
  VLANs that end on the router.
- 8021q uppers are refused on ports of the offloaded bridge; a port with one
  is bridged in software (a netifd `lanN.M` beside br-lan depends on bring-up
  order; use `bridge-vlan`), and unicast to its stations floods to the
  offloaded ports. On a VLAN-aware port, a refused upper with a bridge VID
  still drops that VID from the port in the switch (DSA adds it first):
  delete and re-add the bridge VLAN.
- A port joining a bridge whose `vlan_protocol` is not 802.1Q (802.1ad) is
  bridged in software. DSA does not pass a later protocol change on to the
  driver: ports of an offloaded bridge switched to 802.1ad stay offloaded
  with 802.1Q rules until they are taken out and added back. Set the
  protocol before adding the ports.
- Only learning, flood, mcast_flood and bcast_flood are offloaded; other
  bridge port flags are refused on offloaded ports. No MDB: multicast floods
  per `mcast_flood` whatever IGMP snooping decides. No LAG, MST, rate
  limiting, MIB counters or GPIO; EEE off.
- Ageing is clamped to 14-800 s; `ip -d link show br-lan` shows the value set.
- A failed runtime `vlan_filtering` change splits the bridge until repeated.
- `bridge fdb replace` of a host address onto a LAN port: once deleted there
  it floods; replaced back onto Wi-Fi, the entry stays on the LAN port.
- Link-local traps need 0432; 01:80:c2:00:00:10-2f, CDP, SSTP and PTP keep
  their reset actions. A VLAN-aware port without a PVID admits tagged frames
  only (TAG_ONLY; the bench's lan4 dropped untagged frames); whether untagged
  link-local frames still reach the CPU on such a port is untested (3.3).
- VID 1 and priority-tagged frames reach Linux tagged: a host tagging VID 1
  needs a VLAN-aware br-lan with VID 1 untagged PVID, or a `lanN.1` upper.
