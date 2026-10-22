/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RTL8372N register addresses compared with these public sources:
 * - RTLPlayground, MIT, commit f0aea3dcac056e3274fd39e1c76a7117471c37da
 * - linux-mainline-zte-zxslc-sr1010, GPL-2.0, commit
 *   07f8687248578d4be6931c665ff5d08bb6cc3d9d
 *
 * SDS indirect-command fields and polarity operations are compared with the
 * public ZTE driver and RTLPlayground's MIT register-operation traces.
 * RTLPlayground attributes the companion TX bits to OEM firmware, so that is
 * behavioral corroboration, not independent provenance. Airjinkela's driver
 * also has disclosed SDK-derived lineage. Keep the map limited to definitions
 * used here; these comparisons do not establish independent origin.
 * Do not add unverified SerDes pages or vendor PHY patch data.
 */
#ifndef __RTL837X_REGMAP_H__
#define __RTL837X_REGMAP_H__

#include <linux/bits.h>

#define RTL837X_REG_CHIP_ID                         0x0004
#define RTL837X_REG_RESET                           0x0024
#define RTL837X_RESET_SOC                           BIT(0)

#define RTL837X_SMI_PHY_PORT_SELECT                 0x6438
#define RTL837X_SMI_PHY_CTRL                        0x643c
#define RTL837X_SMI_PHY_READ_DATA                   0x6440
#define RTL837X_SMI_PHY_WRITE_DATA                  0x6444
#define RTL837X_SMI_PHY_BUSY                        BIT(0)
#define RTL837X_SMI_PHY_STATUS                      GENMASK(26, 24)
#define RTL837X_SMI_PHY_READ_CMD                    0x3
#define RTL837X_SMI_PHY_WRITE_CMD                   0x7

#define RTL837X_SMI_MAC_TYPE                        0x6330
#define RTL837X_SMI_MAC_PORT3_TYPE                  GENMASK(7, 6)
#define RTL837X_SMI_MAC_PORT8_TYPE                  GENMASK(17, 16)
#define RTL837X_SMI_PORT_POLLING                    0x6334
#define RTL837X_SMI_PORT_POLLING_4_7                GENMASK(7, 4)
#define RTL837X_SMI_GLOBAL_CTRL                     0x632c
#define RTL837X_SMI_POLLING_MASK                    GENMASK(20, 12)
#define RTL837X_SMI_CTRL                            0x6454
#define RTL837X_SMI_MDC_ENABLE                      GENMASK(14, 12)

/*
 * Ports whose received frames are parsed for an RTL8_4 tag (EtherType
 * 0x8899), bits 9:0 in the RTL8373 SDK; bit 9 is the internal 8051.
 */
#define RTL837X_CPU_TAG_AWARE                       0x603c
#define RTL837X_CPU_TAG_AWARE_PORTS                 GENMASK(8, 0)
#define RTL837X_CPU_TAG_CTRL                        0x6720
#define RTL837X_CPU_TAG_ENABLE                      BIT(1)
#define RTL837X_CPU_TAG_INSERT_MODE                 GENMASK(11, 10)
#define RTL837X_EXTERNAL_CPU_PORT                   0x6724
#define RTL837X_EXTERNAL_CPU_PORT_MASK              GENMASK(3, 0)

#define RTL837X_SDS_MODE_SELECT                     0x7b20
#define RTL837X_SDS0_MODE_MASK                      GENMASK(4, 0)
#define RTL837X_SDS1_MODE_MASK                      GENMASK(9, 5)
#define RTL837X_SDS_MODE_10GBASE_R                  0x1a
#define RTL837X_SDS_INDACS_COMMAND                  0x03f8
#define RTL837X_SDS_INDACS_READ_DATA                0x03fc
#define RTL837X_SDS_INDACS_WRITE_DATA               0x0400
#define RTL837X_SDS_INDACS_EXECUTE                  BIT(15)
#define RTL837X_SDS_INDACS_WRITE                    BIT(14)
#define RTL837X_SDS_INDACS_REG                      GENMASK(11, 7)
#define RTL837X_SDS_INDACS_PAGE                     GENMASK(6, 1)
#define RTL837X_SDS_INDACS_INDEX                    BIT(0)
#define RTL837X_SDS_PAGE_XSG                        0x00
#define RTL837X_SDS_PAGE_10GBASE_R                  0x06
#define RTL837X_SDS_XSG_REG_POLARITY                0x00
#define RTL837X_SDS_10G_REG_POLARITY                0x02
#define RTL837X_SDS_XSG_TX_POLARITY_SWAP            BIT(8)
#define RTL837X_SDS_XSG_RX_POLARITY_SWAP            BIT(9)
#define RTL837X_SDS_10G_TX_POLARITY_SWAP            BIT(14)
#define RTL837X_SDS_10G_RX_POLARITY_SWAP            BIT(13)

