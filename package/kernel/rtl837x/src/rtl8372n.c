// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 StarField Xu <air_jinkela@163.com>
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/ethtool.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/rcupdate.h>
#include <linux/regmap.h>
#include <linux/of_net.h>

#include "rtl837x.h"

#define RTL8372N_MDIO_RESET_DELAY_US 10

#define IS_SERDES_PORT(port) (((port) == 3) || ((port) == 8))

#define PORT_TO_SERDES_IDX(port) ((port) == 3 ? 0 : 1)

struct rtl8372n_pcs {
	struct phylink_pcs pcs;
	struct rtl837x_priv *priv;
	int index;
};

struct rtl8372n {
	struct rtl8372n_pcs pcs[RTL8372N_NUM_PORTS];
};

static int rtl8372n_detect(struct rtl837x_priv *priv)
{
	u32 chip_id;
	int ret;

	ret = rtl837x_reg_read(priv, RTL837X_REG_CHIP_ID, &chip_id);
	if (ret)
		return ret;

	dev_info(priv->dev, "chip ID: 0x%08x\n", chip_id);
	if ((chip_id >> 8) != 0x837270)
		return -ENODEV;

	priv->num_ports = RTL8372N_NUM_PORTS;
	return 0;
}

static int rtl8372n_soft_reset_chip(struct rtl837x_priv *priv)
{
	u32 tmp;
	int ret;

	ret = priv->write_reset(priv);
	if (ret)
		return ret;

	msleep(250);
	return regmap_read_poll_timeout(priv->map, RTL837X_REG_RESET, tmp,
					!(tmp & RTL837X_RESET_SOC),
					20000, 1000000);

}

static enum dsa_tag_protocol rtl8372n_get_tag_protocol(struct dsa_switch *ds,
							int port,
							enum dsa_tag_protocol mp)
{
	return DSA_TAG_PROTO_RTL8_4;
}

static int rtl8372n_mdio_phy_read_c45(struct mii_bus *bus, int port, int devad, int regnum)
{
	struct rtl837x_priv *priv = bus->priv;
	u16 val;
	int ret = priv->ops->phy_read_c45(priv, port, devad, regnum, &val);

	if (ret) {
		dev_err_ratelimited(priv->dev,
				    "PHY C45 read failed: port=%d devad=%d reg=0x%04x err=%d\n",
				    port, devad, regnum, ret);
		return ret;
	}

	return val;
}

static int rtl8372n_mdio_phy_write_c45(struct mii_bus *bus, int port, int devad, int regnum, u16 val)
{
	struct rtl837x_priv *priv = bus->priv;

	return priv->ops->phy_write_c45(priv, port, devad, regnum, val);
}

static int rtl8372n_register_phys(struct rtl837x_priv *priv,
				  struct mii_bus *bus, struct device_node *mnp)
{
	struct device_node *child;
	struct phy_device *phy;
	u32 seen = 0;
	int port, addr, ret;

	/* This private bus exposes only enabled internal user PHYs. */
	for_each_available_child_of_node(mnp, child) {
		if (of_node_name_eq(child, "ethernet-phy-package")) {
			dev_err(priv->dev,
				"PHY packages are not supported on the private MDIO bus\n");
			of_node_put(child);
			return -EOPNOTSUPP;
		}
		addr = of_mdio_parse_addr(priv->dev, child);
		if (addr < 0 || !of_mdiobus_child_is_phy(child) ||
		    !(BIT(addr) & dsa_user_ports(priv->ds) & RTL8372N_PHY_PORT_MASK) ||
		    (seen & BIT(addr))) {
			of_node_put(child);
			return -EINVAL;
		}
		seen |= BIT(addr);
	}
	if (mnp && seen != (dsa_user_ports(priv->ds) & RTL8372N_PHY_PORT_MASK)) {
		dev_err(priv->dev,
			"MDIO node must describe every enabled internal PHY at ports 4-7\n");
		return -EINVAL;
	}

	for (port = 4; port <= 7; port++) {
		if (!(dsa_user_ports(priv->ds) & BIT(port)))
			continue;

		/*
		 * The switch reaches its internal PHYs through an MMD-addressed
		 * access engine, so register them as the Clause 45 devices they
		 * are and let phylib bind the matching driver by ID (the RTL8224
		 * entry of the Realtek PHY driver).
		 */
		phy = get_phy_device(bus, port, true);
		if (IS_ERR(phy)) {
			ret = PTR_ERR(phy);
			dev_err(priv->dev, "PHY discovery port %d failed: %d\n", port, ret);
			return ret;
		}
		/*
		 * This bus has no Clause 22 accessors. Drop the "C22 registers
		 * present" bit so that phylib's generic aneg restart and
		 * fallbacks stay on the C45 path.
		 */
		phy->c45_ids.devices_in_package &= ~BIT(0);

		for_each_available_child_of_node(mnp, child) {
			if (of_mdio_parse_addr(priv->dev, child) == port)
				break;
		}
		if (child)
			ret = of_mdiobus_phy_device_register(bus, phy, child, port);
		else
			ret = phy_device_register(phy);
		of_node_put(child);
		if (ret) {
			phy_device_free(phy);
			dev_err(priv->dev, "PHY registration port %d failed: %d\n", port, ret);
			return ret;
		}
		/* Registered PHYs are removed and freed by managed bus teardown. */
	}
	return 0;
}

static int rtl8372n_setup_mdio(struct rtl837x_priv *priv)
{
	struct device_node *np = priv->dev->of_node;
	struct device_node *mnp;
	struct dsa_switch *ds = priv->ds;
	struct device *dev = priv->dev;
	struct mii_bus *bus;
	struct phy_device *phy;
	int port, ret = 0;

	mnp = of_get_child_by_name(np, "mdio");

	if (mnp && !of_device_is_available(mnp)) {
		ret = -ENODEV;
		goto out;
	}

	bus = devm_mdiobus_alloc(dev);
	if (!bus) {
		ret = -ENOMEM;
		goto out;
	}

	/* DSA falls back to this bus for user ports without a phy-handle. */
	ds->user_mii_bus = bus;

	bus->priv = priv;
	bus->name = KBUILD_MODNAME "-mii";
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));
	bus->read_c45 = rtl8372n_mdio_phy_read_c45;
	bus->write_c45 = rtl8372n_mdio_phy_write_c45;
	bus->parent = dev;
	/* Internal PHYs are registered explicitly, for enabled ports only. */
	bus->phy_mask = ~0;
	if (mnp) {
		device_set_node(&bus->dev, of_fwnode_handle(mnp));
		/* Match the default used by __of_mdiobus_register(). */
		bus->reset_delay_us = RTL8372N_MDIO_RESET_DELAY_US;
		of_property_read_u32(mnp, "reset-delay-us", &bus->reset_delay_us);
		of_property_read_u32(mnp, "reset-post-delay-us", &bus->reset_post_delay_us);
	}

	ret = devm_mdiobus_register(dev, bus);
	if (!ret)
		ret = rtl8372n_register_phys(priv, bus, mnp);
	if (!ret) {
		for (port = 4; port <= 7; port++) {
			if (!(dsa_user_ports(ds) & BIT(port)))
				continue;
			phy = mdiobus_get_phy(bus, port);
			if (!phy) {
				dev_err(dev, "port %d: PHY device absent\n", port);
				ret = -ENODEV;
				continue;
			}
			/*
			 * phy->drv survives a failed phylib probe, so check the
			 * binding itself. Unbound, phylib falls back to genphy.
			 */
			device_lock(&phy->mdio.dev);
			if (!device_is_bound(&phy->mdio.dev))
				dev_warn(dev,
					 "port %d: no PHY driver bound to C45 ID %#010x (probe failed or kmod-phy-realtek missing)\n",
					 port, phy->c45_ids.device_ids[MDIO_MMD_PMAPMD]);
			else
				dev_dbg(dev, "port %d: PHY driver %s\n", port,
					phy->drv->name);
			device_unlock(&phy->mdio.dev);
			/*
			 * The MACs implement no LPI, so phylink leaves EEE alone.
			 * Keep it off, as the SDK-based driver did, instead of
			 * advertising the PHY's reset default.
			 */
			phy_disable_eee(phy);
		}
	}
	if (ret) {
		ds->user_mii_bus = NULL;
		dev_err(dev, "failed to register MDIO bus: %d\n", ret);
	}

