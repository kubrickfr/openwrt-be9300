# OpenWrt for the GL.iNet Flint 3 (GL-BE9300)

Mainline **OpenWrt** support for the **GL.iNet Flint 3 (GL-BE9300)** — Qualcomm
**IPQ5332** (quad Cortex-A53) with tri-band Wi-Fi 7, a Realtek **RTL8372N** 10G
switch and a **RTL8221B** 2.5G WAN PHY.

> **This branch (`be9300`) is a complete, buildable OpenWrt tree.**
> Clone it and build — there is nothing to drop into another checkout.

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
| WAN | RTL8221B 2.5G, USXGMII |
| Storage | eMMC |

## Status

| Subsystem | State |
|---|---|
| Boot / procd / SSH | working |
| LAN (RTL8372N via DSA + EDMA/PPE) | working |
| WAN (2.5G, USXGMII) | working, links at 2.5 Gbps |
| VLANs (bridge-vlan on DSA) | working |
| Wi-Fi 7, all three bands | working |
| MLO (AP MLD across 2.4/5/6 GHz) | working |
| DFS | working: CAC and secondary AP after CAC tested (a radar event itself not yet tested) |
| 802.11k / 802.11v | working |
| eMMC sysupgrade + return to stock | working |

Throughput measured between two units over a 2.5G trunk: **~1.8–1.9 Gbit/s**.
Through the WAN (PPPoE, software flowtable offload), 2.2 Gbit/s down and
0.8 Gbit/s up, the subscribed line rate, with no drops in the EDMA/PPE
counters.

## Firmware

- 5/6 GHz (QCN9274): `ath12k-firmware-qcn9274` from linux-firmware
  (WLAN.WBE.1.6-01243, the newest published build).
- 2.4 GHz (IPQ5332): no IPQ5332 firmware is published in linux-firmware yet, so
  the tree carries WLAN.WBE.1.6-01270 as `ath12k-firmware-ipq5332-legacy` (split
  `.mdt` images, see `package/firmware/ath12k-firmware-legacy/PROVENANCE`).

## Known issues

- **ath12k firmware hang under sustained load.** After hours with many clients
  the Q6 can take a fatal error; radios stay down until reboot. Reported
  upstream.
- **Removing a link from a running MLD.** Disabling or reconfiguring the
  5 GHz radio removes its link from the MLO network, after which clients can
  no longer complete the handshake on the remaining 6 GHz link until Wi-Fi is
  restarted (`wifi`). A radar event that forces a new CAC may take the same
  path. Under investigation.
- **5 GHz at 160 MHz.** Blocks that include DFS channels (36–64, 100–128) can
  fail chandef re-validation after CAC (`Failed to set beacon parameters`,
  interface down). Use EHT80 on 5 GHz; 6 GHz is fine at 160 MHz.
- **802.11r is incompatible with MLO.** hostapd's FT code has no MLD
  awareness — do not enable 11r on an MLO SSID. 11k/11v are fine.

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

Images land in `bin/targets/qualcommbe/ipq53xx/`.

### Don't want to build from source?

Pre-built reference images of the original, pre-rebase tree are published
periodically on the
**[perceival/openwrt-flint3 Releases page](https://github.com/perceival/openwrt-flint3/releases)**,
in three flavours:

- **`vanilla`** — the exact, unmodified default this tree produces with zero customization
  (no LuCI, `wpad-basic-mbedtls`) — what you'd get building it yourself with no changes
- **`ap`** — full config (LuCI, tri-band MLO) plus the FT-over-MLO roaming series; what the
  maintainer's own household runs
- **`router`** — gateway role: LuCI, software nftables flowtable offload (not silicon-level
  hardware NAT acceleration — see [perceival/openwrt-flint3#1](https://github.com/perceival/openwrt-flint3/issues/1)),
  WireGuard, unbound, chrony, mDNS reflection

See the disclaimer above before flashing any of them.

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

This tree is a rebase of [perceival's](https://github.com/perceival/openwrt-flint3)
Flint 3 port and the work of its contributors, among them
[François Guerraz](https://github.com/kubrickfr), who maintains this rebase.
That port was built on
[JiaY-shi's](https://github.com/JiaY-shi/openwrt) GL-BE6500 tree (the working
IPQ5332 Wi-Fi and RTL837x DSA foundation) and on Til Kaiser's IPQ53xx subtarget
series. Thanks to everyone contributing hardware findings and testing via the
issue trackers and the forum thread.