#define RTL837X_MAC_LINK_STATUS                     0x63e8
#define RTL837X_MAC_SPEED_STATUS_0                  0x63f0
#define RTL837X_MAC_SPEED_STATUS_1                  0x63f4
#define RTL837X_MAC_DUPLEX_STATUS                   0x63f8
#define RTL837X_MAC_TX_PAUSE_STATUS                 0x63fc
#define RTL837X_MAC_RX_PAUSE_STATUS                 0x6400
#define RTL837X_MAC_SPEED_STATUS_REG(port) \
	((port) < 8 ? RTL837X_MAC_SPEED_STATUS_0 : RTL837X_MAC_SPEED_STATUS_1)
#define RTL837X_MAC_SPEED_STATUS_MASK(port) \
	(GENMASK(3, 0) << (((port) % 8) * 4))

#define RTL837X_PORT_ISOLATION_BASE                 0x50c0
#define RTL837X_PORT_ISOLATION_REG(port) \
	(RTL837X_PORT_ISOLATION_BASE + ((port) * 4))

/* Public ZTE DSA map: per-port learning limit and global flood masks. */
#define RTL837X_L2_LEARN_LIMIT_REG(port) (0x5384 + (port) * 4)
#define RTL837X_L2_LEARN_LIMIT_MASK GENMASK(12, 0)
/* Learning on: the LUT size, as the SDK-based driver and ZTE write it. */
#define RTL837X_L2_LEARN_LIMIT_MAX 4160
/*
 * RTL8373 SDK: one global action, despite the per-port API, for a frame
 * with a new source address on a port at its learning limit: 0 forwards
 * without learning, 1 drops, 2 traps.
 */
#define RTL837X_L2_LEARN_LIMIT_ACT 0x4f80
#define RTL837X_L2_LEARN_LIMIT_ACT_MASK GENMASK(1, 0)
#define RTL837X_L2_UNKNOWN_UC_FLOOD 0x5360
#define RTL837X_L2_UNKNOWN_MC_FLOOD 0x5364
#define RTL837X_IPV4_UNKNOWN_MC_FLOOD 0x5368
#define RTL837X_IPV6_UNKNOWN_MC_FLOOD 0x536c
#define RTL837X_L2_BROADCAST_FLOOD 0x5370
#define RTL837X_L2_FLOOD_MASK GENMASK(9, 0)

/*
 * RTL8373 SDK: per-ingress-port actions, two bits per port 0-9. For the
 * lookup-miss classes (unknown unicast, L2, IPv4 and IPv6 multicast) 0
 * forwards to that class's flood mask; for a source address that is new (not
 * in the table) or known on another port 0 forwards the frame (the SDK calls
 * it "flooding"). 1 drops and 2 traps in every register; 3 differs per
 * register. Broadcast has no action register.
 */
#define RTL837X_L2_NEW_SA_ACT                       0x4f6c
#define RTL837X_L2_UNMATCHED_SA_ACT                 0x4f70
#define RTL837X_L2_UNKNOWN_MC_ACT                   0x4f74
#define RTL837X_IPV4_UNKNOWN_MC_ACT                 0x4f78
#define RTL837X_IPV6_UNKNOWN_MC_ACT                 0x4f7c
#define RTL837X_L2_UNKNOWN_UC_ACT                   0x5374
#define RTL837X_L2_PORT_ACT_MASK(port)              (GENMASK(1, 0) << ((port) * 2))
#define RTL837X_L2_ACT_FORWARD                      0

/* CIST (MSTI 0) port states, two bits per port 0-9. */
#define RTL837X_STP_STATE                           0x5310
#define RTL837X_STP_STATE_MASK(port)                (GENMASK(1, 0) << ((port) * 2))