out:
	of_node_put(mnp);
	return ret;
}

static unsigned int rtl8372n_sds_pcs_inband_caps(struct phylink_pcs *pcs,
						 phy_interface_t interface)
{
	struct rtl8372n_pcs *p = container_of(pcs, struct rtl8372n_pcs, pcs);

	if ((p->index == 3 || p->index == 8) &&
	    interface == PHY_INTERFACE_MODE_10GBASER)
		return LINK_INBAND_DISABLE;

	return 0;
}

static void rtl8372n_sds_pcs_get_state(struct phylink_pcs *pcs,
				     unsigned int neg_mode,
				     struct phylink_link_state *state)
{
	struct rtl8372n_pcs *p = container_of(pcs, struct rtl8372n_pcs, pcs);
	struct rtl837x_priv *priv = p->priv;
	u32 link, duplex, rx_pause, tx_pause, speed;
	int port = p->index;

	/* Never expose a partially read snapshot after a transport failure. */
	state->link = false;
	state->an_complete = false;
	state->speed = SPEED_UNKNOWN;
	state->duplex = DUPLEX_UNKNOWN;
	state->pause &= ~(MLO_PAUSE_RX | MLO_PAUSE_TX);
	if (rtl837x_reg_bits_read(priv, RTL837X_MAC_LINK_STATUS,
				 BIT(port), &link) || !link)
		return;
	if (rtl837x_reg_bits_read(priv, RTL837X_MAC_DUPLEX_STATUS,
				 BIT(port), &duplex) ||
	    rtl837x_reg_bits_read(priv, RTL837X_MAC_RX_PAUSE_STATUS,
				 BIT(port), &rx_pause) ||
	    rtl837x_reg_bits_read(priv, RTL837X_MAC_TX_PAUSE_STATUS,
				 BIT(port), &tx_pause) ||
	    rtl837x_reg_bits_read(priv, RTL837X_MAC_SPEED_STATUS_REG(port),
				 RTL837X_MAC_SPEED_STATUS_MASK(port), &speed))
		return;

	/* P0 exposes this PCS only for a fixed 10GBASE-R CPU link. */
	if (speed != 4 || !duplex)
		return;
	state->speed = SPEED_10000;
	state->duplex = DUPLEX_FULL;
	if (rx_pause)
		state->pause |= MLO_PAUSE_RX;
	if (tx_pause)
		state->pause |= MLO_PAUSE_TX;
	state->an_complete = true;
	state->link = true;
}

static int rtl8372n_sds_pcs_config(struct phylink_pcs *pcs,
				   unsigned int neg_mode,
				   phy_interface_t interface,
				   const unsigned long *advertising,
				   bool permit_pause_to_mac)
{
	struct rtl8372n_pcs *p = container_of(pcs, struct rtl8372n_pcs, pcs);
	struct rtl837x_priv *priv = p->priv;
	u32 mask;

	if (interface != PHY_INTERFACE_MODE_10GBASER)
		return -EOPNOTSUPP;
	if (p->index != 3 && p->index != 8)
		return -EINVAL;

	mask = p->index == 3 ? RTL837X_SDS0_MODE_MASK : RTL837X_SDS1_MODE_MASK;
	return rtl837x_reg_bits_write(priv, RTL837X_SDS_MODE_SELECT, mask,
				      RTL837X_SDS_MODE_10GBASE_R);
}

static void rtl8372n_sds_pcs_link_up(struct phylink_pcs *pcs, unsigned int neg_mode,
			    phy_interface_t interface, int speed, int duplex)
{
	struct rtl8372n_pcs *_pcs = container_of(pcs, struct rtl8372n_pcs, pcs);
	struct rtl837x_priv *priv = _pcs->priv;
	int port = _pcs->index;

	dev_dbg(priv->dev, "[%s]PCS link up serdes (%d) mode (%s)\n", __func__,
			  PORT_TO_SERDES_IDX(port),
			  phy_modes(interface));
}

static const struct phylink_pcs_ops rtl8372n_sds_pcs_ops = {
	.pcs_inband_caps = rtl8372n_sds_pcs_inband_caps,
	.pcs_get_state = rtl8372n_sds_pcs_get_state,
	.pcs_config = rtl8372n_sds_pcs_config,
	.pcs_link_up = rtl8372n_sds_pcs_link_up,
};

static void rtl8372n_phylink_get_caps(struct dsa_switch *ds, int port,
				       struct phylink_config *config)
{
	if (IS_SERDES_PORT(port)) {
		__set_bit(PHY_INTERFACE_MODE_10GBASER,
			  config->supported_interfaces);
		config->mac_capabilities = MAC_10000FD |
			MAC_SYM_PAUSE | MAC_ASYM_PAUSE;
	} else if (port >= 4 && port <= 7) {
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);
		config->mac_capabilities = MAC_2500FD | MAC_1000 | MAC_100 |
			MAC_10 | MAC_SYM_PAUSE | MAC_ASYM_PAUSE;
	}
}

static struct phylink_pcs *rtl8372n_phylink_mac_select_pcs(struct phylink_config *config,
						phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct rtl837x_priv *priv = dp->ds->priv;
	struct rtl8372n *chip_data = priv->chip_data;

	if (IS_SERDES_PORT(dp->index))
		return &(chip_data->pcs[dp->index].pcs);
	return NULL;
}

static void rtl8372n_phylink_mac_config(struct phylink_config *config, unsigned int mode,
			const struct phylink_link_state *state)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct rtl837x_priv *priv = dp->ds->priv;

	if (!IS_SERDES_PORT(dp->index))
		return;
	dev_dbg(priv->dev, "[%s]: port:%d interface:%s speed:%s duplex:%s advertising:%*pb pause:0x%x\n", __func__,
			   dp->index, phy_modes(state->interface), phy_speed_to_str(state->speed),
			   phy_duplex_to_str(state->duplex), __ETHTOOL_LINK_MODE_MASK_NBITS, state->advertising, state->pause);
}

static void rtl8372n_phylink_mac_link_down(struct phylink_config *config, unsigned int mode,
				phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct rtl837x_priv *priv = dp->ds->priv;

	if (!IS_SERDES_PORT(dp->index))
		return;

	dev_dbg(priv->dev, "[%s]: port:%d interface:%s\n", __func__,
			   dp->index, phy_modes(interface));
}

static void rtl8372n_phylink_mac_link_up(struct phylink_config *config,
			struct phy_device *phy, unsigned int mode,
			phy_interface_t interface, int speed, int duplex,
			bool tx_pause, bool rx_pause)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct rtl837x_priv *priv = dp->ds->priv;

	if (!IS_SERDES_PORT(dp->index))
		return;
	dev_dbg(priv->dev,
			   "[%s]: port:%d mode:%s speed:%s duplex:%s tx_pause:%d rx_pause:%d\n", __func__,
			   dp->index, phy_modes(interface), phy_speed_to_str(speed),
			   phy_duplex_to_str(duplex), tx_pause, rx_pause
			);
}

static const struct phylink_mac_ops rtl8372n_phylink_mac_ops = {
	.mac_select_pcs	= rtl8372n_phylink_mac_select_pcs,
	.mac_config	= rtl8372n_phylink_mac_config,
	.mac_link_down	= rtl8372n_phylink_mac_link_down,
	.mac_link_up	= rtl8372n_phylink_mac_link_up,
};

