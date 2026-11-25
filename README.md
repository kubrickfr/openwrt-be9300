# OpenWrt for the GL.iNet Flint 3 (GL-BE9300)

Mainline **OpenWrt** support for the **GL.iNet Flint 3 (GL-BE9300)**: Qualcomm
**IPQ5332** (quad Cortex-A53) with tri-band Wi-Fi 7, a Realtek **RTL8372N** 10G
switch and a **RTL8221B** 2.5G WAN PHY.

This is a complete, buildable OpenWrt tree: upstream OpenWrt `main` with a
curated GL-BE9300 series on top. Build from `be9300`, the tested branch;
`be9300-next` is where new work lands first. Target
**`qualcommbe/ipq53xx`**, kernel **6.18**.

> [!WARNING]
> **Unofficial, community-maintained port — not affiliated with, endorsed by, or supported
> by GL.iNet or the OpenWrt project.** Provided **as-is, with no warranty of any kind**.
> Flashing third-party firmware carries real risk, including bricking the device, and may
> void your hardware warranty. **Back up your eMMC first** — the ART partition holds your
> unit's unique radio calibration data and MAC addresses and cannot be recovered from
> anywhere else. If this router matters to you, test on a spare unit before relying on it.

## Where this tree comes from

It carries [perceival's Flint 3 port](https://github.com/perceival/openwrt-flint3)
(branch `flint3-be9300`), rebased onto upstream OpenWrt `main`. That port was
built on [JiaY-shi's GL-BE6500 (Flint 3e) work](https://github.com/JiaY-shi/openwrt),
which provided the IPQ5332 Wi-Fi and RTL837x DSA foundation, and on the IPQ53xx
subtarget of [openwrt/openwrt#23161](https://github.com/openwrt/openwrt/pull/23161).

The rebase is not a plain replay. On top of the port it adds:

- an SDK-free RTL8372N DSA driver that offloads a VLAN-aware bridge to the
  switch;
- PPE hardware flow offload of IPv4 downloads on VLAN 1, PPPoE included;
  with the opt-in S-R mode, uploads too, and other VLANs of the bridge;
- EDMA and PPE driver fixes (WAN ingress at line rate, RPS, ring mapping);
- newer ath12k and hostapd fixes, Wi-Fi MAC addresses, the regulatory
  country and fan control.

The aim is a tree that tracks upstream closely and is easy to review and to
send upstream. The GL-BE6500 and UniFi U7 Pro XGS profiles of the trees it
grew from are not carried.

## Hardware

| Block | Detail |
|---|---|
| SoC | Qualcomm IPQ5332, 4× Cortex-A53 |
| Wi-Fi 2.4 GHz | on-SoC radio, ath12k over AHB |
| Wi-Fi 5 / 6 GHz | one QCN9274 over PCIe, split into a 5 GHz and a 6 GHz radio |
| Switch | RTL8372N, SDK-free DSA driver (`realtek,rtl8372n`, `rtl8_4` tag), 10GBASE-R to the SoC |
| WAN | RTL8221B 2.5G PHY, `2500base-x` to the SoC |
| Storage | eMMC |

Wi-Fi MAC addresses come from the ART partition (offset 0x04, as on stock);
the secondary links of an MLD get locally administered addresses derived
from it. The kernel's AES and GCM use the ARMv8 Crypto Extensions.

## Status

| Subsystem | State |
|---|---|
| Boot, sysupgrade, return to stock | working |
| LAN switch | working; one VLAN-aware bridge switched in hardware, access, trunk and tagged-only ports |
| WAN | working at 2.5 Gbit/s, line rate into the SoC |
| Flow offloading | working; IPv4 downloads in hardware, PPPoE included. The PPE driver is on by default; fw4's hardware flow offloading, off in a fresh configuration, turns it on. Uploads and VLANs other than 1 too with the opt-in S-R mode (package `ppe-sr`) |
| Wi-Fi 7, all three bands; MLO across 5 and 6 GHz | working |
| DFS | working: CAC, and the secondary AP after CAC |
| 802.11k / 802.11v | working |

What is untested and what is open is under
[Known limitations](#known-limitations).

## Performance

Measured on this board, over PPPoE on a VLAN WAN:

- With hardware flow offload on, one TCP download at 2202 Mbit/s with
  the busiest core 24.7% busy. In software the same stream ran at
  2010 Mbit/s with a core at 99.4% (mock-ISP bench, 2.5G client,
  2026-11-12). On the same bench 0.0007-0.0021% of the downloads' packets
  reached the CPU, and at most 0.0029% over a 10-hour soak; what is left
  on the CPU is the client's ACKs.
- In S-R mode, one TCP upload at 939 Mbit/s (the 1G client's limit) with
  the router's cores 0.1% busy; in software the same upload kept one core
  at 100%. Downloads in S-R mode: 939 Mbit/s at 0.0% (bench, 2026-11-21).
  S-R also holds at 2.5G.
- On a 2.2/0.8 Gbit/s FTTH line, 2110-2133 Mbit/s down with Ookla's CLI. A
  PC on the ONT without the router got 2080-2107 Mbit/s from the same
  server, so the router does not limit the line (2026-10-27).
- The WAN port takes 2370 Mbit/s with no FIFO overflow (bench,
  2026-11-12). FIFO overflow, from the PPE's TDM table, caused the old
  1.9 Gbit/s ceiling; the fix is in this tree.
- Port to port through the switch, 2.35 Gbit/s with the router's cores
  under 4% busy.

The CPU figures are busy time measured from idle time. BusyBox `top`
reads far lower on this kernel. Method and raw figures:
`target/linux/qualcommbe/docs/ppe-flow-offload/TESTING.md` (sections 0,
0b and 6.4).

## Flow offloading

Choose in LuCI: **Network → Firewall → General Settings → Routing/NAT
Offloading → Flow offloading type**:

- **Hardware flow offloading** (recommended). The PPE forwards IPv4
  downloads into the LAN switch, PPPoE over a VLAN WAN included.
  Everything it declines runs in the software flowtable.
- **Software flow offloading.** The software flowtable only. It is built
  on `br-lan`, and since OpenWrt's 706-03 a flow through `br-lan` is not
  offloaded at all, so LAN and Wi-Fi traffic takes the normal forwarding
  path. For the software flowtable without the PPE, choose hardware flow
  offloading and set `flow_offload=0` (below).
- **None.**

Or with uci:

```sh
uci set firewall.@defaults[0].flow_offloading='1'
uci set firewall.@defaults[0].flow_offloading_hw='1'   # '0' for software only
uci commit firewall && fw4 reload
```

The PPE driver takes what fw4 offers. Its two switches, `flow_offload` and
`pppoe_offload`, default to on for ipq53xx (kmod-qcom-ppe options
`KERNEL_QCOM_PPE_FLOW_OFFLOAD` and `KERNEL_QCOM_PPE_PPPOE_OFFLOAD`; off on
ipq95xx, where this code has never run). So fw4's setting alone turns
offload on and off. fw4's hardware flow offloading is off in a fresh
configuration. To override:

- at boot: `options qcom-ppe flow_offload=0` (or `pppoe_offload=0`, to
  keep PPPoE in software) in `/etc/modules.conf`. The module name there is
  `qcom-ppe`, with a hyphen;
- at runtime: `/sys/module/qcom_ppe/parameters/`;
- with S-R: `ppe-sr`, with `option ppe_params 1` (its default), sets both
  back to 1 when it arms (`ppe-sr enable`, or the daemon's first arm),
  and puts them back if `ppe-sr enable` fails. To keep either one at 0,
  set `ppe_sr.main.ppe_params` to 0;
- state: `/sys/kernel/debug/ppe/offload_flows`. `declined` counts offers,
  not flows: the kernel re-offers each active flow about once a second.

**How it was tested.** It was the live gateway on a 2.2 Gbit/s FTTH line
from 2026-10-22 to 2026-11-09: the first PPPoE session in hardware, a
night of about 10 hours under a guard, four days of normal use, redials,
firewall reloads and a client moved between ports. From 2026-11-07 to
2026-11-09 it booted with offload on by default (`0442`'s defaults). Then
a mock-ISP bench ran from 2026-11-09 to 2026-11-19 on the same unit:

- PPPoE faults from the ISP side: PADT, TermReq, concentrator restarts and
  changes, new and reused session IDs, LCP echo loss, a silent
  concentrator, cable pulls, the flap breaker;
- a 10-hour soak with 30 injected PPPoE faults;
- hostile WAN frames, a full flow table, FDB floods past the switch's 4160
  entries, broadcast and multicast storms at 1G, all cores at 100%;
- client moves between ports, bridge VLAN changes, firewall reloads,
  disable under load, a panic, and boots with offload on.

There was no router failure: no stuck flow, no degraded state, no kernel
error. Every session came back and its flows went back into hardware. The
bench found one router bug, in the generic kernel and not the PPE: the
fraglist GRO TTL bug under Known limitations. The bench image had both
`0442` options unset and switched offload on at runtime or from
`/etc/modules.conf`; images built from this tree have both defaults on.
The record, with the gates and what each showed, is in
`target/linux/qualcommbe/docs/ppe-flow-offload/` (TESTING.md sections 0
and 0b, DESIGN.md sections 9-11).

Since 2026-11-21 the same unit has been the main gateway again, on an
image built from this tree with both `0442` defaults on and S-R
auto-armed across three VLANs. By 2026-11-25 it had run 3 days 18 hours
with no reboot, no fault and no stuck flow; about 770,000 flows went into
hardware, roughly half of them uploads, and a WAN redial was followed in
hardware.

**Uploads.** In the default mode the PPE offloads downloads only: its
parser cannot read past the switch's `rtl8_4` tag. The S-R mode (patches
0460-0468, package `ppe-sr`) has the switch put a per-port S-tag on uploads
so that the PPE can offload them, on every VLAN of its scope. It is
opt-in: the package is not in the default image, its configuration ships
disabled, and it enables nothing on its own. How it was tested:

- one night ACTIVE on the live gateway, about 14 hours (2026-11-05/06),
  with household uploads in hardware and no fault; a speed test at the
  start reached the line's rate;
- a bench bring-up on 2026-11-10, in which its arm and disarm paths passed;
- a bench run on 2026-11-21: uploads and downloads in hardware at the 1G
  client's rate (figures under [Performance](#performance)); it also
  holds at 2.5G;
- the main gateway since 2026-11-21, auto-armed, with a scope of VLAN 1
  on an access port and VLANs 1, 10 and 20 on a trunk: no fault, every
  VLAN with flows in hardware.

S-R has had less fault testing than the default mode: the bench torture
of 2026-11-11 to 2026-11-17 (PPPoE faults, storms, floods, a full flow
table) ran with S-R off.

To use it, add `ppe-sr` (Network) in `make menuconfig`, then:

```sh
uci set ppe_sr.main.enabled='1'      # S-tag switch mode, from the next boot
uci set ppe_sr.main.auto_arm='1'     # arm after the boot holdoff
uci delete ppe_sr.main.class
uci add_list ppe_sr.main.class='lan2 U 1'    # <lanN> <U|T> <vid>
uci add_list ppe_sr.main.class='lan1 T 1'
uci add_list ppe_sr.main.class='lan1 T 10'
uci add_list ppe_sr.main.class='lan1 T 20'
uci set ppe_sr.main.ack_multi_cvid='1'       # needed for more than one VLAN
uci commit ppe_sr && ppe-sr modconf && reboot
```

S-R needs the VLAN-aware `br-lan` (`vlan_filtering 1`). Every VLAN in
the scope needs its L3 device (`br-lan.<vid>`), and every port in a class
must be in `br-lan` and listed in `list ack_port` (lan1 and lan2 by
default). VLANs 3900-3919 are reserved for the switch's steering. Keep ports
that lead to Wi-Fi access points out of the scope (see the roaming entry
under Known limitations). `ppe-sr status` shows the state; `ACTIVE`
means it offloads.

Notes on the software flowtable and the switch ports are in
`target/linux/qualcommbe/docs/flowtable-notes.md`.

## Known limitations

Offload scope, by design:

- In the default mode only IPv4 downloads into the switch on VLAN 1 are
  offloaded; uploads and other VLANs need S-R. In either mode IPv6, Wi-Fi
  clients, hairpin NAT and multicast stay in the software flowtable. S-R
  covers any VLAN of the bridge that has an L3 device (`br-lan.<v>`); a
  scope with more than one VLAN needs `option ack_multi_cvid 1`, and VLANs
  3900-3919 are reserved for the switch. VLANs 1, 10 and 20 have run in
  hardware on the gateway.
- At most 4 PPPoE sessions and 4 bridge MACs are in hardware. Three
  sessions at once ran in hardware on the bench (2026-11-21), and losing
  one moved only its own flows to software. The PPPoE flap breaker is
  shared by all sessions and has no time window: two sessions in a row
  that die within 60 s of going into hardware, however far apart in time,
  keep every session in software until offload is disabled and enabled
  again.
- Neither the PPPoE session match nor the flow key has a VLAN. Frames with
  the session ID from the concentrator's MAC on another VLAN of the WAN
  port, or untagged, and a plain-IP flow's tuple in another encapsulation,
  are forwarded in hardware (measured). Software would drop most of them;
  sending them needs a host on the ISP side of the ONT. `pppoe_offload=0`
  or `flow_offload=0` avoids it.
- Like the software flowtable, the PPE checks neither the PPPoE length
  field nor TCP RST sequence numbers.
- Downloads the PPE forwards skip every SQM shaper, and so do uploads it
  forwards in S-R mode. Without the PPE, flow offloading with SQM on a
  PPPoE or VLAN WAN still skips the download (ingress) shaper: see
  `flowtable-notes.md`.
- LAN frames carry the `rtl8_4` tag past the PPE parser, so LAN ingress
  nearly all lands on one EDMA Rx ring (RPS spreads the protocol work)
  without Rx checksum offload, and frames to the LAN ports go without Tx
  checksum offload or TSO. `lanN` counters count only what crosses the CPU.

Open issues:

- On the production gateway, a firewall reload stalls a download running
  at line rate for 0.5-5 s; fw4 reloads on any firewall change and twice
  after a WAN redial. The bench does not reproduce it (0-200 ms; nothing
  lost where captured). The cause is not known.
- CPU-forwarded TCP can leave with its TTL not decremented while GRO is on
  for the WAN (measured: 81-94% of such frames). It is a generic kernel
  bug in fraglist GRO, not the PPE. Workaround: `ethtool -K wan gro off`.
  A fix is written and not yet in this tree.
- A client that moves from a LAN port to the router's Wi-Fi, to a port of
  the other tagging form, or out of S-R's scope while its old port keeps
  carrier: its hardware downloads go back to software as soon as the
  bridge learns it there, and stay in software until the flowtable's gc
  pass offers them on the new route (`0469`; before it, both modes could
  send them to the old port until the connections ended). It has run in
  the gateway's image since 2026-11-21 but has not triggered there, so its
  decline path is desk-tested only. Wired moves between two switch ports
  of the same form are re-pointed in place, as before (measured on the
  bench in the default mode: cable and MAC-clone moves, 2026-11-13).
  `offload_flows` counts the declines as `fdb_moved`, and
  `offload_sr_status` as `decline_moved`. Keeping ports that lead to
  access points out of S-R's scope avoids the case for S-R.
- In two of five PPPoE session restarts under load on the bench, 6-16% of
  the frames the PPE punted for the dead session (up to about 130) were
  lost in router software; the other three lost none. No flow got stuck.
- Wi-Fi: on 2026-11-07 a live 6 GHz restart crashed the QCN9274 firmware,
  and an ath12k NULL dereference in the `hw_scan` cleanup then rebooted the
  router. The tree carries the two posted fixes (`953-01`, `953-02`). Ten
  6 GHz restart cycles on the bench (20 restarts) caused no crash, but
  the exact production sequence (the radio torn down with a client
  attached) has not been reproduced, so that path of the fix is unproven.
  The firmware crash itself is not explained. Wi-Fi has otherwise run on
  the household gateway since September 2026, all three bands and MLO,
  with this the only crash; with the fixes, on the bench from 2026-11-09
  and on the gateway since 2026-11-21 (channel on auto), no crash.
- After hours of sustained load with many clients, the QCN9274 firmware
  can take a fatal error (reported upstream). The firmware dump is
  released automatically (`/etc/hotplug.d/devcoredump/10-release-coredump`).
  One firmware crash takes the 5 and 6 GHz radios down together.
- In October a simulated firmware crash brought 2.4 GHz back in about
  6.5 s, but the 5 GHz and MLD APs needed `wifi`. Not retested since.
- Removing the 5 GHz link from a running MLD (disabling or reconfiguring
  that radio, or a radar event) has left clients unable to associate on
  the 6 GHz link until `wifi`. On the bench, bouncing either radio brought
  the links back by themselves; association on the remaining link was not
  tested with an MLO client. A similar failure is reported when a client
  is denied on one link at runtime (`hostapd_cli -i ap-mld0_link1
  deny_acl ADD_MAC <mac>`, then `deauthenticate <mac>`); not retested.
- 802.11r is incompatible with MLO: hostapd's FT code has no MLD
  awareness. Do not enable 11r on an MLO SSID. 11k/11v are fine.
- 5 GHz at 160 MHz on blocks with DFS channels can fail chandef
  re-validation after CAC (seen in October). The bench of 2026-11-18/19
  did not reproduce it on 36-64 or 100-128. If you see `Failed to set
  beacon parameters`, use EHT80.
- Switch driver: one bridge is switched in hardware and a second runs in
  software; there is no MDB, so multicast floods per `mcast_flood`
  whatever IGMP snooping decides. The full list is section 7 of
  `package/kernel/rtl8372n/TESTING.md`. The IGMP-snooping issues seen with
  the previous driver (2026-10-04) have not been retested.
- Units with no country in ART: the 6 GHz radio fails to come up with
  country `00`, and `gl-art-country-check` logs a warning at boot. Set it
  by hand: `uci set wireless.radioN.country=XX; uci commit wireless; wifi`.

Untested: a VLAN-unaware `br-lan` (OpenWrt's default layout) with
offload, the WAN at 1G or 100M, a DFS radar event, storms at 2.5G, S-R
under the bench's fault and storm torture, 0469's decline path on
hardware, IPv6 on a real line, other boards and ISPs, and IPQ9574.

## Firmware

- 5/6 GHz (QCN9274): `ath12k-firmware-qcn9274` from linux-firmware
  (WLAN.WBE.1.6-01243, the newest published build).
- 2.4 GHz (IPQ5332): no IPQ5332 firmware is published in linux-firmware yet, so
  the tree carries WLAN.WBE.1.6-01270 as `ath12k-firmware-ipq5332-legacy` (split
  `.mdt` images, see `package/firmware/ath12k-firmware-legacy/PROVENANCE`).

## Building

```sh
git clone -b be9300 https://github.com/kubrickfr/openwrt-be9300.git
cd openwrt-be9300
./scripts/feeds update -a
./scripts/feeds install -a
make menuconfig     # Target System: Qualcomm Atheros 802.11be
                    # Subtarget:     ipq53xx
                    # Target Profile: GL.iNet GL-BE9300
make -j"$(nproc)"
```

The switch driver offloads one bridge and its VLANs to the switch
(`CONFIG_RTL837X_DSA_OFFLOAD`, on by default); `options rtl8372n_dsa
offload=0` in `/etc/modules.conf` keeps every bridge in software. The PPE
defaults are under [Flow offloading](#flow-offloading).

Images land in `bin/targets/qualcommbe/ipq53xx/`. The build fails if the
kernel image is larger than the 7 MiB `0:HLOS` partition, rather than
producing an image that cannot boot.

Pre-built images are on the
[perceival/openwrt-flint3 Releases page](https://github.com/perceival/openwrt-flint3/releases).
They are built from perceival's tree, not this one. If your configuration
uses `dhcp.odhcpd.maindhcp=1` (his router images do), read
`target/linux/qualcommbe/docs/MIGRATING-FROM-PERCEIVAL.md` before flashing
an image from this tree with that configuration kept: it would leave the
LAN without DHCPv4.

## Installing

Full instructions, including the round trip back to stock, are on the
device page: **https://openwrt.org/toh/gl.inet/gl-be9300**

Short version: from stock firmware, use the **factory** image with
`sysupgrade -F -n`. The stock image check requires a QSDK FIT, so `-F` is
required and the "missing section" warnings for `u-boot`/`tz`/`sb11` are
expected. Do **not** force the plain sysupgrade image from stock.

Stock may report `qcom,ipq5332-ap-mi01.6` as its board name. That is the
generic Qualcomm RDP468 identity, deliberately not in this profile's
`SUPPORTED_DEVICES`, so the compatibility warning is expected
([perceival/openwrt-flint3#9](https://github.com/perceival/openwrt-flint3/issues/9)).

**Back up your eMMC first.**

## Differences from perceival/openwrt-flint3

Both trees evolve in parallel. This one was rebased from his `flint3-be9300`
as of 2026-09-22 (`b12c85403f`), with some later work picked since.

- **Switch driver:** the SDK-free RTL8372N DSA driver (from
  perceival/openwrt-flint3#104) with the native `rtl8_4` tag and bridge and
  VLAN offload, in place of the SDK-based driver and `tag_8021q`.
- **PPE flow offload:** his table primitives and IPv4 engine are carried as
  0433/0434, under his authorship, with a safety layer, PPPoE download
  offload and an egress-only emulation of the `rtl8_4` tag on top. His
  ingress parser alias and IPv6 path are not carried. See
  `target/linux/qualcommbe/docs/ppe-flow-offload/PROVENANCE.md`.
- **IPQ5332 UserPD boot:** the legacy boot-info path with the
  reference-board reserved-memory layout, as his tree had at the rebase
  point. His later vendor Q6 memory contract cannot be mixed with it and
  is not carried; SMEM item 507 is written once at probe.
- **Not carried:** the FT-over-MLO hostapd series (992–999a). DHCP keeps
  upstream's defaults (dnsmasq and odhcpd-ipv6only).

## Upstream

Patches from this work that have gone upstream or are in review:

- `wifi: ath12k: advertise AP_VLAN interface mode for IPQ5332` (linux-wireless)
- hostapd WDS/AP_VLAN `bss->ctx` fix (applied by Jouni Malinen)
- ath12k `hw_scan` NULL dereference after a firmware crash: reported

Carried from upstream review: the ath12k `hw_scan` fixes by Chia-Lin Kao
(AceLan) and Jose Ignacio Tornos Martinez (`953-01`, `953-02`).

## Links

- Forum thread: https://forum.openwrt.org/t/gl-inet-flint-3-exploration-gl-be9300-ipq5332/250267
- Device page: https://openwrt.org/toh/gl.inet/gl-be9300

## Credits

Kamil Bienkiewicz (perceival) maintains the Flint 3 port this tree is
rebased from. He wrote the PPE flow offload primitives (0433/0434), the EDMA
checksum fallback (0431), the earlier rtl837x VLAN fixes, the `tag_8021q`
bridge-VID patch (766, since replaced by the flowtable patch 716), the
coredump release handler, the faster switch bring-up and the ARMv8 crypto
configuration carried here.

Maksims Neroba wrote the SDK-free RTL8372N DSA driver the switch package is
based on (perceival/openwrt-flint3#104), adapting Airjinkela's DSA refactor;
his work found the switch's table locking, STP encoding and L2 flush
completion bit used by the offload (see `package/kernel/rtl8372n/PROVENANCE.md`).

JiaY-shi's GL-BE6500 tree provided the working IPQ5332 Wi-Fi and RTL837x DSA
foundation, and Til Kaiser the IPQ53xx subtarget series. Chia-Lin Kao
(AceLan) and Jose Ignacio Tornos Martinez wrote the ath12k `hw_scan` fixes
carried as `953-01` and `953-02`.
[François Guerraz](https://github.com/kubrickfr) maintains this rebase.
Thanks to everyone contributing hardware findings and testing via the issue
trackers and the forum thread.