/* Global L2 control; other fields belong to ageing and lookup code. */
#define RTL837X_L2_CTRL                             0x5350
#define RTL837X_L2_CTRL_TRAP_CPU                    GENMASK(16, 15)
/*
 * RTL8373 SDK: a destination found in the table on a port outside the
 * ingress port's isolation mask is handled as unknown (1) or as "NOP" (0,
 * undocumented: dropped, or forwarded across isolation).
 */
#define RTL837X_L2_CTRL_LOOKUP_HIT_ISO_UNKNOWN      BIT(8)
/*
 * RTL8373 SDK dal_rtl8373_l2_hashFull_set(): a new source address whose
 * bucket and the overflow CAM are full: 0 forwards (without learning), 1
 * drops, 2 traps.
 */
#define RTL837X_L2_CTRL_HASH_FULL_ACT               GENMASK(7, 6)

/* RTL8373 SDK: bit p set lets a frame received on port p leave by it. */
#define RTL837X_L2_SRC_PORT_PERMIT                  0x4f84

/*
 * RTL8373 SDK: per-port bit, whether a source address known on another port
 * may move to port p. The SDK's own comment ("0: can't move, 1: can move")
 * contradicts the register name (FORBID), so it is only read back.
 */
#define RTL837X_L2_SA_MOVING_FORBID                 0x535c

/* Ageing time of learned entries; the SDK writes seconds * 5. */
#define RTL837X_L2_AGE_CTRL                         0x5354
#define RTL837X_L2_AGE_UNIT                         GENMASK(15, 0)
#define RTL837X_L2_AGE_UNITS_PER_SEC                5

#define RTL837X_IGMP_CTRL                           0x5290
#define RTL837X_IGMP_CTRL_SNOOPING                  BIT(0)

/*
 * Reserved multicast (RMA) actions for DA 01:80:c2:00:00:xx, one word per
 * slot. Slots 0-7 cover 00-0f: 00, 01 (PAUSE), 02, 03, then 04-07/09-0c/0f,
 * 08, 0d and 0e. The LLDP word replaces them for LLDP frames while
 * RMA_CFG.LLDP_EN is set.
 */
#define RTL837X_RMA_CTRL(slot)                      (0x4ecc + (slot) * 4)
#define RTL837X_RMA_SLOT_PAUSE                      1
#define RTL837X_RMA_LINK_LOCAL_SLOTS                8
#define RTL837X_RMA_CTRL_LLDP                       0x4f18
#define RTL837X_RMA_ACTION                          GENMASK(5, 4)
#define RTL837X_RMA_DIS_STORM                       BIT(3)
#define RTL837X_RMA_CKEEP                           BIT(2)
#define RTL837X_RMA_VLAN_LEAKY                      BIT(1)
#define RTL837X_RMA_PISO_LEAKY                      BIT(0)
#define RTL837X_RMA_ACTION_TRAP                     1
#define RTL837X_RMA_CFG                             0x4f1c
#define RTL837X_RMA_CFG_LLDP_EN                     BIT(3)
#define RTL837X_RMA_PTP_TRAP_CTRL                   0x4f34
#define RTL837X_RMA_PTP_TRAP_CPU                    GENMASK(17, 16)
/* Trap CPU select: 0 none, 1 internal 8051, 2 external CPU, 3 both. */
#define RTL837X_TRAP_CPU_EXTERNAL                   2

#define RTL837X_SMI_MDIO_CTRL_REG                   21
#define RTL837X_SMI_MDIO_BUSY                       BIT(2)
#define RTL837X_SMI_MDIO_ADDR_REG                   22
#define RTL837X_SMI_MDIO_DATA_LOW                   23
#define RTL837X_SMI_MDIO_DATA_HIGH                  24
#define RTL837X_SMI_MDIO_READ_CMD                   0x1b
#define RTL837X_SMI_MDIO_WRITE_CMD                  0x19

/*
 * Indirect table access engine shared by the VLAN and L2 tables. Field widths
 * follow the RTL8373 SDK (RTL8373_ITA_*); see PROVENANCE.md for each field.
 */