static int rtl8372n_set_tag_rtl(struct dsa_switch *ds)
{
	int ret;
	struct rtl837x_priv *priv = ds->priv;
	struct dsa_port *dp, *cpu_dp = NULL;

	/* This P0 implementation supports exactly one CPU port. */
	dsa_switch_for_each_cpu_port(dp, ds) {
		if (cpu_dp)
			return -EOPNOTSUPP;
		cpu_dp = dp;
	}

	if (!cpu_dp)
		return -ENODEV;

	/* Set external CPU DSA tag insert mode. */
	/*
	 *	CPU_INSERT_TO_ALL = 0,
	 *	CPU_INSERT_TO_TRAPPING,
	 *	CPU_INSERT_TO_NONE,
	 *	CPU_INSERT_END
	 */
	ret = rtl837x_reg_bits_write(priv, RTL837X_CPU_TAG_CTRL,
			  RTL837X_CPU_TAG_INSERT_MODE, 0
			);
	if (ret)
		return ret;

	/* Set external CPU port. */
	ret = rtl837x_reg_bits_write(priv, RTL837X_EXTERNAL_CPU_PORT,
			  RTL837X_EXTERNAL_CPU_PORT_MASK, cpu_dp->index
			);
	if (ret)
		return ret;

	/* Enable CPU tag. */
	ret = rtl837x_reg_bits_write(priv, RTL837X_CPU_TAG_CTRL,
			  RTL837X_CPU_TAG_ENABLE, 1
			);
	if (ret)
		return ret;

	/*
	 * Only the CPU port parses an RTL8_4 tag: a user port that did would
	 * take a forged 0x8899 frame for a CPU-sent one and deliver it to the
	 * ports its tag names, across isolation. So the mask of ports 0-8 is
	 * written whole, the CPU port alone, instead of OR-ing the CPU port
	 * into a reset value never read (as the SDK's cpuTag_enable_set()
	 * does). Bit 9, the 8051's own tag awareness, is left as read: the
	 * 8051 reaches no port (its isolation mask is 0).
	 */
	ret = rtl837x_reg_bits_write(priv, RTL837X_CPU_TAG_AWARE,
				     RTL837X_CPU_TAG_AWARE_PORTS,
				     BIT(cpu_dp->index));
	if (ret)
		return ret;

	return 0;
}

static int rtl8372n_port_set_isolation(struct rtl837x_priv *priv, int port,
					u32 mask)
{
	return rtl837x_reg_write(priv, RTL837X_PORT_ISOLATION_REG(port), mask);
}

/*
 * Port isolation (0x50c0 + 4p) is the forwarding domain: the mask of an
 * ingress port lists the ports its frames may leave by. The CPU port
 * reaches every user port and every user port the CPU. The invariant for
 * the rest: two user ports are linked, each in the other's mask, only while
 * both offload the same bridge and both are in the same known hardware VLAN
 * mode (priv->vlan_mode). A standalone port, or one whose mode is unknown,
 * reaches the CPU alone. A linked port's mask also lists the port itself
 * (rtl8372n_port_link()); that opens no path to another port.
 *
 * Why the mode matters: a VLAN-unaware port has no ingress filter, admits
 * every VID and its frames skip the egress membership check, so a
 * VLAN-unaware port linked to a VLAN-aware one would carry frames of any
 * VID into that port's VLANs. A port therefore changes mode only while it
 * is unlinked: rtl8372n_port_vlan_filtering() unlinks it, reprograms it,
 * records the new mode and links it with the members now in that mode. DSA
 * toggles a bridge's vlan_filtering one port at a time; meanwhile the bridge
 * is two meshes that do not forward to each other in hardware, and it is
 * whole again once every port has been toggled. If one port fails, DSA
 * does not change back the ports before it and the bridge reverts: the two
 * meshes stay apart until the toggle is repeated, which an administrative
 * down/up does not do (rtl8372n_fdb_resync_work() logs it). A port joining a
 * VLAN-aware bridge is VLAN-unaware until DSA's attribute sync, so it is
 * linked with the other members only once that sync has made it VLAN-aware.
 *
 * Linking puts the port into its peers' masks first and opens its own mask
 * last; unlinking closes its own mask first, then takes the port out of
 * every other user port's mask. A mode change goes ahead only once the
 * unlink has succeeded. A port whose unlink fails may still be linked to
 * ports it must not reach: it is held, marked in isolation_dirty and put in
 * CIST DISABLED at once. So is a port that a bridge leave, a failed join or
 * the retry below leaves unlinked but not in its target state. A held port
 * stays DISABLED, whatever DSA asks, until one sequence has unlinked it,
 * given it its target VLAN mode and, in a bridge, linked it with the
 * members in that mode: only then is the hold released
 * (rtl8372n_port_release()), and the state DSA last set written back, or
 * the state DSA asks for when rtl8372n_port_stp_state_set() retries
 * (rtl8372n_port_isolation_held()). vlan_lock covers these masks, the hold
 * mask and the mode record.
 */

/* The members user @port is linked with under the invariant above. */
static u16 rtl8372n_port_link_peers(struct dsa_switch *ds, int port)
{
	struct rtl837x_priv *priv = ds->priv;
	struct dsa_port *dp = dsa_to_port(ds, port), *other;
	enum rtl8372n_vlan_mode mode = priv->vlan_mode[port];
	u16 peers = 0;

	if (mode == RTL8372N_VLAN_MODE_UNKNOWN)
		return 0;
	dsa_switch_for_each_user_port(other, ds) {
		if (other != dp && dsa_port_bridge_same(dp, other) &&
		    priv->vlan_mode[other->index] == mode)
			peers |= BIT(other->index);
	}
	return peers;
}

/* Write back the CIST state DSA last set for @port. */
static int rtl8372n_port_stp_restore(struct dsa_switch *ds, int port)
{
	return rtl8372n_stp_set_state(ds->priv, port,
				      rtl8372n_stp_from_br(dsa_to_port(ds, port)->stp_state));
}

/*
 * Hold user @port: mark it and put it in CIST DISABLED, where
 * rtl8372n_port_stp_state_set() keeps it until a retry has put it in its
 * target state. Returns the error of the DISABLED write, for the caller's
 * log line.
 */
static int rtl8372n_port_hold(struct dsa_switch *ds, int port)
{
	struct rtl837x_priv *priv = ds->priv;

	lockdep_assert_held(&priv->vlan_lock);

	priv->isolation_dirty |= BIT(port);
	return rtl8372n_stp_set_state(priv, port, RTL8372N_STP_DISABLED);
}

/*
 * User @port is in its target state: unlinked, in its target VLAN mode and,
 * in a bridge, linked with the members in that mode. Release its hold, if
 * any, and with @restore write back the CIST state DSA last set; without it
 * the caller writes a state next. A port that is not held is left as is.
 */
void rtl8372n_port_release(struct dsa_switch *ds, int port, bool restore)
{
	struct rtl837x_priv *priv = ds->priv;
	int err;

	lockdep_assert_held(&priv->vlan_lock);

	if (!(priv->isolation_dirty & BIT(port)))
		return;
	priv->isolation_dirty &= ~BIT(port);
	err = restore ? rtl8372n_port_stp_restore(ds, port) : 0;
	if (err)
		dev_err(priv->dev,
			"port %d: hold released, STP state not restored: %d\n",
			port, err);
	else
		dev_warn(priv->dev, "port %d: hold released\n", port);
}

/*
 * Close every hardware path between user @port and the other user ports,
 * members of its bridge or not: its own mask goes back to the CPU alone,
 * then it leaves every other user port's mask. Every write is tried; the
 * first error is returned, and the port is then held. A success releases
 * no hold: the caller does that once the port is in its target state.
 */
int rtl8372n_port_unlink(struct dsa_switch *ds, int port)
{
	struct rtl837x_priv *priv = ds->priv;
	struct dsa_port *dp;
	int ret, err;

	lockdep_assert_held(&priv->vlan_lock);

	ret = rtl8372n_port_set_isolation(priv, port, dsa_cpu_ports(ds));
	if (ret)
		dev_err(priv->dev, "port %d: isolation not restored: %d\n",
			port, ret);
	dsa_switch_for_each_user_port(dp, ds) {
		if (dp->index == port)
			continue;
		err = regmap_update_bits(priv->map,
					 RTL837X_PORT_ISOLATION_REG(dp->index),
					 BIT(port), 0);
		if (err)
			dev_err(priv->dev,
				"port %d: still reachable from port %d: %d\n",
				port, dp->index, err);
		ret = ret ?: err;
	}

	if (ret) {
		err = rtl8372n_port_hold(ds, port);
		dev_err(priv->dev, "port %d: may still be linked, %s: %d\n",
			port, err ? "CIST DISABLED not set" : "held DISABLED",
			err);
	}
	return ret;
}

