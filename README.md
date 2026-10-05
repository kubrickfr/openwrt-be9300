# OpenWrt for the GL.iNet Flint 3 (GL-BE9300)

Mainline **OpenWrt** support for the **GL.iNet Flint 3 (GL-BE9300)** — Qualcomm
**IPQ5332** (quad Cortex-A53) with tri-band Wi-Fi 7, a Realtek **RTL8372N** 10G
switch and a **RTL8221B** 2.5G WAN PHY.

> **This branch (`be9300`) is a complete, buildable OpenWrt tree.**
> Clone it and build — there is nothing to drop into another checkout.
> `be9300-next` is where the series is rebased onto newer upstream `main`;
> `be9300` follows it once the result has been tested on hardware.

## Where this tree comes from

This repository carries
[perceival's Flint 3 port](https://github.com/perceival/openwrt-flint3)
(branch `flint3-be9300`), to which
[François Guerraz](https://github.com/kubrickfr) contributed, rebased onto
upstream OpenWrt `main`. That port was itself built on
[JiaY-shi's GL-BE6500 (Flint 3e) work](https://github.com/JiaY-shi/openwrt),
which provided the IPQ5332 Wi-Fi and RTL837x DSA foundation.

The rebase is not a plain replay. The series was reorganised and refreshed
for current upstream, picks up newer ath12k and hostapd fixes from JiaY-shi's
tree, and adds further GL-BE9300 work: PPE clocking, Wi-Fi MAC addresses,
the regulatory country and fan control.

The goal of this project is simply to have a version that tracks upstream
more closely: the same GL-BE9300 support, carried as a curated patch series
on a recent upstream `main` instead of an older fork point. Hopefully this
makes the work easier to keep current, to review and to send upstream, and
helps the community.

Target: **`qualcommbe/ipq53xx`**, kernel **6.18**.

The tree is upstream OpenWrt `main` with a curated series on top: the IPQ53xx
subtarget from [openwrt/openwrt#23161](https://github.com/openwrt/openwrt/pull/23161),
the IPQ5332 platform, ath12k and PPE patches, the RTL837x DSA driver and the
GL-BE9300 device support. The GL-BE6500 (Flint 3e) and UniFi U7 Pro XGS
profiles of the trees it grew from are not carried.

> [!WARNING]
> **Unofficial, community-maintained port — not affiliated with, endorsed by, or supported
> by GL.iNet or the OpenWrt project.** Provided **as-is, with no warranty of any kind**.
> Flashing third-party firmware carries real risk, including bricking the device, and may
> void your hardware warranty. **Back up your eMMC first** — the ART partition holds your
> unit's unique radio calibration data and MAC addresses and cannot be recovered from
> anywhere else. If this router matters to you, test on a spare unit before relying on it.

## Hardware

| Block | Detail |
|---|---|
| SoC | Qualcomm IPQ5332, 4× Cortex-A53 |
| Wi-Fi 2.4 GHz | on-SoC radio, ath12k over AHB |
| Wi-Fi 5 / 6 GHz | 2× QCN9274, ath12k over PCIe |
| Switch | RTL8372N, out-of-tree DSA driver (`realtek,rtl837x`); SoC↔switch link is 10GBASE-R |
| WAN | RTL8221B 2.5G PHY, `2500base-x` to the SoC |
| Storage | eMMC |

## Status

| Subsystem | State |
|---|---|
| Boot / procd / SSH | working |
| LAN (RTL8372N via DSA + EDMA/PPE) | working |
| WAN (2.5G) | working at 2.5 Gbps; 1G/100M link rates untested |
| VLANs (bridge-vlan on DSA) | working, with one VLAN-filtering bridge on the switch ports (see Known issues) |
| Flow offloading | software flowtable only; no PPE hardware flow offload (see below) |
| Wi-Fi 7, all three bands | working |
| MLO (AP MLD across 2.4/5/6 GHz) | working |
| DFS | working: CAC and secondary AP after CAC tested (a radar event itself not yet tested) |
| 802.11k / 802.11v | working |
| eMMC sysupgrade + return to stock | working |

Throughput measured between two units over a 2.5G trunk: **~1.8–1.9 Gbit/s**.
Through a PPPoE WAN with software flowtable offload: **2.04 Gbit/s down**
(4 TCP streams, PPE clock 200 MHz, 2026-10-04), with no drops in the
EDMA/PPE counters. The busiest core was at 78% softirq.

The kernel's AES and GCM use the ARMv8 Crypto Extensions (`aes-ce`,
`gcm-aes-ce`; `39df06d866`, checked 2026-10-04), which helps IPsec, OpenVPN
DCO, dm-crypt and SMB3 encryption. WireGuard does not use AES and is
unaffected.

Wi-Fi MAC addresses come from the ART partition (offset 0x04, as on stock);
the secondary links of an MLD get locally administered addresses derived
from it (`/etc/uci-defaults/99-gl-be9300-wifi-macs`).

## Flow offloading

Use **software** flow offloading only:

```sh
uci set firewall.@defaults[0].flow_offloading='1'
uci set firewall.@defaults[0].flow_offloading_hw='0'
uci commit firewall && fw4 reload
```

- This tree has **no PPE hardware flow offload**: no driver implements
  flowtable offload (`TC_SETUP_FT`).
- fw4 still accepts `flow_offloading_hw=1`, because pending patch 701 makes
  its probe pass. The flowtable is then built on `lan1`–`lan4` and the Wi-Fi
  interfaces instead of `br-lan`, and flows to wired clients are sent straight
  to a switch port.
- With patch 766, such flows through the default VLAN-unaware `br-lan` work
  (tested 2026-10-04), but nothing is gained, and there is a cost. Such a
  flow stays pinned to the port where the bridge last saw the client. In testing, a download to a client on `lan1`
  stalled for as long as `lan4` was down, because the bridge had learned the
  client on `lan4`. With `flow_offloading_hw=0` the same test did not stall.
- 766 does not cover ports of a VLAN-filtering bridge: with VLAN filtering
  and `flow_offloading_hw=1`, put the LAN on a `br-lan.N` VLAN interface, not
  directly on `br-lan`.
- A PPPoE WAN cannot be hardware-offloaded even with the experimental PPE
  offload series in perceival/openwrt-flint3: that driver declines PPPoE in
  both directions.

## Firmware

- 5/6 GHz (QCN9274): `ath12k-firmware-qcn9274` from linux-firmware
  (WLAN.WBE.1.6-01243, the newest published build).
- 2.4 GHz (IPQ5332): no IPQ5332 firmware is published in linux-firmware yet, so
  the tree carries WLAN.WBE.1.6-01270 as `ath12k-firmware-ipq5332-legacy` (split
  `.mdt` images, see `package/firmware/ath12k-firmware-legacy/PROVENANCE`).

## Known issues

- **ath12k firmware hang under sustained load.** After hours with many clients
  the Q6 can take a fatal error. Reported upstream. The firmware dump is now
  released automatically (`/etc/hotplug.d/devcoredump/10-release-coredump`),
  so recovery no longer waits 5 minutes for it to be collected. In a simulated
  crash (2026-10-04) the 2.4 GHz AP was back after about 6.5 s, but the 5 GHz
  AP and the MLD AP did not come back until `wifi` was run. Under
  investigation.
- **Removing a link from a running MLD.** Disabling or reconfiguring the
  5 GHz radio removes its link from the MLO network, after which clients can
  no longer complete the handshake on the remaining 6 GHz link until Wi-Fi is
  restarted (`wifi`). A radar event that forces a new CAC may take the same
  path. A similar failure is reported without any reconfiguration, when a
  client is denied on one link at runtime:
  `hostapd_cli -i ap-mld0_link1 deny_acl ADD_MAC <mac>` then
  `hostapd_cli -i ap-mld0_link1 deauthenticate <mac>` (undo with
  `hostapd_cli -i ap-mld0_link1 deny_acl DEL_MAC <mac>`). The client authenticates on the 6 GHz link and
  never associates. Under investigation; not yet retested on this tree.
- **5 GHz at 160 MHz.** Blocks that include DFS channels (36–64, 100–128) can
  fail chandef re-validation after CAC (`Failed to set beacon parameters`,
  interface down). Use EHT80 on 5 GHz; 6 GHz is fine at 160 MHz.
- **802.11r is incompatible with MLO.** hostapd's FT code has no MLD
  awareness — do not enable 11r on an MLO SSID. 11k/11v are fine. An
  experimental FT-over-MLO hostapd series exists in perceival/openwrt-flint3
  (patches 992–999a); it is not carried here.
- **VLAN filtering next to another bridge.** The switch has one global VLAN
  table, and isolation between bridges relies on VLAN membership alone. A
  VLAN-filtering bridge and any other bridge on the switch ports that share a
  VID (VLAN 1 by default) are joined in hardware on that VID: in testing
  (2026-10-04),
  broadcasts from a VLAN-filtering `br-lan` reached a port in a second bridge.
  Standalone ports are not affected. Until this is fixed, put all switch ports
  that use VLAN filtering in a single bridge, and use `br-lan.N` VLAN
  interfaces rather than more bridges.
- **IGMP snooping.** With `igmp_snooping` enabled on `br-lan`, the router's
  IGMP queries reached `lan1` but not `lan4` in testing (2026-10-04).
  Snooping is off by default; leave it off.
- **Unprogrammed ART country.** The regulatory country is read from the ART
  partition at first boot. Some units have none, and their 6 GHz radio then
  fails to come up with country `00`. When ART has no country and a radio has
  none configured either, `gl-art-country-check` logs a warning at boot. Set
  the country by hand:
  `uci set wireless.radioN.country=XX; uci commit wireless; wifi`
  (for each radio, with your two-letter country code).

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

Images land in `bin/targets/qualcommbe/ipq53xx/`. The build fails if the
kernel image is larger than the 7 MiB `0:HLOS` partition, rather than
producing an image that cannot boot (`KERNEL_SIZE` in
`target/linux/qualcommbe/image/ipq53xx.mk`).

### Don't want to build from source?

Pre-built images are published on the
**[perceival/openwrt-flint3 Releases page](https://github.com/perceival/openwrt-flint3/releases)**.
They are built from perceival's own tree, not from this one, and differ from
it (see [Differences](#differences-from-perceivalopenwrt-flint3)). If your
configuration uses `dhcp.odhcpd.maindhcp=1` (perceival's router images do),
read [Migrating](#migrating-from-perceivalopenwrt-flint3-images) before
flashing an image built from this tree with that configuration kept.

See the disclaimer above before flashing any of them.

## Differences from perceival/openwrt-flint3

Both trees evolve in parallel. This tree was rebased from perceival's
`flint3-be9300` as of 2026-09-22 (`b12c85403f`), and some later work was
picked from it since.

- **Not carried:** the PPE hardware flow offload series, the native `rtl8_4`
  switch tag (this tree uses `tag_8021q` only), the FT-over-MLO hostapd series
  (992–999a), and the vendor Q6 memory contract described below.
- **IPQ5332 UserPD boot.** ath12k describes the UserPD to the RootPD through
  the legacy boot-info path (DT `qcom,legacy-userpd-bootinfo`, patches
  903-02a/903-02b), with the reference-board reserved-memory layout:
  `q6_region` at 0x4a900000 (43 MiB), `m3-dump` at 0x4d400000, `q6-caldb` at
  0x4d500000, `ramoops` at 0x4da00000 (`ipq5332-gl-be9300.dts` and kernel
  patch 2012). This was perceival's own design up to
  the rebase point. The vendor Q6 memory contract (named regions, SMEM-507
  boot arguments refreshed on every load) reached his tree on 2026-09-23
  (merge `50800256ec`, with `c7d1cf7b66`) and
  is not carried here: it cannot be mixed with the legacy path. This was a
  deliberate choice, not an omission. Known residual: SMEM item 507 is written
  once at probe, not on every firmware reload.
- **DHCP:** upstream defaults (dnsmasq + odhcpd-ipv6only), where his router
  images use full odhcpd as the DHCPv4 server. See below.

## Migrating from perceival/openwrt-flint3 images

This tree keeps upstream OpenWrt's DHCP setup: dnsmasq serves DHCPv4 and DNS,
and odhcpd-ipv6only serves RA and DHCPv6. perceival's router images run full
odhcpd as the DHCPv4 server (`dhcp.odhcpd.maindhcp=1`), reportedly with no
dnsmasq section. This section applies to any kept configuration with
`maindhcp=1`. odhcpd-ipv6only has no DHCPv4 and silently ignores
`dhcpv4='server'`, and dnsmasq generates no DHCP ranges while `maindhcp=1`.
A kept configuration therefore leaves the LAN **without DHCPv4**, and without
DNS if there is no dnsmasq section. Before flashing, make sure you can reach
the router with a static address.

**Option A — upstream defaults**, run after the first boot. If unbound
serves your DNS, read the unbound paragraph below before running it.

```sh
uci set dhcp.odhcpd.maindhcp='0'
uci -q show dhcp | grep -q '=dnsmasq$' || {
	uci add dhcp dnsmasq >/dev/null
	for o in domainneeded=1 boguspriv=1 localise_queries=1 rebind_protection=1 \
		rebind_localhost=1 local=/lan/ domain=lan expandhosts=1 cachesize=1000 \
		authoritative=1 readethers=1 leasefile=/tmp/dhcp.leases \
		resolvfile=/tmp/resolv.conf.d/resolv.conf.auto nonwildcard=1 \
		localservice=1 ednspacket_max=1232; do
		uci set dhcp.@dnsmasq[0].$o
	done
}
uci set dhcp.lan.dhcpv4='server'
uci commit dhcp && /etc/init.d/odhcpd restart && /etc/init.d/dnsmasq restart
```

Also set `dhcpv4='server'` on every other `config dhcp` section that should
serve DHCPv4: dnsmasq treats a missing option as disabled (the defaults above
are those of `package/network/services/dnsmasq/files/dhcp.conf`). Check that
`/var/etc/dnsmasq.conf.*` now has a `dhcp-range=set:lan,…` line.

If unbound serves DNS, use the "parallel dnsmasq" setup from unbound's
README (`feeds/packages/net/unbound/files/README.md`): unbound keeps port 53
and asks dnsmasq, moved to port 1053, for the names of DHCP clients. Before
the `uci commit dhcp` line above:

```sh
uci set dhcp.@dnsmasq[0].port='1053'
uci set dhcp.@dnsmasq[0].noresolv='1'
uci add_list dhcp.lan.dhcp_option='option:dns-server,0.0.0.0'
uci set unbound.@unbound[0].dhcp_link='dnsmasq'
uci commit unbound
```

and afterwards `/etc/init.d/unbound restart`. Add the same `dhcp_option` to
every other section that serves DHCPv4.

**Option B — keep odhcpd as the DHCPv4 server:** build with
`CONFIG_PACKAGE_odhcpd=y`, which replaces odhcpd-ipv6only in the image
(upstream `1f7eb34c53`; checked with `make defconfig` on 2026-10-05). Then
`maindhcp=1` configurations work unchanged, but DNS still needs a server:
unbound, or a dnsmasq section (which then serves DNS only). Packages
installed with apk do not survive a sysupgrade, so build it in.

## Installing

Full, hardware-verified instructions — including the round trip back to stock —
are on the device page:

**https://openwrt.org/toh/gl.inet/gl-be9300**

Short version: from stock firmware, use the **factory** image with
`sysupgrade -F -n`. The stock image check requires a QSDK FIT, so `-F` is
required and the "missing section" warnings for `u-boot`/`tz`/`sb11` are
expected. Do **not** force the plain sysupgrade image from stock.

Stock QSDK firmware may report `qcom,ipq5332-ap-mi01.6` as its board name.
That is the generic Qualcomm MI01.6/RDP468 identity used by the vendor path,
not the OpenWrt GL-BE9300 device identifier. The same compatible is used by
the upstream Qualcomm RDP468 device tree, so it is intentionally not added to
this profile's `SUPPORTED_DEVICES`: doing so would advertise the Flint 3 image
as compatible with other hardware using that generic identity. The resulting
stock compatibility warning is therefore expected; use the documented `-F`
factory-image path instead (see [perceival/openwrt-flint3#9](https://github.com/perceival/openwrt-flint3/issues/9)).

**Back up your eMMC first** — the ART partition holds this unit's radio
calibration and MAC addresses and cannot be recovered from anywhere else.

## Upstream

Patches from this work that have gone upstream or are in review:

- `wifi: ath12k: advertise AP_VLAN interface mode for IPQ5332` (linux-wireless)
- hostapd WDS/AP_VLAN `bss->ctx` fix (applied by Jouni Malinen)
- ath12k `hw_scan` NULL-deref report (with the Qualcomm dev team)

## Links

- Forum thread: https://forum.openwrt.org/t/gl-inet-flint-3-exploration-gl-be9300-ipq5332/250267
- Device page: https://openwrt.org/toh/gl.inet/gl-be9300

## Credits

Kamil Bienkiewicz (perceival) wrote the rtl837x VLAN ingress and
standalone-VLAN fixes, the `tag_8021q` bridge-VID patch (766), the coredump
release handler, the faster switch bring-up and the ARMv8 crypto
configuration carried here.

This tree is a rebase of [perceival's](https://github.com/perceival/openwrt-flint3)
Flint 3 port and the work of its contributors, among them
[François Guerraz](https://github.com/kubrickfr), who maintains this rebase.
That port was built on
[JiaY-shi's](https://github.com/JiaY-shi/openwrt) GL-BE6500 tree (the working
IPQ5332 Wi-Fi and RTL837x DSA foundation) and on Til Kaiser's IPQ53xx subtarget
series. Thanks to everyone contributing hardware findings and testing via the
issue trackers and the forum thread.