#define RTL837X_TABLE_CTRL                          0x5cac
#define RTL837X_TABLE_CTRL_ADDR                     GENMASK(28, 16)
#define RTL837X_TABLE_CTRL_TYPE                     GENMASK(10, 8)
#define RTL837X_TABLE_CTRL_WRITE                    BIT(1)
#define RTL837X_TABLE_CTRL_EXECUTE                  BIT(0)
/* L2 operation control and status; the control fields persist across ops. */
#define RTL837X_TABLE_L2_CTRL                       0x5cb0
#define RTL837X_TABLE_L2_PORT                       GENMASK(22, 19)
#define RTL837X_TABLE_L2_ENTRY_CLR                  BIT(18)
#define RTL837X_TABLE_L2_METHOD                     GENMASK(17, 14)
/* SDK TBL_TYPE, undocumented; may flag an overflow (CAM) entry. */
#define RTL837X_TABLE_L2_TYPE                       BIT(13)
#define RTL837X_TABLE_L2_HIT                        BIT(12)
#define RTL837X_TABLE_L2_INDEX                      GENMASK(11, 0)
#define RTL837X_TABLE_WRITE_DATA(i)                 (0x5cb8 + (i) * 4)
#define RTL837X_TABLE_READ_DATA(i)                  (0x5ccc + (i) * 4)
#define RTL837X_TABLE_MAX_WORDS                     5

/* VLAN table entry, data word 0, indexed by VID. */
#define RTL837X_VLAN_ENTRY_MEMBER                   GENMASK(9, 0)
#define RTL837X_VLAN_ENTRY_UNTAG                    GENMASK(19, 10)
#define RTL837X_VLAN_ENTRY_FID_MSTI                 GENMASK(23, 20)
#define RTL837X_VLAN_ENTRY_SVLAN_CHK                BIT(24)
/* SDK, ZTE and Air: IVL (1) / SVL (0). RTLPlayground's prose says "valid". */
#define RTL837X_VLAN_ENTRY_IVL                      BIT(25)
#define RTL837X_VLAN_ENTRY_FIELDS                   GENMASK(25, 0)
/*
 * The fields that make an entry admit, untag or govern anything: members,
 * untag set and FID/MSTI. Bits 25:24 alone only choose how frames of a VID
 * with no member would be keyed for learning.
 */
#define RTL837X_VLAN_ENTRY_USED                     (RTL837X_VLAN_ENTRY_MEMBER | \
						     RTL837X_VLAN_ENTRY_UNTAG | \
						     RTL837X_VLAN_ENTRY_FID_MSTI)

/* L2 unicast entry, data words 0-2 (multicast and L3 entries differ). */
#define RTL837X_L2_W0_MAC2                          GENMASK(31, 24)
#define RTL837X_L2_W0_MAC3                          GENMASK(23, 16)
#define RTL837X_L2_W0_MAC4                          GENMASK(15, 8)
#define RTL837X_L2_W0_MAC5                          GENMASK(7, 0)
#define RTL837X_L2_W1_SPA_LO                        GENMASK(31, 30)
#define RTL837X_L2_W1_IVL                           BIT(29)
#define RTL837X_L2_W1_L3                            BIT(28)
#define RTL837X_L2_W1_VID_FID                       GENMASK(27, 16)
#define RTL837X_L2_W1_MAC0                          GENMASK(15, 8)
#define RTL837X_L2_W1_MULTICAST                     BIT(8)
#define RTL837X_L2_W1_MAC1                          GENMASK(7, 0)
#define RTL837X_L2_W2_STATIC                        BIT(16)
#define RTL837X_L2_W2_AUTH                          BIT(5)
#define RTL837X_L2_W2_AGE                           GENMASK(4, 2)
#define RTL837X_L2_W2_SPA_HI                        GENMASK(1, 0)

/* Whole-LUT clear: write 1 to start; reads 1 while the clear runs. */
#define RTL837X_L2_FLUSH_ALL                        0x53d8
#define RTL837X_L2_FLUSH_ALL_EXEC                   BIT(0)

/*
 * Selective flush: START with a port mask runs it and clears itself, BUSY
 * reads 1 while it runs, and the port mask stays latched afterwards, so
 * only BUSY tells completion. The mode register selects by port (0), port
 * and VID (1) or port and FID (2); STATIC also removes static entries.
 */
#define RTL837X_L2_FLUSH_CMD                        0x53d4
#define RTL837X_L2_FLUSH_CMD_BUSY                   BIT(17)
#define RTL837X_L2_FLUSH_CMD_START                  BIT(16)
#define RTL837X_L2_FLUSH_CMD_PORTS                  GENMASK(9, 0)
#define RTL837X_L2_FLUSH_MODE                       0x53dc
#define RTL837X_L2_FLUSH_MODE_STATIC                BIT(2)
#define RTL837X_L2_FLUSH_MODE_SEL                   GENMASK(1, 0)