/*
 * Link user @port with the members in its VLAN mode: into their masks
 * first, then its own mask opened to the CPU and them. Linking a port that
 * is linked already writes the same bits again. On failure the port may be
 * linked in part, with members in its mode only, as the invariant allows:
 * a caller that goes on to change its mode, or holds it, unlinks it again,
 * and must not change its mode unless that succeeds. @peers receives the
 * members linked.
 *
 * A port of the offloaded bridge in a known mode also lists itself, as
 * every mask of the SDK-based driver did: a destination learned on the
 * ingress port (two hosts behind one unmanaged switch) is then inside the
 * isolation mask, and the source-port filter (0x4f84, whose user-port bits
 * setup clears) drops the frame. Without the bit, whether the chip counts
 * such a hit as outside isolation is undocumented, and if it does, 0x5350
 * bit 8 floods it as unknown to the CPU and the other members. A
 * standalone port, or one in an unknown mode, reaches the CPU alone.
 */
int rtl8372n_port_link(struct dsa_switch *ds, int port, u16 *peers)
{
	struct rtl837x_priv *priv = ds->priv;
	u32 own = dsa_cpu_ports(ds);
	unsigned long members;
	unsigned int i;
	int ret;

	lockdep_assert_held(&priv->vlan_lock);

	members = rtl8372n_port_link_peers(ds, port);
	for_each_set_bit(i, &members, RTL8372N_NUM_PORTS) {
		ret = regmap_update_bits(priv->map,
					 RTL837X_PORT_ISOLATION_REG(i),
					 BIT(port), BIT(port));
		if (ret)
			return ret;
	}
	if (dsa_port_bridge_dev_get(dsa_to_port(ds, port)) &&
	    priv->vlan_mode[port] != RTL8372N_VLAN_MODE_UNKNOWN)
		own |= BIT(port);
	ret = rtl8372n_port_set_isolation(priv, port, own | members);
	if (ret)
		return ret;
	*peers = members;
	return 0;
}

/*
 * The VLAN mode user @port must be in: DSA's belief while it offloads a
 * bridge, VLAN-unaware while standalone (needs_standalone_vlan_filtering
 * is false).
 */
static bool rtl8372n_port_vlan_target(struct dsa_switch *ds, int port)
{
	struct dsa_port *dp = dsa_to_port(ds, port);

	return dsa_port_bridge_dev_get(dp) && dsa_port_is_vlan_filtering(dp);
}

/*
 * Whether user @port must stay in CIST DISABLED: it is held and could not
 * be put right. A held port, or one whose VLAN mode is unknown or not its
 * target after failures, is put right here: unlinked (again), given its
 * target mode (rtl8372n_port_vlan_mode_repair()), then linked again with
 * the members in that mode (none when standalone). The repair comes first
 * because a port held after a failed mode change may keep the mode that
 * DSA rejected; linking it by that record would open a path in a mode
 * neither DSA nor the bridge believes in. The hold is released only once
 * all three have succeeded. If one fails, the port is held, unlinked again
 * if the link failed, rather than given a state DSA asks for while it is
 * unlinked from its members or in an unknown mode.
 *
 * A port of the offloaded bridge that is in its target mode and not held
 * is linked again all the same: a failed relink can leave such a port
 * unlinked, with nothing recorded (rtl8372n_port_vlan_fallback(), or a
 * relink of its own that fails here), and linking a linked port writes the
 * same bits again. A failure there is logged; the port, linked in part at
 * worst, gets the state DSA asks for and is linked again at its next state
 * change.
 *
 * Only port_stp_state_set() calls this, which writes the state DSA asks
 * for, or DISABLED while the port is held, right after. An administrative
 * down/up retries it, and so does a link flap on a port of the offloaded
 * bridge (the bridge's carrier check sets its STP state); a standalone
 * port's carrier change does not reach the driver, so a held standalone
 * port needs `ip link set <port> down; ip link set <port> up` (or a bridge
 * join). dp->stp_state still holds the previous state here (DSA updates it
 * after the op), so a released port is not given it back: it stays
 * DISABLED while it is linked again, until the caller's write.
 */
bool rtl8372n_port_isolation_held(struct dsa_switch *ds, int port)
{
	struct rtl837x_priv *priv = ds->priv;
	bool held, target;
	u16 peers;
	int ret;

	if (!dsa_is_user_port(ds, port))
		return false;

	mutex_lock(&priv->vlan_lock);
	target = rtl8372n_port_vlan_target(ds, port);
	if ((priv->isolation_dirty & BIT(port)) ||
	    priv->vlan_mode[port] != rtl8372n_vlan_mode(target)) {
		ret = rtl8372n_port_unlink(ds, port);
		if (!ret)
			ret = rtl8372n_port_vlan_mode_repair(ds, port, target);
		if (!ret) {
			ret = rtl8372n_port_link(ds, port, &peers);
			if (ret) {
				dev_err(priv->dev,
					"port %d: not linked again: %d\n",
					port, ret);
				rtl8372n_port_unlink(ds, port);
			}
		}
		/* The caller writes DISABLED for a held port. */
		if (ret)
			priv->isolation_dirty |= BIT(port);
		else
			rtl8372n_port_release(ds, port, false);
	} else if (dsa_port_bridge_dev_get(dsa_to_port(ds, port))) {
		ret = rtl8372n_port_link(ds, port, &peers);
		if (ret)
			dev_err(priv->dev, "port %d: not linked again: %d\n",
				port, ret);
	}
	held = priv->isolation_dirty & BIT(port);
	mutex_unlock(&priv->vlan_lock);
	return held;
}

/*
 * Setup calls this with offload=0 (rtl8372n_offload) and, instead of failing,
 * when a step that only bridge offload relies on fails: the switch then works
 * as in P0, standalone ports through the CPU, and every bridge stays in
 * software (port_bridge_join returns -EOPNOTSUPP). @reason must be a string
 * literal; keep it under about 28 characters, since it ends up in an 80-byte
 * netlink extack message. The first reason is kept; setup clears the gate.
 */
void rtl8372n_disable_offload(struct rtl837x_priv *priv, const char *reason)
{
	if (!priv->offload_disabled)
		priv->offload_disabled = reason;
	dev_warn(priv->dev, "hardware bridging disabled: %s\n", reason);
}

/*
 * An 8021q upper must keep its software semantics (switchdev.rst): frames
 * with its VID terminate on the upper and are not bridged. The switch cannot
 * do that on a port of the offloaded bridge: a VLAN-unaware member forwards
 * every VID to the other members, and a VLAN-aware member's VID shares one
 * hardware entry with the bridge's VLANs. So an 8021q upper is refused on a
 * port that offloads a bridge, and a port that already has one is bridged in
 * software instead (port_bridge_join). A port bridged in software, or
 * standalone, keeps its uppers as in P0.
 */
static int rtl8372n_port_prechangeupper(struct dsa_switch *ds, int port,
					struct netdev_notifier_changeupper_info *info)
{
	if (!info->linking || !is_vlan_dev(info->upper_dev) ||
	    !dsa_port_bridge_dev_get(dsa_to_port(ds, port)))
		return 0;
	NL_SET_ERR_MSG_MOD(info->info.extack,
			   "8021q uppers are not supported on ports of the offloaded bridge");
	return -EBUSY;
}

