# RTL8372N DSA driver: provenance

Who wrote what, the public inputs, and the source and confidence of each
register fact. Public availability, a licence or matching values do not prove
independent origin; rows marked **SDK-only lineage, disposition open** need a
maintainer decision before submission.

## Who wrote what

- **Airjinkela (StarField Xu)**, [rtl837x-dsa-driver](https://github.com/airjinkela/rtl837x-dsa-driver/tree/7f3b64c7f3a37db1866fd80ba815d89974d995fd):
  the DSA refactor this driver starts from (switch ops, phylink, MDIO
  transport). His copyright stays in `rtl8372n.c`, `rtl837x_mdio.c`,
  `rtl837x.h` and `src/Makefile`; the reworked `rtl837x_common.c` says
  "Portions adapted". He disclosed SDK-derived parts without naming them.
- **Maksims Neroba**: P0, the SDK-free driver of
  [perceival/openwrt-flint3#104](https://github.com/perceival/openwrt-flint3/pull/104)
  at `7e51247b3882567ce891481395b34e2a2c25f116`, squashed here as "rtl8372n:
  replace the SDK-based driver with an RTL8372N DSA driver"
  (probe, soft reset, register/PHY/SerDes access, 10GBASE-R CPU port, RTL8_4
  tagging, CPU-only isolation), benched on a GL-BE9300: links on all four
  jacks, six LAN pairs through a software bridge, reboots, 33 setup
  readbacks, CPU-only isolation of one standalone port. P1:
  - [MNeroba/openwrt-flint3#1](https://github.com/MNeroba/openwrt-flint3/pull/1)
    (`b6c2da893e1d7a6427dab737e1c56e21cccf82ab`, GPL-2.0-only): table mutex,
    idle wait, single command write, verified VLAN write (`rtl837x_table.c`).
  - [MNeroba/openwrt-flint3#2](https://github.com/MNeroba/openwrt-flint3/pull/2)
    (`847e9f79039b58ed6674502b64f6f2b2b24a251a`, GPL-2.0-only): L2 key lookup
    and hit check, per-port flush (BUSY-only completion, mode restored only
    when idle), CIST helper and state mapping (`rtl837x_table.c`,
    `rtl8372n_l2.c`). On a BE9300: CIST readback 0/1/1/2/3 and 0 to 3 across
    a down/up; flushes on ports 4-7 completed; a static multicast entry
    survived a mode-0 flush of another port; BPDUs reached the CPU at lan4 in
    CIST states 1-3 (with his static BPDU entry, unused here, in place).
  - [airjinkela/rtl837x-dsa-driver#2](https://github.com/airjinkela/rtl837x-dsa-driver/pull/2)
    (`15ff009cb46c27a443d1f1334237807b212bf9c4`, closed): RMA/PTP
    trap-destination analysis, and airjinkela's hardware comment that trapped
    frames reach the CPU port. Informed the trap setup; no code.
- **This branch** (François Guerraz, with Claude; commits named by subject,
  as hashes change on a rebase): "rtl8372n: make standalone ports
  VLAN-transparent"; "rtl8372n: drive the internal PHYs with the Realtek PHY
  driver", internal PHYs as C45 devices bound to the kernel RTL8224 driver
  (P0's PHY layer and its C22 page emulation, after Linux `r8169`, are gone);
  "rtl8372n: keep EEE off and leave internal PHY power to phylib"; tagger
  patch 0432; "rtl8372n: offload bridging and VLANs to the switch".
- **Kamil Bienkiewicz**: patch 0431 (0443 in perceival's tree): EDMA falls
  back to software checksum/GSO when its parser cannot find L3, which the
  RTL8_4 tag breaks. Outside this package; required by it.
- **Replaced**: the SDK-based rtl837x package by JiaY-shi and others, as on
  `be9300-next` before this series; comparison only. "The SDK-based driver"
  below is the revision pinned in the table (perceival/openwrt-flint3
  `2365932733`); this tree's copy carried later fixes on top of it.

## Pinned public inputs

| Input | Revision, licence | Use |
| --- | --- | --- |
| [Airjinkela DSA](https://github.com/airjinkela/rtl837x-dsa-driver/tree/7f3b64c7f3a37db1866fd80ba815d89974d995fd) | `7f3b64c7`, GPL-2.0-or-later headers | Architecture adapted, attribution kept. Its generated register header and PHY/SerDes patch arrays are excluded. |
| [RTLPlayground](https://github.com/logicog/RTLPlayground/tree/f0aea3dcac056e3274fd39e1c76a7117471c37da) | `f0aea3dc`, MIT | Register/operation comparison; notice at the end. Corroboration, not proof of independent research. |
| [ZTE RTL8372N driver](https://github.com/cnjn/linux-mainline-zte-zxslc-sr1010/blob/07f8687248578d4be6931c665ff5d08bb6cc3d9d/drivers/net/ethernet/zte/zx279133-rtl8372n.c) | `07f86872`, GPL-2.0 | Register facts compared. Its CPU-port-8 mode, reset, patch and polarity sequence is board-specific and not imported. Owner provenance unreviewed. |
| Linux v6.18 `drivers/net/phy/realtek/realtek_main.c` | GPL-2.0+ | Drives the internal PHYs (RTL8224 entry, ID `0x001ccad0`); this package carries no PHY register data. |
| Linux v6.18 MDIO/PHY core (`mdio.h`, `mdio_bus*.c`, `of_mdio.c`, `phy_device.c`, `drivers/base/dd.c`) | GPL-2.0 | Public C45 registration (`get_phy_device(..., true)`) and the `device_is_bound()` check. |
| Linux v7.2 `rtl8365mb`/`rtl83xx`, backports `target/linux/generic/backport-6.18/942-05` to `942-09` | GPL-2.0 / GPL-2.0+ | Followed, not copied: table transaction shape, VLAN callbacks and no-PVID handling, bridge join/leave, FDB and bridge-flag callbacks, assisted learning. |
| Linux v6.18 DSA and bridge core, `sja1105_prechangeupper()`, `tag_brcm`; `tag_mxl862xx` (v7.0, backport `762`) | GPL-2.0 / GPL-2.0+ | Call order checked; 8021q-upper refusal and the link-local rule of 0432 written anew after these patterns. |
| RTL8373 SDK (`src/rtk-api`) and the SDK-based driver, [perceival/openwrt-flint3 `2365932733`](https://github.com/perceival/openwrt-flint3/tree/2365932733ca8ec3b346621d9cec2eb3df3b2cf3/package/kernel/rtl837x/src) | SDK files: Realtek proprietary ("All Rights Reserved"); driver files: GPL-2.0 header | Comparison only, nothing imported. The source of every row below citing "SDK". |

## Register and feature ledger

"HW" is what has run on a BE9300; "stock" is GL 4.10.1 corroboration (next
section). Everything else is unmeasured.

| Registers / feature | Source | Status |
| --- | --- | --- |
| MDIO regs 21-24, commands `0x19`/`0x1b`, BUSY | ZTE `rtl8372n_wait_ready/read_reg/write_reg`; Air `rtl837x_mdio.c` | Public. HW: P0. Reset-writer semantics unconfirmed. |
| Chip ID `0x0004`, reset `0x0024`, accept `0x837270xx` | RTLPlayground `rtl837x_regs.h`; ZTE | Public. HW: P0 probe. |
| PHY indirect `0x6438`-`0x6444`, status bits 24-26, C45 command | ZTE `rtl8372n_phy_read/write`; RTLPlayground | Public, reimplemented. HW: P0. Timeout budget unconfirmed. |
| PHY power OCP `0xa610` = `0x2058` up / `0x2858` down | RTLPlayground power ops; Air | Public; stock uses the same values. Only `0x2858` is written, across reset and teardown; phylib owns the bit otherwise (it mirrors PMA/PMD control 1). |
| Internal PHY binding (C45, RTL8224) | Linux `get_phy_device()`, `realtek_main.c` | Upstream. HW: same binding observed under the SDK-based driver. |
| SMI polling / MAC type `0x632c`/`0x6330`/`0x6334`/`0x6454` | RTLPlayground init; ZTE | Public; stock init writes the same. HW: P0. |
| MAC status `0x63e8` link, `0x63f0`/`0x63f4` speed, `0x63f8` duplex, `0x63fc`/`0x6400` pause | SDK `dal_rtl8373_portStatus_get()` | **SDK-only lineage, disposition open.** Read only, for the SerDes PCS state of ports 3 and 8; no value recorded on HW. |
| SDS indirect `0x03f8`/`0x03fc`/`0x0400` | Air helper; ZTE helper; RTLPlayground | The three agree; Air's lineage disclosed SDK-derived, ZTE's unreviewed. HW: P0. |
| SDS mode `0x7b20` = `0x1a`; polarity page 0 reg 0 bits 8 (TX)/9 (RX), page 6 reg 2 bits 14 (TX)/13 (RX) | RTLPlayground mode constants, SDS init, OEM notes; Air mode mapping; ZTE | Behaviour corroborated, not a clean source. Lane mapping and each flag's effect not isolated; stock sets 3 bits per SerDes (not page 0 bit 9). |
| RTL8_4 CPU tag `0x603c`/`0x6720`/`0x6724` | RTLPlayground; Air `set_tag_rtl`; Linux `tag_rtl8_4.c`; SDK `CPU_TAG_AWARE_CTRL` | Public; the tag-aware mask layout (bit 9 = 8051) is **SDK-only lineage, disposition open**. HW: P0 traffic. Stock sets only `0x6724` = 3. |
| VLAN 1, table `0x5cac`/`0x5cb8`, read `0x5ccc`, PVID `0x4e1c`, filters `0x4e14`/`0x4e18` | RTLPlayground `vlan_setup`/`vlan_get`; ZTE | Public; stock uses the same layout and filters. HW: VLAN 1 written and read (P0, MNeroba#2). |
| Table engine fields (`0x5cac` addr/type/write/busy, `0x5cb0` method 17:14, ENTRY_CLR 18, hit 12, index 11:0) | RTLPlayground table section, `doc/l2.md`; ZTE; MNeroba#1/#2 | HW: VLAN write/read, L2 multicast write and method-0 lookup (MNeroba#2). 13-bit address and 3-bit type widths, method-7 port filter `0x5cb0` 22:19: **SDK-only lineage, disposition open**. `0x5cb0` bit 13: SDK-only, meaning unknown (ends an FDB walk with a warning). |
| VLAN entry bits 23:20 FID/MSTI, 24 SVLAN check, 25 IVL | Air `ivl_en`, ZTE `VLAN_IVL` (bit 25); SDK (23:20, 24) | Bit 25 public (RTLPlayground's prose calls it "valid"); bits 20-24 **SDK-only lineage, disposition open**. Stock writes FID 0, SVLAN 0, and IVL 1 except on VIDs 2-8. |
| L2 unicast entry (MAC, VID/FID 27:16, IVL 29, L3 28, port 31:30 + word 2 1:0, age 4:2, auth 5, static 16) | ZTE encode/program/delete; RTLPlayground L2 dump; SDK `_rtl8373_fdbStUser2Smi` | Add writes age 6, auth 1. In-place rewrite of a learned entry, and learning never setting static: **SDK-only**. No unicast entry on HW yet; the setup self-test is the first. |
| L2 table clear `0x53d8` bit 0 | SDK `dal_rtl8373_l2_table_clear` | **SDK-only lineage, disposition open.** Used by the SDK-based driver without polling. |
| Admission `0x4e10`, egress tag mode `0x6738` (2 bits per port) | RTLPlayground; ZTE (addresses, layout); rtl8365mb by analogy and SDK (encodings) | ORIGINAL 0 corroborated by stock; REAL_KEEP 3 not. Admission 1 (TAG_ONLY): **SDK-only lineage, disposition open**, never run. |
| Egress transparency `0x4eb8 + (port / 3) * 4` | SDK `dal_rtl8373_vlan_portTransparent_set` only; mechanism as rtl8365mb | **SDK-only lineage, disposition open.** No public source; stock never writes it. |
| VLAN-aware port mode: bridge PVID, else PVID 0 + TAG_ONLY + empty VID 0 (`0x02000000`); PVID 1 when unaware | rtl8365mb (942-06); SDK-based driver's PVID-per-mode rule | HW: PVID 0 on a VLAN-unaware port gave a dead LAN (SDK-based driver), so never written there. The rest never benched. |
| Isolation `0x50c0 + port * 4` | RTLPlayground; ZTE | Stride HW-proven for ports 0-8 (P0); port 9 (`0x50e4`): **SDK-only lineage**. Self bit as in the SDK-based driver. Per-bridge masks never run. |
| Learning limit `0x5384 + port * 4` (bits 12:0, 4160 = on) | ZTE (with `_MAX` 4160); RTLPlayground address; SDK | Public. 0 at setup and on leave; behaviour at 0 unmeasured. |
| Flood masks `0x5360`-`0x5370` | ZTE `*FLOOD`; SDK-based driver's flag mapping | Public. CPU-only at setup; bridge flags drive the port bits. |
| CIST state `0x5310` (2 bits per port) | RTLPlayground; MNeroba#2; SDK | HW: encoding (MNeroba#2); stock uses the same. That states 0-2 block data is unmeasured. |
| Per-port flush `0x53d4`, mode `0x53dc` | RTLPlayground `port_l2_forget_port()`; ZTE; MNeroba#2; SDK | HW: completion on ports 4-7. Mode 0 sparing static unicast entries: documented, unmeasured. |
| Ageing `0x5354` 15:0 = s x 5, clamped 14-800 s | ZTE (cites the SDK DAL); SDK | **SDK-only lineage, disposition open.** The clamp has no SDK basis. |
| Lookup-miss `0x5374`, `0x4f74`, `0x4f78`/`0x4f7c`; new-SA `0x4f6c`; unmatched-SA `0x4f70` | SDK action setters | **SDK-only lineage, disposition open.** Written 0 (forward). |
| IGMP/MLD snooping `0x5290` bit 0 | SDK `dal_rtl8373_igmp_state_set` | **SDK-only lineage, disposition open.** Written 0. |
| Link-local trap: RMA `0x4ecc + 4 * slot` (slots 0, 2-7), LLDP `0x4f18`, RMA_CFG `0x4f1c` bit 3, trap CPU `0x4f34` 17:16 and `0x5350` 16:15 | RTLPlayground (addresses only); SDK; MNeroba's analysis (airjinkela#2) | **SDK-only lineage, disposition open**, every field. Which select steers RMA traps, and the REASON of trapped frames, unmeasured. |
| VLAN table reset: sentinel VID 4094, `0x4e14` bit 3 TABLE_RST, sample/sweep | SDK `dal_rtl8373_vlan_reset()` | **SDK-only lineage, disposition open.** Stock never uses TABLE_RST but sweeps every VID (fallback corroborated). |
| VLAN controls: `0x4e14` bits 0/1 VID types, `0x4e30`/`0x4e34` learn-disable, `0x4e38` port FID, egress keep `0x6728` | SDK field list, `vlan_init` | **SDK-only lineage, disposition open.** Stock writes `0x4e30`/`0x4e34` = 0 too. |
| L2 actions: `0x5350` bit 8 lookup-hit isolation, 7:6 hash-full; `0x4f80` learn-limit; `0x4f84` source-port permit | SDK setters; SDK-based driver cleared `0x4f84` | **SDK-only lineage, disposition open.** |
| Left at reset: `0x535c` SA-moving (logged), `0x5088`/`0x508c`, RMA trap priority and storm bypass, PTP `0x4f20`/`0x4f24`, RMA 10-2f `0x4eec`-`0x4f0c`, CDP `0x4f10`, SSTP `0x4f14` | SDK only | Not written; listed so bench anomalies can be traced to them. |

## What stock GL 4.10.1 corroborates

Static analysis of stock `rtl8372n-sw.ko` (identical in 4.10.0b2, whose
runtime capture applies); its U-Boot writes no VLAN, table, isolation,
CPU-tag, L2 or STP register.

- **Agrees:** the VLAN entry layout and write command; PVID layout; ingress
  filter on VLAN-aware ports and `0x4e14` bit 2 on; ORIGINAL egress on
  VLAN-aware user ports; learn-disable slots 0; CIST encoding; `0xa610`
  values; `0x6724` = 3; a full-table sweep as table reset.
- **Never written by stock:** egress transparency and keep, REAL_KEEP,
  TAG_ONLY, VID 0/4095 types, TABLE_RST, VID 0 entries, isolation as the
  forwarding domain, `0x5350` bit 8, the Realtek CPU tag (stock makes the CPU
  a tagged member of every VLAN instead).
- **Design differences, no contradiction:** CPU egress REAL_KEEP vs ORIGINAL,
  TAG_ONLY + PVID 0 vs GL's VLAN-4095 pseudo-native VLAN, the SerDes RX swap
  bit. Stock's model is the proven fallback if REAL_KEEP or transparency fail.

## Licences

- GPL-2.0-or-later, as Air's headers: `rtl8372n.c`, `rtl837x_mdio.c`,
  `rtl837x_common.c`, `rtl837x.h`, `rtl837x_regmap.h`, `src/Makefile`.
  GPL-2.0-only, as the MNeroba code they adapt: `rtl837x_table.c`,
  `rtl8372n_l2.c`, `rtl8372n_vlan.c`. `MODULE_LICENSE("GPL")` is metadata;
  `LICENSE` is the GPL-2.0 text. 0432 has no Signed-off-by yet.
- Excluded: `rtk-api`, the restricted generated register header, all
  PHY/SerDes patch arrays, GPIO, swconfig and the SDK debug interfaces.
- Licences do not settle rights in SDK-derived material. Open: the retained
  Air and ZTE lineage ([Airjinkela's SDK disclosure](https://github.com/RuijieNetworksCommunity/rtl837x-gsw-driver/issues/2#issuecomment-5946313878),
  [JiaY-shi's source question](https://github.com/JiaY-shi/rtl83xx/issues/1))
  and each SDK-only row. No Signed-off-by is inferred from attribution.

## RTLPlayground notice

```text
RTLPlayground register/operation reference
Source revision: f0aea3dcac056e3274fd39e1c76a7117471c37da
https://github.com/logicog/RTLPlayground

MIT License

Copyright (c) 2025 The RTLPlayground Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