/*
 * VLAN port controls; see the PROVENANCE.md rows for each. The 0x4e10 and
 * 0x6738 per-port fields are in RTLPlayground and the ZTE driver; the mode
 * encodings follow the equivalent RTL8365MB controls in Linux rtl8365mb.
 * The egress transparency register has no pinned public source: its only
 * source is the RTL8373 SDK formerly shipped in this package.
 */
#define RTL837X_VLAN_CTRL                           0x4e14
/*
 * RTL8373 SDK, meaning inferred from its enum names: a frame tagged VID 0
 * (or 4095) is classified to the PVID like an untagged one (0) or keeps
 * that VID (1).
 */
#define RTL837X_VLAN_CTRL_VID0_TYPE                 BIT(0)
#define RTL837X_VLAN_CTRL_VID4095_TYPE              BIT(1)
#define RTL837X_VLAN_CTRL_FILTER                    BIT(2)
/* RTL8373 SDK: write 1 to reset the VLAN table; it may not clear itself. */
#define RTL837X_VLAN_CTRL_TABLE_RST                 BIT(3)
/* RTL8373 SDK: two per-VID learning-disable slots (VALID 0, VID 12:1). */
#define RTL837X_VLAN_L2_LRN_DIS(slot)               (0x4e30 + (slot) * 4)
#define RTL837X_VLAN_L2_LRN_DIS_SLOTS               2
#define RTL837X_VLAN_L2_LRN_DIS_FIELDS              GENMASK(13, 0)
/* RTL8373 SDK: port p's frames take a port-based FID whatever their VLAN. */
#define RTL837X_PORT_BASED_FID_EN                   0x4e38
/*
 * Accepted frame types, two bits per port; 0 accepts all frames, 1 tagged
 * frames only (RTL8373 SDK; whether that drops priority tags is unknown).
 */
#define RTL837X_VLAN_INGRESS_CTRL                   0x4e10
#define RTL837X_VLAN_ACCEPT_FRAME_TYPE_MASK(port)   (GENMASK(1, 0) << ((port) * 2))
#define RTL837X_VLAN_ACCEPT_FRAME_TYPE_ALL          0
#define RTL837X_VLAN_ACCEPT_FRAME_TYPE_TAG_ONLY     1
/* Per-port ingress VLAN membership filter. */
#define RTL837X_VLAN_INGRESS_FILTER                 0x4e18
/* Per egress port: ingress ports whose frames skip VLAN membership checks. */
#define RTL837X_VLAN_EGRESS_TRANS_REG(port)         (0x4eb8 + ((port) / 3) * 4)
#define RTL837X_VLAN_EGRESS_TRANS_MASK(port)        (GENMASK(9, 0) << (((port) % 3) * 10))
#define RTL837X_VLAN_EGRESS_TRANS_BIT(egress, ingress) \
	BIT((ingress) + ((egress) % 3) * 10)
/* RTL8373 SDK, same layout: ingress ports whose tag format egress keeps. */
#define RTL837X_VLAN_EGRESS_KEEP_REG(port)          (0x6728 + ((port) / 3) * 4)
#define RTL837X_VLAN_EGRESS_KEEP_MASK(port)         (GENMASK(9, 0) << (((port) % 3) * 10))
/* Egress tag mode, two bits per port. */
#define RTL837X_VLAN_EGRESS_TAG                     0x6738
#define RTL837X_VLAN_EGRESS_TAG_MODE_MASK(port)     (GENMASK(1, 0) << ((port) * 2))
#define RTL837X_VLAN_EGRESS_MODE_ORIGINAL           0
#define RTL837X_VLAN_EGRESS_MODE_REAL_KEEP          3
#define RTL837X_PORT_PVID_BASE                      0x4e1c
#define RTL837X_PORT_PVID_REG(port) \
	(RTL837X_PORT_PVID_BASE + (((port) >> 1) * 4))
#define RTL837X_PORT_PVID_MASK(port) \
	(GENMASK(11, 0) << (((port) & 1) * 12))


#endif /* __RTL837X_REGMAP_H__ */