static bool rtl8372n_port_has_vlan_upper(struct dsa_switch *ds, int port)
{
	struct net_device *upper;
	struct list_head *iter;
	bool found = false;

	rcu_read_lock();
	netdev_for_each_upper_dev_rcu(dsa_to_port(ds, port)->user, upper, iter) {
		if (is_vlan_dev(upper)) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

/*
 * Offload one bridge (ds->max_num_bridges = 1), VLAN-aware or not, unless
 * setup disabled offload, the port has an 8021q upper or the bridge's VLAN
 * protocol is not 802.1Q: -EOPNOTSUPP makes DSA bridge it in software,
 * standalone and CPU-only in the switch. The port is unlinked, which a
 * standalone port already is, so that a join also clears whatever an
 * earlier failure left, then put in CIST DISABLED, then linked with the
 * members in its VLAN mode (see the isolation invariant above), which
 * releases a hold an earlier failure left on it. DISABLED is the state
 * DSA's attribute sync writes next for a new bridge port, followed by its
 * flags (learning, flooding), VLAN awareness and the ageing time. A port
 * joining a VLAN-filtering bridge is thus linked with the members only when
 * that sync makes it VLAN-aware; its VLANs follow from the bridge
 * (rtl8372n_vlan.c), after any stale one is dropped.
 */
static int rtl8372n_port_bridge_join(struct dsa_switch *ds, int port,
				     struct dsa_bridge bridge,
				     bool *tx_fwd_offload,
				     struct netlink_ext_ack *extack)
{
	struct dsa_port *dp = dsa_to_port(ds, port);
	struct rtl837x_priv *priv = ds->priv;
	bool unlinked;
	u16 peers = 0;
	u16 proto;
	int ret, err;

	if (!dsa_is_user_port(ds, port))
		return -EINVAL;
	if (priv->offload_disabled) {
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "hardware bridging disabled: %s at setup",
				       priv->offload_disabled);
		dev_dbg(priv->dev, "port %d: bridged in software: %s\n", port,
			priv->offload_disabled);
		return -EOPNOTSUPP;
	}
	if (rtl8372n_port_has_vlan_upper(ds, port)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Port has 8021q uppers, bridged in software");
		return -EOPNOTSUPP;
	}
	/*
	 * The driver programs 802.1Q VLANs only: offloaded, a bridge with
	 * another vlan_protocol (802.1ad) would have its S-tagged frames
	 * switched by C-tag rules without any error. DSA does not pass a
	 * later protocol change on to the driver, so this holds only for the
	 * protocol the bridge has when the port joins.
	 */
	if (!br_vlan_get_proto(bridge.dev, &proto) && proto != ETH_P_8021Q) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Only 802.1Q bridges are offloaded, bridged in software");
		dev_dbg(priv->dev,
			"port %d: bridged in software: VLAN protocol 0x%04x\n",
			port, proto);
		return -EOPNOTSUPP;
	}

	mutex_lock(&priv->vlan_lock);
	ret = rtl8372n_port_unlink(ds, port);
	unlinked = !ret;
	if (!ret)
		ret = rtl8372n_stp_set_state(priv, port, RTL8372N_STP_DISABLED);
	if (!ret)
		ret = rtl8372n_port_vlan_mode_repair(ds, port,
						     dsa_port_is_vlan_filtering(dp));
	if (!ret)
		ret = rtl8372n_port_vlan_drop_stale(ds, port);
	if (!ret) {
		ret = rtl8372n_port_link(ds, port, &peers);
		if (ret)
			unlinked = !rtl8372n_port_unlink(ds, port);
	}
	/*
	 * DSA calls no bridge leave when the join itself fails: restore the
	 * standalone state of a port that is unlinked, VLAN-unaware too (the
	 * repair above follows DSA's belief, which can be VLAN-aware after a
	 * rolled-back join), or hold it if that fails, as a leave does. A
	 * port that could not be unlinked is held already.
	 */
	if (!ret) {
		rtl8372n_port_release(ds, port, false);
	} else if (unlinked &&
		   !rtl8372n_port_vlan_mode_repair(ds, port, false)) {
		rtl8372n_port_release(ds, port, false);
		rtl8372n_port_stp_restore(ds, port);
	} else if (unlinked) {
		err = rtl8372n_port_hold(ds, port);
		dev_err(priv->dev,
			"port %d: join undone in an unknown VLAN mode, %s: %d\n",
			port, err ? "CIST DISABLED not set" : "held DISABLED",
			err);
	}
	mutex_unlock(&priv->vlan_lock);
	if (ret) {
		dev_err(priv->dev, "port %d: bridge join failed: %d\n", port,
			ret);
		return ret;
	}

	dev_dbg(priv->dev, "port %d joined bridge %u, linked %#x\n", port,
		bridge.num, peers);
	return 0;
}

/*
 * Return the port to standalone forwarding: unlinked first, then
 * VLAN-unaware, no flooding towards it, no learning, its learned entries
 * flushed, and FORWARDING. A port that could not be unlinked keeps its
 * mode, and one that could not be made VLAN-unaware is in an unknown mode:
 * either is held DISABLED instead, even against the FORWARDING DSA writes
 * next, which retries it (rtl8372n_port_isolation_held()); the hold is
 * released once it is unlinked and VLAN-unaware. After a normal leave DSA
 * resets the flags and the state again, and makes a port of a VLAN-aware
 * bridge VLAN-unaware (port_vlan_filtering), which finds it so already; but
 * when DSA rolls back a join this driver accepted, nothing follows this
 * call, and dp->vlan_filtering may still say VLAN-aware (an error after the
 * attribute sync made the port VLAN-aware). The port is made VLAN-unaware
 * here whatever DSA believes, since every standalone port is
 * (needs_standalone_vlan_filtering is false) and a VLAN-aware one with no
 * VLAN would receive nothing. A void op: failures are logged and the next
 * step still runs.
 */
static void rtl8372n_port_bridge_leave(struct dsa_switch *ds, int port,
				       struct dsa_bridge bridge)
{
	struct rtl837x_priv *priv = ds->priv;
	bool unlinked, standalone = false;
	int ret, err = 0;

	if (!dsa_is_user_port(ds, port))
		return;

	mutex_lock(&priv->vlan_lock);
	unlinked = !rtl8372n_port_unlink(ds, port);
	/* Never change the mode of a port that may still be linked. */
	if (unlinked)
		standalone = !rtl8372n_port_vlan_mode_standalone(ds, port);
	if (standalone)
		rtl8372n_port_release(ds, port, false);
	else if (unlinked)
		err = rtl8372n_port_hold(ds, port);
	mutex_unlock(&priv->vlan_lock);
	ret = rtl8372n_port_set_flood(ds, port, RTL8372N_FLOOD_FLAGS, false);
	if (ret)
		dev_err(priv->dev, "port %d: flooding not cleared: %d\n",
			port, ret);
	ret = rtl8372n_port_set_learning(ds, port, false);
	if (ret)
		dev_err(priv->dev, "port %d: learning not disabled: %d\n",
			port, ret);
	rtl8372n_port_fast_age(ds, port);
	if (!unlinked) {
		dev_err(priv->dev,
			"port %d: left bridge %u but may still reach its members, held DISABLED\n",
			port, bridge.num);
		return;
	}
	if (!standalone) {
		dev_err(priv->dev,
			"port %d: left bridge %u in an unknown VLAN mode, %s: %d\n",
			port, bridge.num,
			err ? "CIST DISABLED not set" : "held DISABLED", err);
		return;
	}
	ret = rtl8372n_stp_set_state(priv, port, RTL8372N_STP_FORWARDING);
	if (ret)
		dev_err(priv->dev, "port %d: STP forwarding not restored: %d\n",
			port, ret);
	dev_dbg(priv->dev, "port %d left bridge %u\n", port, bridge.num);
}

/*
 * Log one setup readback line. Returns true if the register was read and
 * matched, so that a caller can turn bridge offload off on a mismatch or a
 * failed read of a control only offload relies on.
 */
bool rtl8372n_report_p0_reg(struct rtl837x_priv *priv, const char *name,
			    u32 reg, u32 mask, u32 expected)
{
	u32 value;
	int ret;

	ret = rtl837x_reg_read(priv, reg, &value);
	if (ret) {
		dev_warn(priv->dev, "P0 readback %s at %#x failed: %d\n",
			 name, reg, ret);
		return false;
	}

	if ((value & mask) == (expected & mask)) {
		dev_info(priv->dev,
			 "P0 readback %s at %#x: value=%#x expected=%#x mask=%#x PASS\n",
			 name, reg, value, expected, mask);
		return true;
	}
	dev_warn(priv->dev,
		 "P0 readback %s at %#x: value=%#x expected=%#x mask=%#x MISMATCH\n",
		 name, reg, value, expected, mask);
	return false;
}

/* Log a register whose meaning is not settled enough to check it. */
void rtl8372n_report_p0_value(struct rtl837x_priv *priv, const char *name,
			      u32 reg)
{
	u32 value;
	int ret;

	ret = rtl837x_reg_read(priv, reg, &value);
	if (ret)
		dev_warn(priv->dev, "P0 readback %s at %#x failed: %d\n",
			 name, reg, ret);
	else
		dev_info(priv->dev,
			 "P0 readback %s at %#x: value=%#x (not checked)\n",
			 name, reg, value);
}

