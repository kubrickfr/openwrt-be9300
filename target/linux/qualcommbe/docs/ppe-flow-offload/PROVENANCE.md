# PPE flow offload for the GL-BE9300: provenance

Who wrote what, the public inputs, and the source and confidence of every
register fact the offload patches rely on. Matching values in a public
source do not prove that the silicon behaves as described; the confidence
column says what backs each row.

Confidence labels:
- **src**: read from a public Qualcomm source (pinned below), path:line.
- **meas-P**: measured on a GL-BE9300 by Kamil Bienkiewicz and recorded in
  his patches (perceival/openwrt-flint3 `2365932733`).
- **meas-U**: measured on this tree's gateway (TESTING.md section 0).
- **hyp**: a hypothesis this tree depends on; TESTING.md names its gate.

Nothing in this series has run on hardware yet; every meas-U cell is empty.

## Who wrote what

- **Kamil Bienkiewicz** (perceival), [perceival/openwrt-flint3](https://github.com/perceival/openwrt-flint3)
  `2365932733`, patches `0411`-`0456` of `target/linux/qualcommbe/patches-6.18`,
  GPL-2.0-only:
  - `0433` here: the table primitives (flow, host, nexthop, L3 interface,
    MY_MAC, public IP tables, their encoders and refcounted pools, the APPE
    register map), from his 0411-0418, 0421, 0431, 0433, 0434, 0436-0438,
    0440 and 0441, the flow table base of 0454, the @deacclr_en kernel-doc
    of 0442 and the table parts of 0450, 0451 and 0455.
  - `0434` here: the IPv4 NAT offload engine (TC flowtable hook, indirect
    blocks, match/action parsing, DSA user-port egress with the emulated
    rtl8_4 head tag, VLAN push to a tagged WAN, per-flow counters, TCP and
    fragment exceptions, re-arm, FDB-move follower, table-full handling,
    the per-port L3 interface programming in `edma_port.c`), from his 0419,
    0420, 0422, 0424, 0427, 0428, 0435, 0436, 0439, 0440, 0442, 0444, 0446,
    0448-0450, 0452, 0453, 0455 and 0456, the IPv4 parts of 0451, and the
    engine parts of 0434 (PMTU from the redirect target) and 0441 (the NAT
    encoder fields); his 0423 (NETIF_F_HW_TC) and the .ndo_setup_tc hook
    of 0420 are set by `0435`.
  - The egress half of his parser alias (0445/0449: egress S-TPID 0x8899,
    conduit as S-VLAN egress port) is the basis of `0436`; the whole alias
    is the bench-only profile A of `0439`.
  - His patches carry his Signed-off-by, except 0440 and 0441 (per-flow
    counters, the NAT flow encoder's parameters), which have none in his
    tree. The ports here are modified (IPv6, tag_8021q and the ingress
    alias left out, IPv4 gated on the fragment exception, INET added to
    Kconfig, re-indented onto this tree's EDMA base) and carry no
    Signed-off-by, his or anyone's; `0433`/`0434` keep him as author and
    list his source patches and what changed in the port.
- **This branch** (François Guerraz, with Claude): `pending-6.18/714`
  (PPPoE match), `0435` (off by default, runtime switch, snapshot/restore
  with a latch, fail-closed disable, key allowlist, the header exceptions,
  hardware-state staleness and its breaker, withdrawal of declined
  re-offers, cleanup on flowtable unbind, the port MTU command, debugfs
  state), `0436` (profile E), `0437` (VLAN-aware bridge egress), `0438` (PPPoE
  sessions, their watch and breaker), `0439` (PPPoE upload, profile A),
  `0441` (bridge MAC egress), these documents.
- **Not carried**: perceival's EDMA fixes 0425/0426 (Tx fragment unmap),
  which are independent of offload; tracked separately.

## Pinned public inputs

| Input | Revision, licence | Use |
| --- | --- | --- |
| [qca-ssdk](https://git.codelinaro.org/clo/qsdk/oss/lklm/qca-ssdk) `NHSS.QSDK.12.5` | `a7745a8d10949778739fd61f11f77f0fbcd07148` (2025-02-10), ISC | Register addresses, fields, encodings, programming order |
| [nss-ppe](https://git.codelinaro.org/clo/qsdk/oss/lklm/nss-ppe) `NHSS.QSDK.12.5.5.r1` | `78a78dc53a887c6647406efb960c30755f66ba1b` (2024-09-02), ISC | How the vendor stack programs PPPoE sessions, L3 interfaces, exceptions |
| perceival/openwrt-flint3 `2365932733` | GPL-2.0-only | Kamil Bienkiewicz's offload series and its measurements |
| Linux 6.18.55 `net/netfilter`, `drivers/net/ppp/pppoe.c`, `net/sched/cls_flower.c` | GPL-2.0 | PPPoE match semantics (cls_flower convention), forward path |

IPQ5332 is built by the SDK as MPPE: `-DHPPE -DAPPE -DMPPE`, runtime type
APPE revision 1 (`make/config.mk:74-76`, `src/init/ssdk_init.c:2190-2194`,
`src/adpt/adpt.c:84-87`). The APPE register layouts apply; MPPE changes
table sizes only. Base addresses (`include/hsl/hppe/hppe_reg_access.h`):
IPE_L3 0x200000 (:29), TUNNEL_LOOKUP 0x300000 (:41), IPR 0x1e0000 (:39),
tunnel parser 0x1d0000 (:42), PTX 0x020000 (:50), ingress policer 0x100000
(:32). All lie inside the driver's existing regmap ranges (`ppe.c`).

## Register ledger

SSDK paths are relative to `include/hsl/` unless they start with `src/`.

| Register (driver macro) | Address | Fields used | Source | Conf. | Patch |
| --- | --- | --- | --- | --- | --- |
| Flow table op/data/result (`PPE_IN_FLOW_TBL_OP*`) | 0x200528 / 0x200550 / 0x2005c4 | cmd id, op type, hash-block bitmap, mode, entry index; result valid count | `hppe/hppe_flow_reg.h:268,429` | src, meas-P | 0433 |
| Flow table (`PPE_IN_FLOW_TBL_ADDR`), 2048 slots of 0x20 | 0x240000 | word 0 VALID read back after a delete and on a refresh; word 1 DE_ACCE [28] on a refresh (same bit in the NAT and bridge layouts) | `hppe/hppe_flow_reg.h:1764-1771`; DE_ACCE `hppe/hppe_flow_reg.h:936-1065` (`in_flow_tbl_0`/`_1`, `de_acce:1` after 28 bits of word 1) | src, meas-P (VALID, his IPv6 self-test of 0454, not carried); DE_ACCE set by hardware: hyp HD | 0433, 0435 |
| IPv4 5-tuple flow entry, NAT overlays | via op data | VALID, HOST_ADDR_INDEX, PROTOCOL, SRC_L3_IF_VALID [20], SRC_L3_IF [28:21], FWD_TYPE (1 SNAT, 2 DNAT), NEXT_HOP, L4 port, IP, ports, PMTU (L3 type) | `hppe/hppe_flow_reg.h:936-1097`, `src/adpt/hppe/adpt_hppe_flow.c:277-299` | src, meas-P | 0433, 0434 |
| Flow counters (`PPE_IN_FLOW_CNT_TBL`) | 0x120000 | packets 32 bit, bytes 40 bit | `hppe/hppe_flow_reg.h:2173,2196-2197` | src, meas-P | 0433 |
| Host table op (`PPE_HOST_TBL_OP*`), IPv4 host entry | 0x200724 / 0x200738 / 0x200770 | VALID, KEY_TYPE 0, LAN_WAN | `hppe/hppe_ip_reg.h:625,1774-1830` | src, meas-P (DNAT host marked WAN) | 0433, 0434 |
| Nexthop table (`PPE_IN_NEXTHOP_TBL`), 768 × 0x20 | 0x260000 | TYPE port, PORT, POST_L3_IF, STAG_FMT/SVID, CTAG_FMT/CVID, PUB_ADDR_INDEX, MAC, DNAT IP | `hppe/hppe_ip_reg.h:1898-2018` | src, meas-P | 0433 |
| Public IP table (`PPE_PUB_IP_ADDR_TBL`), 16 | 0x2004d8 | IPv4 address | `hppe/hppe_ip_reg.h:1032-1042` | src, meas-P | 0433 |
| MY_MAC table, 8 × 2 words | 0x200000 | MAC, VALID [48] | `hppe/hppe_ip_reg.h:28-56` | src, meas-P | 0433, 0434 |
| L3_VP_PORT table, 4 words | 0x204000 + 0x10·port | L3_IF_VALID [0], L3_IF_INDEX [8:1]; latches on the last word | `hppe/hppe_ip_reg.h:1222-1456` | src, meas-P | 0433 |
| Ingress L3 interface (`PPE_IN_L3_IF_TBL`), 256 × 3 words | 0x206000 | MRU, MTU, IPv4/IPv6 route enable, TTL exceed cmd (redirect) and de-accel (off here; the vendor sets it, `nss-ppe drv/ppe_drv/ppe_drv_l3_if.c:574`), MAC_BITMAP, **PPPOE_EN [43]** | `hppe/hppe_ip_reg.h:1458-1578` (PPPOE_EN :1515-1519) | src, meas-P (except PPPOE_EN: hyp HP0; 0441's unbound, unrouted entries: hyp HB) | 0433, 0435, 0438, 0441 |
| Egress L3 interface (`PPE_EG_L3_IF_TBL`), 256 × 3 words | 0x023000 | MAC, **SESSION_ID [63:48]**, **PPPOE_EN [64]** | `hppe/hppe_ip_reg.h:2020-2074` | src, meas-P (MAC, on port-bound routed interfaces only; 0441's unbound ones: hyp HB; PPPoE: hyp HP2) | 0433, 0438, 0439, 0441 |
| PPPoE session (`PPE_PPPOE_SESSION`), 64 × stride 8 | 0x306800 | SESSION_ID [15:0], PORT_BITMAP [23:16], TYPE [25:24] = 0 (port bitmap) | `appe/appe_pppoe_reg.h:30-89`; TYPE value `src/adpt/appe/adpt_appe_pppoe.c:169-171`, `include/adpt/adpt.h:2253-2288` | src; hyp HP0/HP1 | 0438 |
| PPPoE session EXT | 0x306a00 | MC_VALID [0], UC_VALID [1], SMAC_VALID [2], SMAC[15:0] in [31:16]; free iff MC = UC = 0; written last, cleared first | `appe/appe_pppoe_reg.h:91-130`; `src/adpt/appe/adpt_appe_pppoe.c:125-129,165-167` | src | 0438 |
| PPPoE session EXT1 | 0x306c00 | SMAC[47:16], mac[0] in [31:24] | `appe/appe_pppoe_reg.h:132-152`; `adpt_appe_pppoe.c:161-163` | src | 0438 |
| PPPoE session EXT2 | 0x306e00 | L3_IF_INDEX [15:8], L3_IF_VALID [16] ("this l3 interface index will remove pppoe header", `fal/fal_pppoe.h:40`) | `appe/appe_pppoe_reg.h:154-193` | src; hyp HP0 | 0438 |
| L3 exception action (`PPE_L3_EXCEPTION_CMD`), 72 codes | 0x20081c | CMD [1:0] (3 = redirect), DE_ACCE [2] | `hppe/hppe_sec_reg.h:28-48` | src, meas-P | 0434 |
| L3 exception enables (generic / flow hit / flow miss) | 0x200cdc / 0x201678 / 0x2017a8 | EXCEP_EN [0]: generic 0, hit 1, miss 0 (APPE) | `hppe/hppe_sec_reg.h:145`; `appe/appe_sec_reg.h:78,101`; `src/adpt/hppe/adpt_hppe_sec.c:169-214` | src, meas-P | 0434 |
| TCP flag exception parser (`PPE_L4_EXCEPTION_PARSING_CTRL`) | 0x1e0028 | value/mask per slot, FIN/SYN/RST | `hppe/hppe_sec_reg.h:227` | src, meas-P | 0434 |
| Exception codes | — | TCP flags 46-48 (FIN, RST de-accelerate; SYN redirect only); unknown L2 0, IPv4 small IHL 5, options 6, header incomplete 7, bad total length 8, data incomplete 9, fragment 10, small TTL 12, checksum 14, TCP header incomplete 42, TCP small data offset 45, UDP / UDP-Lite header incomplete 55 / 61 (redirect only); PPPoE 1-3 (de-accelerate) | `include/fal/fal_sec.h:36-100`; the vendor's list and de-accel choices `nss-ppe drv/ppe_drv/ppe_drv_exception.c:26-212` | src | 0434, 0435, 0438 |
| L3 route control (`PPE_L3_ROUTE_CTRL`, `_EXT`), read only | 0x200480 / 0x200494 | MRU/MTU/MTU-DF fail, flow source-interface check, PPPoE multicast, flow PMTU fail: each must read 3 (redirect) | `hppe/hppe_ip_reg.h:387-620`; reset 0x0dbc36db / 0x00031b23 read back by Kamil (his 0436, patch line 64) | src, meas-P | 0435, 0438 |
| Egress TPIDs (`PPE_EG_VLAN_TPID`) | 0x20040 | STPID [15:0] = 0x8899 (profile E), CTPID [31:16] stays 0x8100 | `hppe/hppe_portvlan_reg.h:1275-1307` | src, meas-P (with his full alias) ; hyp HE1 for E alone | 0436 |
| Port egress VLAN (`PPE_PORT_EG_VLAN_TBL`) | 0x20020 + 4·port | VLAN_TYPE [0] (S-VLAN port), PCP_PROP [6] and DEI_PROP [7] cleared, on the conduit; on the CPU port (profile A) CTAG/STAG mode = 2 (unmodified), VSI mode off, by masked update (TX_COUNTING_EN [8] kept) | `hppe/hppe_portvlan_reg.h:1208-1273` (bits :1259-1268); mode encoding `src/adpt/hppe/adpt_hppe_portvlan.c:453-470` | src, meas-P; PCP/DEI not reaching the inserts: hyp HE2b | 0436, 0439 |
| Port MRU/MTU control (`PPE_MRU_MTU_CTRL_TBL`), 3 words | 0x65000 + 0x10·port | MTU_CMD [31:30] := 3 (redirect) while offload is on; MRU, MTU, MRU_CMD left as the port has them | `hppe/hppe_portctrl_reg.h:565-700` (APPE `MRU_MTU_CTRL_TBL_ADDRESS` 0x5000 within its block, 0x65000 in this driver's `ppe_regs.h:225`; `mru_mtu_ctrl_tbl`); action encoding this tree's `ppe_config.h:246-257` (`enum ppe_action_type`) | src; what the PPE counts against it: hyp HM | 0435 |
| Ingress/tunnel parser TPIDs (`PPE_IPR_VLAN_TPID`, `PPE_TPR_VLAN_TPID`) | 0x1e0020 / 0x1d0020 | CTAG [15:0], STAG [31:16]; profile A writes 0x88990000 | `hppe/hppe_portvlan_reg.h:91-117`; `appe/appe_portvlan_reg.h:396-422` | src, meas-P (H0 false: tagged WAN not parsed) | 0439 |
| Port parsing (`PPE_PORT_PARSING`) | 0x1e0000 + 4·port | PORT_ROLE [0] (core) on the conduit, profile A | `hppe/hppe_portvlan_reg.h:28-61` | src, meas-P | 0439 |
| EDMA service code 1 egress bypass | (existing driver) | VLAN translate, tag format control, L2/L3 edit bypassed for CPU transmits | this tree's `ppe_config.c:2059` `ppe_servcode_init()`, `ppe_config.h:140-155` | src (driver) ; hyp HE1 | 0436 rationale |

## Behaviour the code relies on that no source states

| Id | Statement | Why believed | Gate |
| --- | --- | --- | --- |
| HE1 | Profile E (and the port MTU command) alone change nothing for software-path frames | CPU transmits bypass egress editing; the PPE bridges nothing between ports | TESTING R1, L1 |
| HE2 | The RTL8372N accepts the emulated head tag (W2 = 0x8100) on this tree's driver | meas-P on the SDK-based driver | R3 (rig) |
| HV1 | An injected untagged frame is classified to VLAN 1 and leaves per VLAN 1's entry | this tree's rtl8372n driver: CPU port VLAN-unaware PVID 1, ORIGINAL egress | R3 (rig) |
| HE2b | PCP and DEI of the WAN's VLAN tag do not reach the emulated tag's words | PCP/DEI propagation off at reset and cleared by E; the S-tag PCP comes from the port default (0) | R3 (rig) |
| HB | An L3 interface bound to no port (no L3_VP_PORT, MAC_BITMAP 0, routing off), used only as a nexthop's POST_L3_IF, gives the frame its EG_L3_IF source MAC and its IN_L3_IF MTU is the one checked | routing enable and MAC_BITMAP are ingress checks; the vendor never builds this shape (`nss-ppe drv/ppe_drv/ppe_drv_l3_if.c:572-573` always sets routing) | R3 (rig) |
| HS1 | A valid session entry leaves the session's frames that miss the flow table intact for the CPU (PPPoE header kept) | not stated anywhere; the vendor keeps its session entry valid for the whole link, from PPP connect to disconnect (`nss-ppe clients/pppoe/nss_ppe_pppoe_mgr.c:203-270,350-353`, `drv/ppe_drv/ppe_drv_pppoe_session.c:103`) | R2 (rig), before any live session |
| HP0 | A frame of the programmed session is decapsulated, routed and hits the flow keyed on the session's L3 interface | vendor recipe (nss-ppe) | R3 (rig) |
| HP1 | Plain IPv4, another session or another source MAC with the same 5-tuple does not hit | session match fields; SRC_L3_IF on the flow | R4 (rig) |
| HD | Hardware sets a flow entry's DE_ACCE when a de-accelerating exception fires on it | `nss-ppe drv/ppe_drv/tun/ppe_drv_tun_decap.c:345` ("deacce_en bit got set ... by HW") for the vendor's tunnel tables; the field in the flow entry | R2 (a wrong-code PPPoE frame on an idle flow: `rearmed_deacce` rises by 1, `rearmed_invalid` flat) |
| HM | Full-size frames pass the port and L3 MTU checks at zero margin; the emulated tag's 8 bytes are counted as the vendor says ("PPE ignore vlan headers at L2") | `nss-ppe drv/ppe_drv/ppe_drv_port.c:1635` | R3 (rig), CPU codes 79/80/82/83/91/93/94 |
| HP2 | Upload encap from EG_L3_IF builds a correct PPPoE header | vendor recipe | R6 (rig, profile A) |
| H0 | WAN 802.1Q still parsed with C-TPID 0x0000 | expected false (meas-P) | R6 (rig, profile A) |