static bool rtl8372n_report_p0_field(struct rtl837x_priv *priv,
				     const char *name, u32 reg, u32 mask,
				     u32 expected)
{
	u32 value;
	int ret;

	ret = rtl837x_reg_bits_read(priv, reg, mask, &value);
	if (ret) {
		dev_warn(priv->dev, "P0 readback %s at %#x failed: %d\n",
			 name, reg, ret);
		return false;
	}

	if (value == expected) {
		dev_info(priv->dev,
			 "P0 readback %s at %#x: value=%#x expected=%#x mask=%#x PASS\n",
			 name, reg, value, expected, mask);
		return true;
	}
	dev_warn(priv->dev,
		 "P0 readback %s at %#x: value=%#x expected=%#x MISMATCH\n",
		 name, reg, value, expected);
	return false;
}

static const u32 rtl8372n_cpu_flood_regs[] = {
	RTL837X_L2_UNKNOWN_UC_FLOOD, RTL837X_L2_UNKNOWN_MC_FLOOD,
	RTL837X_IPV4_UNKNOWN_MC_FLOOD, RTL837X_IPV6_UNKNOWN_MC_FLOOD,
	RTL837X_L2_BROADCAST_FLOOD,
};

/*
 * A VLAN entry must still hold the word setup left in it, after the VLAN
 * control writes that followed it: the canonical word of VIDs 0 and 1
 * (bits 25:0 compared), no member, untag or FID/MSTI bit for the others
 * (bits 23:0, as the table reset checks them). Bits 31:26 are reserved.
 */
static bool rtl8372n_report_p0_vlan(struct rtl837x_priv *priv, u16 vid,
				    u32 mask, u32 expected)
{
	struct rtl837x_vlan_entry vlan;
	int ret;

	ret = rtl837x_vlan_entry_read(priv, vid, &vlan);
	if (ret) {
		dev_warn(priv->dev, "P0 readback VLAN %u table entry failed: %d\n",
			 vid, ret);
		return false;
	}
	if ((vlan.raw & mask) == expected) {
		dev_info(priv->dev,
			 "P0 readback VLAN %u data0=%#x expected=%#x mask=%#x PASS\n",
			 vid, vlan.raw, expected, mask);
		return true;
	}
	dev_warn(priv->dev,
		 "P0 readback VLAN %u data0=%#x expected=%#x mask=%#x MISMATCH\n",
		 vid, vlan.raw, expected, mask);
	return false;
}

/*
 * The P0 lines (VLAN 1, isolation, learning limits, CPU flood masks, PVID,
 * accepted frame types, ingress filter, egress tag mode, filter enable)
 * cover what standalone ports need, written since P0 and bench-proven
 * there; they are informational, as is the CPU tag-aware mask, which
 * bridging does not change. The other VLAN lines check controls that
 * only bridge offload relies on and that P0 never wrote, or whose only
 * source is the SDK: VID 0, the VIDs left empty, egress transparency and
 * keep, TABLE_RST, the VID types, the learning-disable slots and the
 * port-based FID. A failed read or a mismatch of one of those turns offload
 * off, as rtl8372n_l2_report_readback() does for the L2 actions and traps.
 * Setup runs this before DSA creates the user ports, so before any bridge
 * can be offloaded.
 */
static void rtl8372n_report_p0_readback(struct dsa_switch *ds, u16 members)
{
	struct rtl837x_priv *priv = ds->priv;
	struct dsa_port *dp;
	unsigned int cpu_mask = dsa_cpu_ports(ds);
	unsigned int user_mask = dsa_user_ports(ds);
	static const u16 empty_vids[] = RTL8372N_VLAN_SAMPLE_VIDS;
	u32 accept_mask = 0, egress_tag_mask = 0, egress_tag = 0;
	bool offload = true;
	int port, i;

	/*
	 * VID 0 is the empty no-PVID VLAN; VLAN 1 holds every port. The VIDs
	 * setup leaves empty, the table reset's sentinel included, must have
	 * stayed so since the reset.
	 */
	offload &= rtl8372n_report_p0_vlan(priv, 0, RTL837X_VLAN_ENTRY_FIELDS,
					   rtl837x_vlan_entry_canonical(0, 0));
	rtl8372n_report_p0_vlan(priv, 1, RTL837X_VLAN_ENTRY_FIELDS,
				rtl837x_vlan_entry_canonical(members, members));
	for (i = 0; i < ARRAY_SIZE(empty_vids); i++)
		offload &= rtl8372n_report_p0_vlan(priv, empty_vids[i],
						   RTL837X_VLAN_ENTRY_USED, 0);

	for (port = 0; port <= RTL8372N_INTERNAL_CPU_PORT; port++) {
		u32 isolation = 0;

		if (user_mask & BIT(port))
			isolation = cpu_mask;
		else if (cpu_mask & BIT(port))
			isolation = user_mask;
		rtl8372n_report_p0_reg(priv, "port isolation",
				       RTL837X_PORT_ISOLATION_REG(port),
				       GENMASK(9, 0), isolation);
		rtl8372n_report_p0_field(priv, "learning limit",
					 RTL837X_L2_LEARN_LIMIT_REG(port),
					 RTL837X_L2_LEARN_LIMIT_MASK, 0);
	}

	for (i = 0; i < ARRAY_SIZE(rtl8372n_cpu_flood_regs); i++)
		rtl8372n_report_p0_reg(priv, "CPU flood destination",
				       rtl8372n_cpu_flood_regs[i],
				       GENMASK(9, 0), cpu_mask);

	dsa_switch_for_each_available_port(dp, ds)
		rtl8372n_report_p0_field(priv, "PVID",
					 RTL837X_PORT_PVID_REG(dp->index),
					 RTL837X_PORT_PVID_MASK(dp->index), 1);

	dsa_switch_for_each_available_port(dp, ds) {
		port = dp->index;
		accept_mask |= RTL837X_VLAN_ACCEPT_FRAME_TYPE_MASK(port);
		egress_tag_mask |= RTL837X_VLAN_EGRESS_TAG_MODE_MASK(port);
		egress_tag |= RTL837X_VLAN_EGRESS_MODE_REAL_KEEP <<
			      __ffs(RTL837X_VLAN_EGRESS_TAG_MODE_MASK(port));
		offload &= rtl8372n_report_p0_field(priv,
						    "VLAN egress transparent ports",
						    RTL837X_VLAN_EGRESS_TRANS_REG(port),
						    RTL837X_VLAN_EGRESS_TRANS_MASK(port),
						    members);
		/* Never written: nothing may override the egress tag mode. */
		offload &= rtl8372n_report_p0_field(priv, "VLAN egress keep ports",
						    RTL837X_VLAN_EGRESS_KEEP_REG(port),
						    RTL837X_VLAN_EGRESS_KEEP_MASK(port),
						    0);
	}
	rtl8372n_report_p0_reg(priv, "VLAN accepted frame types",
			       RTL837X_VLAN_INGRESS_CTRL, accept_mask, 0);
	rtl8372n_report_p0_reg(priv, "VLAN ingress filter",
			       RTL837X_VLAN_INGRESS_FILTER, members, 0);
	rtl8372n_report_p0_reg(priv, "VLAN egress tag mode",
			       RTL837X_VLAN_EGRESS_TAG, egress_tag_mask, egress_tag);
	/*
	 * Informational: a user port parsing the tag would bypass isolation
	 * whether or not a bridge is offloaded, so the offload gate would not
	 * help; a MISMATCH is a warning for the bench.
	 */
	rtl8372n_report_p0_reg(priv, "CPU tag-aware ports",
			       RTL837X_CPU_TAG_AWARE,
			       RTL837X_CPU_TAG_AWARE_PORTS, cpu_mask);
	rtl8372n_report_p0_field(priv, "VLAN filter enable",
				 RTL837X_VLAN_CTRL, RTL837X_VLAN_CTRL_FILTER, 1);
	offload &= rtl8372n_report_p0_field(priv, "VLAN table reset",
					    RTL837X_VLAN_CTRL,
					    RTL837X_VLAN_CTRL_TABLE_RST, 0);
	offload &= rtl8372n_report_p0_reg(priv, "VLAN reserved VID types",
					  RTL837X_VLAN_CTRL,
					  RTL837X_VLAN_CTRL_VID0_TYPE |
					  RTL837X_VLAN_CTRL_VID4095_TYPE,
					  RTL837X_VLAN_CTRL_VID4095_TYPE);
	for (i = 0; i < RTL837X_VLAN_L2_LRN_DIS_SLOTS; i++)
		offload &= rtl8372n_report_p0_reg(priv, "VLAN learning disable",
						  RTL837X_VLAN_L2_LRN_DIS(i),
						  RTL837X_VLAN_L2_LRN_DIS_FIELDS,
						  0);
	offload &= rtl8372n_report_p0_reg(priv, "port-based FID enable",
					  RTL837X_PORT_BASED_FID_EN, members,
					  0);
	if (!offload)
		rtl8372n_disable_offload(priv, "VLAN readback mismatch");
	rtl8372n_l2_report_readback(ds);
}

static int rtl8372n_configure_sds_polarity(struct rtl837x_priv *priv)
{
	static const char * const rx_swap[] = {
		"sds0-rx-swap", "sds1-rx-swap",
	};
	static const char * const tx_swap[] = {
		"sds0-tx-swap", "sds1-tx-swap",
	};
	struct device_node *np = priv->dev->of_node;
	int sds, ret;

	for (sds = 0; sds < ARRAY_SIZE(rx_swap); sds++) {
		if (of_property_read_bool(np, rx_swap[sds])) {
			ret = rtl837x_sds_reg_bits_write(priv, sds,
						 RTL837X_SDS_PAGE_XSG,
						 RTL837X_SDS_XSG_REG_POLARITY,
						 RTL837X_SDS_XSG_RX_POLARITY_SWAP,
						 true);
			if (ret)
				return ret;

			ret = rtl837x_sds_reg_bits_write(priv, sds,
						 RTL837X_SDS_PAGE_10GBASE_R,
						 RTL837X_SDS_10G_REG_POLARITY,
						 RTL837X_SDS_10G_RX_POLARITY_SWAP,
						 true);
			if (ret)
				return ret;
		}

		if (of_property_read_bool(np, tx_swap[sds])) {
			ret = rtl837x_sds_reg_bits_write(priv, sds,
						 RTL837X_SDS_PAGE_XSG,
						 RTL837X_SDS_XSG_REG_POLARITY,
						 RTL837X_SDS_XSG_TX_POLARITY_SWAP,
						 true);
			if (ret)
				return ret;

			ret = rtl837x_sds_reg_bits_write(priv, sds,
						 RTL837X_SDS_PAGE_10GBASE_R,
						 RTL837X_SDS_10G_REG_POLARITY,
						 RTL837X_SDS_10G_TX_POLARITY_SWAP,
						 true);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int rtl8372n_validate_topology(struct dsa_switch *ds, u16 *members)
{
	struct rtl837x_priv *priv = ds->priv;
	struct dsa_port *dp;
	phy_interface_t interface;
	unsigned int cpu_count = 0;
	int ret;

	*members = 0;
	dsa_switch_for_each_available_port(dp, ds) {
		if (dsa_port_is_dsa(dp))
			return -EOPNOTSUPP;
		ret = of_get_phy_mode(dp->dn, &interface);
		if (ret)
			return dev_err_probe(priv->dev, ret,
					     "port %d needs phy-mode\n", dp->index);
		if (dsa_port_is_cpu(dp)) {
			if (!IS_SERDES_PORT(dp->index) ||
			    interface != PHY_INTERFACE_MODE_10GBASER ||
			    !of_phy_is_fixed_link(dp->dn))
				return -EOPNOTSUPP;
			cpu_count++;
		} else if (dsa_port_is_user(dp)) {
			if (dp->index < 4 || dp->index > 7 ||
			    interface != PHY_INTERFACE_MODE_INTERNAL)
				return -EOPNOTSUPP;
		} else {
			return -EOPNOTSUPP;
		}
		*members |= BIT(dp->index);
	}
	return cpu_count == 1 && dsa_user_ports(ds) ? 0 : -EOPNOTSUPP;
}

static void rtl8372n_quiesce(struct dsa_switch *ds)
{
	struct rtl837x_priv *priv = ds->priv;
	int port, ret;

	/* Best effort on setup failure/teardown; never hide the first error. */
	for (port = 0; port <= RTL8372N_INTERNAL_CPU_PORT; port++) {
		ret = rtl8372n_port_set_isolation(priv, port, 0);
		if (ret)
			dev_warn(priv->dev, "failed to isolate port %d: %d\n", port, ret);
	}
	ret = rtl837x_phys_write_c45(priv, RTL8372N_PHY_PORT_MASK,
				    31, 0xa610, 0x2858);
	if (ret)
		dev_warn(priv->dev, "failed to power down PHYs: %d\n", ret);
	cancel_delayed_work_sync(&priv->fdb_resync_work);
	rtl8372n_fdb_reset(priv);
}

static int rtl8372n_setup_cpu_forwarding(struct dsa_switch *ds)
{
	struct rtl837x_priv *priv = ds->priv;
	unsigned int cpu_mask = dsa_cpu_ports(ds);
	unsigned int user_mask = dsa_user_ports(ds);
	int port, i, ret;

	/* Every user port starts standalone: it reaches only the CPU, learns
	 * nothing and receives no flooded frames. Only bridge join opens a
	 * user-to-user path. The internal 8051 port 9 reaches no port and
	 * learns nothing.
	 */
	for (port = 0; port <= RTL8372N_INTERNAL_CPU_PORT; port++) {
		u32 mask = 0;

		if (user_mask & BIT(port))
			mask = cpu_mask;
		else if (cpu_mask & BIT(port))
			mask = user_mask;
		ret = rtl8372n_port_set_isolation(priv, port, mask);
		if (ret)
			return ret;
		ret = rtl837x_reg_bits_write(priv, RTL837X_L2_LEARN_LIMIT_REG(port),
					     RTL837X_L2_LEARN_LIMIT_MASK, 0);
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(rtl8372n_cpu_flood_regs); i++) {
		ret = regmap_update_bits(priv->map, rtl8372n_cpu_flood_regs[i],
					 RTL837X_L2_FLOOD_MASK, cpu_mask);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * offload=0 keeps every bridge in software, as in P0: setup turns bridge
 * offload off first, so port_bridge_join() returns -EOPNOTSUPP and each user
 * port stays standalone and CPU-only. Setup is otherwise unchanged and still
 * writes what only offload needs (RMA trap, pinned VLAN and L2 controls, L2
 * clear), so the standalone checks see the same switch state either way. It
 * is the bench's phase-1 mode and the field escape hatch. The package option
 * RTL837X_DSA_OFFLOAD, on by default, builds with RTL8372N_OFFLOAD_DEFAULT=1;
 * without it offload is off. "options rtl8372n_dsa offload=0" (or =1) in
 * /etc/modules.conf overrides either default. OpenWrt's kmodloader
 * does not read the kernel command line, where rtl8372n_dsa.offload=0 only
 * reaches a built-in driver. Setup reads it at probe, so it changes only on a
 * module reload, and only the boot-time loader and modprobe read
 * modules.conf: kmodloader's insmod passes just its own arguments, so a
 * reload is "modprobe rtl8372n_dsa" or "insmod rtl8372n_dsa offload=0".
 */
#ifndef RTL8372N_OFFLOAD_DEFAULT
#define RTL8372N_OFFLOAD_DEFAULT 0
#endif
static bool rtl8372n_offload = RTL8372N_OFFLOAD_DEFAULT;
module_param_named(offload, rtl8372n_offload, bool, 0444);
MODULE_PARM_DESC(offload,
		 "Offload a bridge to the switch (default set at build time); 0 keeps every bridge in software");

static int rtl8372n_setup(struct dsa_switch *ds)
{
	struct rtl837x_priv *priv = ds->priv;
	struct rtl8372n *chip_data = priv->chip_data;
	struct dsa_port *dp;
	u16 members;
	int port, ret;

	ret = rtl8372n_validate_topology(ds, &members);
	if (ret)
		return dev_err_probe(priv->dev, ret, "unsupported P0 topology\n");
	priv->isolation_dirty = 0;
	priv->offload_disabled = NULL;
	if (!rtl8372n_offload)
		rtl8372n_disable_offload(priv, "module parameter offload=0");
	/*
	 * The L2 table has no per-bridge database, so one bridge is offloaded
	 * and a second stays in software with CPU-only ports. The CPU port
	 * never learns (learn limit 0, LEARN_DIS in the tag), so DSA installs
	 * the addresses the software bridge learns on foreign ports (Wi-Fi,
	 * VPN) as static CPU-port entries, as in rtl8365mb: unicast to them
	 * then goes to the CPU only instead of flooding to the bridged LAN
	 * ports, and a station that roams to such a port replaces its entry
	 * learned on a LAN port.
	 */
	ds->max_num_bridges = 1;
	ds->fdb_isolation = false;
	ds->assisted_learning_on_cpu_port = true;
	ret = rtl8372n_soft_reset_chip(priv);
	if (ret)
		return dev_err_probe(priv->dev, ret, "failed to reset switch\n");

	for (port = 3; port <= 8; port += 5) {
		chip_data->pcs[port].pcs.ops = &rtl8372n_sds_pcs_ops;
		__set_bit(PHY_INTERFACE_MODE_10GBASER,
			  chip_data->pcs[port].pcs.supported_interfaces);
		chip_data->pcs[port].priv = priv;
		chip_data->pcs[port].index = port;
	}

	ret = rtl837x_reg_bits_write(priv, RTL837X_SMI_MAC_TYPE,
				     RTL837X_SMI_MAC_PORT3_TYPE, 0);
	if (ret)
		goto fail;
	ret = rtl837x_reg_bits_write(priv, RTL837X_SMI_MAC_TYPE,
				     RTL837X_SMI_MAC_PORT8_TYPE, 0);
	if (ret)
		goto fail;
	ret = rtl837x_reg_bits_write(priv, RTL837X_SMI_PORT_POLLING,
				     RTL837X_SMI_PORT_POLLING_4_7, 0xf);
	if (ret)
		goto fail;
	ret = rtl837x_reg_bits_write(priv, RTL837X_SMI_CTRL,
				     RTL837X_SMI_MDC_ENABLE, 0x7);
	if (ret)
		goto fail;
	ret = rtl837x_reg_bits_write(priv, RTL837X_SMI_GLOBAL_CTRL,
				     RTL837X_SMI_POLLING_MASK, 0x1f8);
	if (ret)
		goto fail;
	msleep(20);

	ret = rtl8372n_configure_sds_polarity(priv);
	if (ret)
		goto fail;
	ret = rtl837x_phys_write_c45(priv, RTL8372N_PHY_PORT_MASK,
				    31, 0xa610, 0x2858);
	if (ret)
		goto fail;
	/*
	 * VID 0 empty, VLAN 1 = every available port, PVID 1 everywhere. From
	 * here on, a step that only bridge offload needs (the VLAN table
	 * reset, VID 0, the pinned VLAN controls and L2 actions, the L2
	 * clear, the link-local trap) turns offload off instead of failing setup
	 * (rtl8372n_disable_offload()); what standalone forwarding needs
	 * (VLAN 1, PVID 1, CPU-only isolation, learning off, the CPU flood
	 * masks, the STP states, the VLAN-unaware port mode and the CPU tag)
	 * still fails it.
	 */
	ret = rtl8372n_vlan_setup(ds);
	if (ret)
		goto fail;
	ret = rtl8372n_setup_cpu_forwarding(ds);
	if (ret)
		goto fail;
	ret = rtl8372n_l2_setup(ds);
	if (ret)
		goto fail;
	/*
	 * Start from an empty L2 table whatever the reset left. Learning is
	 * off on every port from here on, so nothing is learned again before
	 * DSA brings the ports up. The FDB record starts empty with it. P0
	 * ran without a clear, and only bridging relies on it, so a clear
	 * that does not complete, or fails, only turns bridge offload off.
	 * On the empty table, the unicast entry path that bridging needs is
	 * tried once; it runs with offload=0 too, as the bench's first
	 * evidence of that path.
	 */
	rtl8372n_fdb_reset(priv);
	ret = rtl837x_l2_clear(priv);
	if (ret == -ETIMEDOUT)
		rtl8372n_disable_offload(priv, "L2 table clear timed out");
	else if (ret)
		rtl8372n_disable_offload(priv, "L2 table clear failed");
	else if (rtl8372n_l2_self_test(ds))
		rtl8372n_disable_offload(priv, "L2 entry self-test failed");
	/*
	 * Make every port VLAN-unaware before the VLAN function is enabled;
	 * the CPU port stays so. Clear the egress transparency fields first so
	 * that they hold exactly the available ports afterwards, whatever
	 * their reset value.
	 */
	dsa_switch_for_each_available_port(dp, ds) {
		ret = rtl837x_reg_bits_write(priv,
					     RTL837X_VLAN_EGRESS_TRANS_REG(dp->index),
					     RTL837X_VLAN_EGRESS_TRANS_MASK(dp->index),
					     0);
		if (ret)
			goto fail;
	}
	dsa_switch_for_each_available_port(dp, ds) {
		ret = rtl8372n_port_set_vlan_filtering(ds, dp->index, false);
		if (ret)
			goto fail;
	}
	ret = rtl8372n_vlan_ctrl_update(priv, RTL837X_VLAN_CTRL_FILTER,
					RTL837X_VLAN_CTRL_FILTER);
	if (ret)
		goto fail;
	ret = rtl8372n_set_tag_rtl(ds);
	if (ret)
		goto fail;
	/*
	 * From here on the switch never forwards link-local frames. Without
	 * the trap, only bridging could forward them between ports.
	 */
	ret = rtl8372n_trap_setup(ds);
	if (ret) {
		dev_warn(priv->dev, "link-local trap not set: %d\n", ret);
		rtl8372n_disable_offload(priv, "link-local trap not set");
	}
	rtl8372n_report_p0_readback(ds, members);
	/*
	 * Register PHYs only after forwarding is set up. They stay in PMA low
	 * power (OCP 0xa610 mirrors PMA/PMD control 1) until phylib resumes
	 * them on attach; from then on phylib owns their power state.
	 */
	ret = rtl8372n_setup_mdio(priv);
	if (ret)
		goto fail;
	return 0;

fail:
	rtl8372n_quiesce(ds);
	return dev_err_probe(priv->dev, ret, "switch setup failed\n");
}

static const struct dsa_switch_ops rtl8372n_switch_ops_mdio = {
	.get_tag_protocol = rtl8372n_get_tag_protocol,
	.setup = rtl8372n_setup,
	.teardown = rtl8372n_quiesce,
	.phylink_get_caps = rtl8372n_phylink_get_caps,
	.set_ageing_time = rtl8372n_set_ageing_time,
	.port_prechangeupper = rtl8372n_port_prechangeupper,
	.port_bridge_join = rtl8372n_port_bridge_join,
	.port_bridge_leave = rtl8372n_port_bridge_leave,
	.port_pre_bridge_flags = rtl8372n_port_pre_bridge_flags,
	.port_bridge_flags = rtl8372n_port_bridge_flags,
	.port_vlan_filtering = rtl8372n_port_vlan_filtering,
	.port_vlan_add = rtl8372n_port_vlan_add,
	.port_vlan_del = rtl8372n_port_vlan_del,
	.port_stp_state_set = rtl8372n_port_stp_state_set,
	.port_fast_age = rtl8372n_port_fast_age,
	.port_fdb_add = rtl8372n_port_fdb_add,
	.port_fdb_del = rtl8372n_port_fdb_del,
	.port_fdb_dump = rtl8372n_port_fdb_dump,
};

static const struct rtl837x_ops rtl8372n_ops = {
	.detect		= rtl8372n_detect,

	.phy_read_c45   = rtl837x_phy_read_c45,
	.phy_write_c45  = rtl837x_phy_write_c45,
};

const struct rtl837x_variant rtl8372n_variant = {
	.ds_ops_mdio = &rtl8372n_switch_ops_mdio,
	.ops = &rtl8372n_ops,
	.pl_mac_ops = &rtl8372n_phylink_mac_ops,
	.chip_data_sz = sizeof(struct rtl8372n),
};
