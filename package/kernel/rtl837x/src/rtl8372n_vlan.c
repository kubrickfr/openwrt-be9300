// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8372N VLAN offload: the VLAN table shadow, per-port VLAN awareness and
 * the bridge PVID.
 *
 * As in Linux rtl8365mb, the VLAN function stays enabled and VLAN awareness
 * is a per-port state. A VLAN-unaware port (standalone, or in a VLAN-unaware
 * bridge) has no ingress membership filter, accepts every frame type, is
 * classified into VLAN 1 (PVID 1), its frames are exempt from the egress
 * membership check on every available port (egress transparency), and frames
 * leave it with their tag format, VID and priority untouched (REAL_KEEP).
 * The switch then hands every frame to the CPU exactly as received, so 8021q
 * uppers and software VLAN filtering work; isolation, not VLAN membership,
 * keeps ports apart. A port in a VLAN-filtering bridge is filtered on both
 * sides, takes the bridge PVID (or admits tagged frames only, with PVID 0, if
 * the bridge gives it none) and egresses frames tagged or untagged as the
 * VLAN entries say (ORIGINAL). The CPU port is always VLAN-unaware.
 *
 * Entries are written whole from a shadow of what DSA asked for. VLAN 1 is
 * also the VLAN of every VLAN-unaware port, so its hardware entry is the
 * bridge's VLAN 1 plus the CPU port and every VLAN-unaware port, and it is
 * never deleted. VID 0 is written empty at setup: it is where a VLAN-aware
 * port without a PVID classifies any frame its admission lets through.
 *
 * The callbacks follow rtl8365mb_port_vlan_filtering/add/del in Linux
 * drivers/net/dsa/realtek/rtl8365mb_main.c (GPL-2.0). Bridge PVID only while
 * filtering, PVID 1 otherwise and never 0 on a VLAN-unaware port is the rule
 * of the SDK-based driver's rtl837x_commit_pvid_for_mode(), which a PVID-0
 * dead LAN proved on this hardware. Entries go through the verified VLAN
 * write of rtl837x_table.c, adapted from Maksims Neroba's
 * MNeroba/openwrt-flint3#1. PROVENANCE.md records each register's source.
 */
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/ktime.h>
#include <linux/lockdep.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <net/switchdev.h>

#include "rtl837x.h"

/* VID 0 holds no member: frames classified there are dropped. */
#define RTL8372N_NO_PVID_VID	0
/* The first VID setup does not write itself after the table reset. */
#define RTL8372N_FIRST_FREE_VID	2
/* How long TABLE_RST may take to clear itself; the SDK never waits. */
#define RTL8372N_VLAN_RESET_POLL_US	10
#define RTL8372N_VLAN_RESET_TIMEOUT_US	10000

struct rtl8372n_vlan_entry {
	u16 member;
	u16 untag;
};

/* Protected by rtl837x_priv.vlan_lock. */
struct rtl8372n_vlan {
	/* What DSA asked for, by VID; untag is a subset of member. */
	struct rtl8372n_vlan_entry entry[VLAN_N_VID];
	/* Bridge PVID of each user port, 0 for none. */
	u16 pvid[RTL8372N_NUM_PORTS];
	/* User ports that are VLAN-aware in hardware. */
	u16 aware;
	/*
	 * Setup read VLAN 1 back otherwise than written and turned bridge
	 * offload off instead of failing (rtl8372n_vlan_setup()).
	 */
	bool vid1_unverified;
};

/*
 * Write the entry of @vid from the shadow. The CPU port is never untagged
 * from the shadow: frames leave it as received (REAL_KEEP). VLAN 1 also holds
 * the CPU port and every VLAN-unaware port, untagged as at setup. @mismatch
 * is as for rtl837x_vlan_entry_write().
 */
static int __rtl8372n_vlan_commit(struct dsa_switch *ds, u16 vid,
				  bool *mismatch)
{
	struct rtl837x_priv *priv = ds->priv;
	struct rtl8372n_vlan *vlan = priv->vlan;
	u16 cpu = dsa_cpu_ports(ds);
	u16 member, untag, forced;

	lockdep_assert_held(&priv->vlan_lock);

	member = vlan->entry[vid].member;
	untag = vlan->entry[vid].untag & member & ~cpu;
	if (vid == RTL8372N_UNAWARE_VID) {
		forced = cpu | (dsa_user_ports(ds) & ~vlan->aware);
		member |= forced;
		untag |= forced;
	}
	return rtl837x_vlan_entry_write(priv, vid, member, untag, mismatch);
}

static int rtl8372n_vlan_commit(struct dsa_switch *ds, u16 vid)
{
	return __rtl8372n_vlan_commit(ds, vid, NULL);
}

/*
 * The steps of a port's VLAN mode, in the order that makes it VLAN-aware:
 * its ingress side tightens first (membership filter, admission, PVID, then
 * the forced VLAN 1 membership goes) and its frames lose their exemption from
 * egress membership checks, and the untag masks start to apply, last. The
 * exact reverse makes it VLAN-unaware again. No intermediate state is more
 * permissive than the VLAN-unaware one. Every step writes its absolute
 * target, so the sequence is idempotent and serves as its own rollback.
 */
static int rtl8372n_vlan_step_filter(struct dsa_switch *ds, int port,
				     bool aware)
{
	return rtl837x_reg_bits_write(ds->priv, RTL837X_VLAN_INGRESS_FILTER,
				      BIT(port), aware);
}

/* With no bridge PVID a VLAN-aware port admits tagged frames only. */
static int rtl8372n_vlan_step_admission(struct dsa_switch *ds, int port,
					bool aware)
{
	struct rtl837x_priv *priv = ds->priv;
	u32 type = RTL837X_VLAN_ACCEPT_FRAME_TYPE_ALL;

	if (aware && !priv->vlan->pvid[port])
		type = RTL837X_VLAN_ACCEPT_FRAME_TYPE_TAG_ONLY;
	return rtl837x_reg_bits_write(priv, RTL837X_VLAN_INGRESS_CTRL,
				      RTL837X_VLAN_ACCEPT_FRAME_TYPE_MASK(port),
				      type);
}

/*
 * A VLAN-unaware port always gets PVID 1: PVID 0 there was a dead LAN on
 * this hardware. A VLAN-aware port gets the bridge PVID, or VID 0 without
 * one, so that a priority-tagged frame admitted despite TAG_ONLY lands in the
 * empty VID 0 and is dropped, as the bridge would drop it, instead of being
 * forwarded in VLAN 1.
 */
static int rtl8372n_vlan_step_pvid(struct dsa_switch *ds, int port,
				   bool aware)
{
	struct rtl837x_priv *priv = ds->priv;
	u16 pvid = RTL8372N_UNAWARE_VID;

	if (aware)
		pvid = priv->vlan->pvid[port] ?: RTL8372N_NO_PVID_VID;
	return rtl837x_reg_bits_write(priv, RTL837X_PORT_PVID_REG(port),
				      RTL837X_PORT_PVID_MASK(port), pvid);
}

/*
 * A VLAN-aware port stays in VLAN 1 only if the bridge puts it there. Once
 * setup has found VLAN 1 stored otherwise than written, that mismatch does
 * not fail the VLAN-unaware sequence of setup: making a port VLAN-unaware
 * there leaves the word unchanged, and with offload off nothing makes a port
 * VLAN-aware, which would still fail.
 */
static int rtl8372n_vlan_step_vid1(struct dsa_switch *ds, int port, bool aware)
{
	struct rtl837x_priv *priv = ds->priv;
	struct rtl8372n_vlan *vlan = priv->vlan;
	bool mismatch;
	int ret;

	if (aware)
		vlan->aware |= BIT(port);
	else
		vlan->aware &= ~BIT(port);
	ret = __rtl8372n_vlan_commit(ds, RTL8372N_UNAWARE_VID, &mismatch);
	if (ret && mismatch && vlan->vid1_unverified && !aware)
		return 0;
	return ret;
}

/*
 * Frames from a VLAN-unaware port skip the egress membership check on every
 * available port, its own and the CPU's included. Only @port's ingress bit
 * changes, so the CPU's frames stay exempt everywhere. Every egress port is
 * written even after an error; the first one is returned.
 */
static int rtl8372n_vlan_step_transparency(struct dsa_switch *ds, int port,
					   bool aware)
{
	struct rtl837x_priv *priv = ds->priv;
	struct dsa_port *dp;
	int ret = 0, err;

	dsa_switch_for_each_available_port(dp, ds) {
		u32 bit = RTL837X_VLAN_EGRESS_TRANS_BIT(dp->index, port);

		err = regmap_update_bits(priv->map,
					 RTL837X_VLAN_EGRESS_TRANS_REG(dp->index),
					 bit, aware ? 0 : bit);
		ret = ret ?: err;
	}
	return ret;
}

static int rtl8372n_vlan_step_egress(struct dsa_switch *ds, int port,
				     bool aware)
{
	return rtl837x_reg_bits_write(ds->priv, RTL837X_VLAN_EGRESS_TAG,
				      RTL837X_VLAN_EGRESS_TAG_MODE_MASK(port),
				      aware ? RTL837X_VLAN_EGRESS_MODE_ORIGINAL :
					      RTL837X_VLAN_EGRESS_MODE_REAL_KEEP);
}

static int (* const rtl8372n_vlan_steps[])(struct dsa_switch *ds, int port,
					     bool aware) = {
	rtl8372n_vlan_step_filter,
	rtl8372n_vlan_step_admission,
	rtl8372n_vlan_step_pvid,
	rtl8372n_vlan_step_vid1,
	rtl8372n_vlan_step_transparency,
	rtl8372n_vlan_step_egress,
};

/*
 * Make @port VLAN-aware or VLAN-unaware. A normal call stops at the first
 * error; a rollback (@best_effort) runs every step and returns the first
 * error.
 */
static int rtl8372n_vlan_port_apply(struct dsa_switch *ds, int port,
				    bool aware, bool best_effort)
{
	unsigned int n = ARRAY_SIZE(rtl8372n_vlan_steps), i;
	struct rtl837x_priv *priv = ds->priv;
	int ret = 0, err;

	lockdep_assert_held(&priv->vlan_lock);

	for (i = 0; i < n; i++) {
		err = rtl8372n_vlan_steps[aware ? i : n - 1 - i](ds, port,
								 aware);
		if (!err)
			continue;
		ret = ret ?: err;
		if (!best_effort)
			break;
	}
	return ret;
}

/*
 * Put @port into the VLAN-aware or VLAN-unaware mode described at the top of
 * this file, stopping at the first error, and record the mode it is in.
 * Setup uses it for every available port, the CPU port included, with
 * @filtering false, while every user port still reaches the CPU alone; later
 * mode changes must unlink the port first (rtl8372n_port_vlan_filtering()).
 */
int rtl8372n_port_set_vlan_filtering(struct dsa_switch *ds, int port,
				     bool filtering)
{
	struct rtl837x_priv *priv = ds->priv;
	int ret;

	mutex_lock(&priv->vlan_lock);
	ret = rtl8372n_vlan_port_apply(ds, port, filtering, false);
	priv->vlan_mode[port] = ret ? RTL8372N_VLAN_MODE_UNKNOWN :
				      rtl8372n_vlan_mode(filtering);
	mutex_unlock(&priv->vlan_lock);
	return ret;
}

/*
 * Give unlinked @port the mode @filtering unless it is recorded in it
 * already. After a failed mode change a port's hardware mode can be
 * unknown, or not the one it must be in, and DSA calls port_vlan_filtering
 * only when its belief changes. Join gives a port DSA's mode
 * (dp->vlan_filtering), which DSA's attribute sync compares against, so
 * that it does not join in the wrong mode and stay linked with nobody.
 * Leave makes it VLAN-unaware (rtl8372n_port_vlan_mode_standalone()).
 * Every step is tried; on failure the mode is recorded unknown. A change
 * that is not @routine repairs a failure and is logged as a warning.
 */
static int __rtl8372n_port_vlan_mode_repair(struct dsa_switch *ds, int port,
					    bool filtering, bool routine)
{
	struct rtl837x_priv *priv = ds->priv;
	int ret;

	lockdep_assert_held(&priv->vlan_lock);

	if (priv->vlan_mode[port] == rtl8372n_vlan_mode(filtering))
		return 0;
	ret = rtl8372n_vlan_port_apply(ds, port, filtering, true);
	if (ret) {
		priv->vlan_mode[port] = RTL8372N_VLAN_MODE_UNKNOWN;
		dev_err(priv->dev,
			"port %d: VLAN filtering %d not re-applied, mode unknown: %d\n",
			port, filtering, ret);
		return ret;
	}
	priv->vlan_mode[port] = rtl8372n_vlan_mode(filtering);
	if (routine)
		dev_dbg(priv->dev, "port %d: VLAN filtering %d, standalone\n",
			port, filtering);
	else
		dev_warn(priv->dev, "port %d: VLAN filtering %d re-applied\n",
			 port, filtering);
	return 0;
}

int rtl8372n_port_vlan_mode_repair(struct dsa_switch *ds, int port,
				   bool filtering)
{
	return __rtl8372n_port_vlan_mode_repair(ds, port, filtering, false);
}

/*
 * Bridge leave: make unlinked @port VLAN-unaware, the mode of every
 * standalone port (needs_standalone_vlan_filtering is false), whatever DSA
 * believes: DSA's belief can still say VLAN-aware there, and a VLAN-aware
 * port with no VLAN receives nothing. On a normal leave of a VLAN-aware
 * bridge the port is VLAN-aware here, as DSA believes, and DSA's reset call
 * (port_vlan_filtering(false)) follows: making it VLAN-unaware is then
 * routine and logged at debug level only. DSA sends a normal leave after it
 * has dropped dp->bridge; its join rollback still has dp->bridge set. A
 * change there, or from a mode that is unknown or not DSA's, follows a
 * failure and keeps the "re-applied" warning.
 */
int rtl8372n_port_vlan_mode_standalone(struct dsa_switch *ds, int port)
{
	struct dsa_port *dp = dsa_to_port(ds, port);
	struct rtl837x_priv *priv = ds->priv;
	bool routine;

	lockdep_assert_held(&priv->vlan_lock);

	routine = !dsa_port_bridge_dev_get(dp) &&
		  priv->vlan_mode[port] ==
		  rtl8372n_vlan_mode(dsa_port_is_vlan_filtering(dp));
	return __rtl8372n_port_vlan_mode_repair(ds, port, false, routine);
}

/*
 * Apply a changed bridge PVID to a VLAN-aware port: a new PVID before
 * untagged frames are admitted, TAG_ONLY before PVID 0. Removing the PVID
 * tries both writes, since either one alone keeps untagged frames out of
 * the old PVID: TAG_ONLY drops them, PVID 0 sends them to the empty VID 0.
 * A VLAN-unaware port keeps PVID 1; the stored PVID applies once it becomes
 * VLAN-aware.
 */
static int rtl8372n_vlan_port_pvid_commit(struct dsa_switch *ds, int port)
{
	struct rtl837x_priv *priv = ds->priv;
	int ret, err;

	if (!(priv->vlan->aware & BIT(port)))
		return 0;
	if (priv->vlan->pvid[port]) {
		ret = rtl8372n_vlan_step_pvid(ds, port, true);
		return ret ?: rtl8372n_vlan_step_admission(ds, port, true);
	}
	ret = rtl8372n_vlan_step_admission(ds, port, true);
	err = rtl8372n_vlan_step_pvid(ds, port, true);
	return ret ?: err;
}

/*
 * The fallback of port_vlan_filtering once (2) or (4) failed with @ret and
 * @port is unlinked again. A port of the offloaded bridge gets back the
 * other mode, which DSA keeps believing in, and is linked again with the
 * members in that mode, as a join would link it: DSA keeps it offloaded
 * after the error, so its frames carry offload_fwd_mark and the bridge does
 * not relay them to those members either. If that link fails it is
 * unlinked (held if that fails too). A standalone port only ever goes back
 * to VLAN-unaware, the standalone mode: a VLAN-aware port with no VLAN
 * receives nothing, and nothing would repair it before its next join.
 * Every step is tried, and the mode is recorded unknown if one fails. A
 * port that ends in its target state is released if it was held; one that
 * does not keeps a hold it had. Otherwise a bridge member left unlinked, or
 * a port in an unknown mode, keeps the state DSA set and reaches the CPU
 * alone until its next STP state change (an administrative down/up, or a
 * link flap while bridged) puts it right (rtl8372n_port_isolation_held()).
 * Returns @ret, or 0 if this retry put a standalone port in the mode asked
 * for after all.
 */
static int rtl8372n_port_vlan_fallback(struct dsa_switch *ds, int port,
				       bool vlan_filtering, int ret)
{
	struct rtl837x_priv *priv = ds->priv;
	bool bridged = dsa_port_bridge_dev_get(dsa_to_port(ds, port));
	bool target = bridged && !vlan_filtering;
	u16 peers;
	int err;

	err = rtl8372n_vlan_port_apply(ds, port, target, true);
	if (err) {
		priv->vlan_mode[port] = RTL8372N_VLAN_MODE_UNKNOWN;
		dev_err(priv->dev,
			"port %d: VLAN filtering %d not restored, mode unknown: %d\n",
			port, target, err);
	} else {
		priv->vlan_mode[port] = rtl8372n_vlan_mode(target);
		if (!bridged) {
			rtl8372n_port_release(ds, port, true);
			if (target != vlan_filtering)
				return ret;
			dev_warn(priv->dev,
				 "port %d: VLAN filtering %d applied on retry after error %d\n",
				 port, target, ret);
			return 0;
		}
		err = rtl8372n_port_link(ds, port, &peers);
		if (!err) {
			rtl8372n_port_release(ds, port, true);
			dev_warn(priv->dev,
				 "port %d: back to VLAN filtering %d, linked %#x\n",
				 port, target, peers);
			return ret;
		}
		rtl8372n_port_unlink(ds, port);
	}
	if (bridged)
		dev_err(priv->dev,
			"port %d: left unlinked after a failed VLAN mode change\n",
			port);
	return ret;
}

/*
 * DSA calls this for user ports, on real transitions only: when a port
 * joins or leaves a VLAN-aware bridge and when the bridge toggles
 * vlan_filtering. It runs before dp->vlan_filtering changes, and again with
 * the old value if DSA fails afterwards, so the driver's own state decides.
 * The port changes mode unlinked (the isolation invariant in rtl8372n.c):
 * (1) unlink it, (2) reprogram it, (3) record the new mode, (4) link it with
 * the members now in that mode. If (1) fails nothing else changes. If (2)
 * or (4) fails, the port is unlinked again and given the mode of
 * rtl8372n_port_vlan_fallback(): a bridge member is back in its old mode
 * and linked again, or, if its mode is unknown or the link fails, stays
 * unlinked, cut off from the other members until its next mode change,
 * join, leave or STP state change. It is never linked in a mode it is not
 * in: if it cannot be unlinked again after (4), it keeps the new mode,
 * held, and the retry gives it DSA's mode before linking it. A held port
 * is released only by a change or fallback that ends in its target state.
 * Leaving a VLAN-aware bridge ends here with the port VLAN-unaware again:
 * PVID 1 and back in VLAN 1. After a call on a port of the offloaded
 * bridge the shared VID 0/1 FDB entries are re-synced, with vlan_lock
 * released (see the end of this function).
 */
int rtl8372n_port_vlan_filtering(struct dsa_switch *ds, int port,
				 bool vlan_filtering,
				 struct netlink_ext_ack *extack)
{
	struct rtl837x_priv *priv = ds->priv;
	struct net_device *br;
	u16 pvid, peers = 0;
	int ret;

	if (!dsa_is_user_port(ds, port))
		return 0;

	mutex_lock(&priv->vlan_lock);
	ret = rtl8372n_port_unlink(ds, port);
	if (ret)
		goto out;
	ret = rtl8372n_vlan_port_apply(ds, port, vlan_filtering, false);
	if (!ret) {
		priv->vlan_mode[port] = rtl8372n_vlan_mode(vlan_filtering);
		ret = rtl8372n_port_link(ds, port, &peers);
		/* Still linked in part, in the new mode: it keeps that mode. */
		if (ret && rtl8372n_port_unlink(ds, port))
			goto out;
	}
	if (ret)
		ret = rtl8372n_port_vlan_fallback(ds, port, vlan_filtering, ret);
	else
		rtl8372n_port_release(ds, port, true);
out:
	pvid = priv->vlan->pvid[port];
	mutex_unlock(&priv->vlan_lock);

	/*
	 * The FDB entries that DSA VIDs 0 and 1 share follow the VID the
	 * offloaded bridge looks up, not this call's argument: DSA's restore
	 * call passes the port's old value, which a joining port's bridge
	 * does not take. During a toggle the bridge has the new value already
	 * (br_vlan_filter_toggle() sets it before it asks the ports), so a
	 * success re-syncs at once; but the bridge puts the old value back if
	 * a later port fails, which no call here sees, so every call, failed
	 * or not, also queues a re-sync for when the change is over
	 * (rtl8372n_fdb_resync_work()). A port that is leaving has no bridge
	 * any more.
	 */
	br = dsa_port_bridge_dev_get(dsa_to_port(ds, port));
	if (br)
		rtl8372n_fdb_resync_deferred(ds);
	if (ret) {
		NL_SET_ERR_MSG_MOD(extack, "Failed to change VLAN filtering");
		dev_err(priv->dev, "port %d: VLAN filtering %d failed: %d\n",
			port, vlan_filtering, ret);
		return ret;
	}
	dev_dbg(priv->dev,
		"port %d: VLAN filtering %d, bridge PVID %u, linked %#x\n",
		port, vlan_filtering, pvid, peers);
	if (br)
		rtl8372n_fdb_resync(ds, br_vlan_enabled(br));
	return 0;
}

/*
 * 8021q pushes VID 0 to the user and CPU ports; it needs no entry, so it is
 * accepted and ignored. Neither the bridge nor 8021q passes VID 4095.
 */
static int rtl8372n_vlan_check(struct dsa_switch *ds, int port, u16 vid,
			       struct netlink_ext_ack *extack)
{
	if (!dsa_is_user_port(ds, port) && !dsa_is_cpu_port(ds, port))
		return -EOPNOTSUPP;
	if (vid >= VLAN_VID_MASK) {
		NL_SET_ERR_MSG_MOD(extack, "VLAN 4095 is reserved");
		return -EINVAL;
	}
	return 0;
}

/*
 * Membership goes in before the PVID. On the CPU port DSA adds the bridge's
 * own VLANs, those of foreign bridge ports and 8021q uppers of VLAN-aware
 * ports, refcounted and with the PVID flag cleared; only the membership
 * counts, since frames leave the CPU port as received and Linux applies the
 * PVID itself. A user port's add also carries flag changes of an existing
 * VLAN: an add without the PVID flag for the current PVID removes it. A
 * failed add leaves the shadow and, as far as the hardware allows, the
 * switch as they were.
 */
int rtl8372n_port_vlan_add(struct dsa_switch *ds, int port,
			   const struct switchdev_obj_port_vlan *vlan,
			   struct netlink_ext_ack *extack)
{
	bool untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;
	bool pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;
	struct rtl837x_priv *priv = ds->priv;
	struct rtl8372n_vlan *state = priv->vlan;
	struct rtl8372n_vlan_entry old;
	u16 vid = vlan->vid, old_pvid, new_pvid;
	int ret, err;

	ret = rtl8372n_vlan_check(ds, port, vid, extack);
	if (ret || !vid)
		return ret;

	mutex_lock(&priv->vlan_lock);
	old = state->entry[vid];
	old_pvid = state->pvid[port];
	new_pvid = old_pvid;

	state->entry[vid].member |= BIT(port);
	if (dsa_is_user_port(ds, port)) {
		if (untagged)
			state->entry[vid].untag |= BIT(port);
		else
			state->entry[vid].untag &= ~BIT(port);
		if (pvid)
			new_pvid = vid;
		else if (old_pvid == vid)
			new_pvid = 0;
	}
	ret = rtl8372n_vlan_commit(ds, vid);
	if (ret)
		goto undo_entry;

	if (new_pvid != old_pvid) {
		state->pvid[port] = new_pvid;
		ret = rtl8372n_vlan_port_pvid_commit(ds, port);
		if (ret) {
			state->pvid[port] = old_pvid;
			err = rtl8372n_vlan_port_pvid_commit(ds, port);
			if (err)
				dev_err(priv->dev,
					"port %d: PVID %u not restored: %d\n",
					port, old_pvid, err);
			goto undo_entry;
		}
	}
	mutex_unlock(&priv->vlan_lock);

	dev_dbg(priv->dev, "port %d: VLAN %u added (flags %#x), bridge PVID %u\n",
		port, vid, vlan->flags, new_pvid);
	return 0;

undo_entry:
	state->entry[vid] = old;
	err = rtl8372n_vlan_commit(ds, vid);
	if (err)
		dev_err(priv->dev, "VLAN %u not restored: %d\n", vid, err);
	mutex_unlock(&priv->vlan_lock);

	NL_SET_ERR_MSG_MOD(extack, "Failed to program VLAN");
	dev_err(priv->dev, "port %d: VLAN %u add failed: %d\n", port, vid, ret);
	return ret;
}

/*
 * Take @port out of VLAN @vid. The shadow keeps the port unless the entry
 * write succeeds; on failure the old entry is written back, best effort,
 * since a write that reported an error may still have landed.
 */
static int rtl8372n_vlan_member_del(struct dsa_switch *ds, int port, u16 vid)
{
	struct rtl837x_priv *priv = ds->priv;
	struct rtl8372n_vlan *state = priv->vlan;
	struct rtl8372n_vlan_entry old = state->entry[vid];
	int ret, err;

	state->entry[vid].member &= ~BIT(port);
	state->entry[vid].untag &= ~BIT(port);
	ret = rtl8372n_vlan_commit(ds, vid);
	if (!ret)
		return 0;
	state->entry[vid] = old;
	err = rtl8372n_vlan_commit(ds, vid);
	if (err)
		dev_err(priv->dev, "VLAN %u not restored: %d\n", vid, err);
	return ret;
}

/*
 * The PVID goes before the membership. The bridge drops a VLAN's PVID role
 * before it asks for the delete and keeps it dropped whatever the result
 * (__vlan_del() in br_vlan.c), so the PVID is cleared in the shadow even if
 * writing it fails, and is not written back: the hardware never admits
 * untagged frames into a VLAN the bridge no longer gives as PVID. A failed
 * PVID write stops the delete there. The membership leaves the shadow only
 * once the entry is written (rtl8372n_vlan_member_del()). On an error DSA
 * keeps the VLAN (on the CPU port it resets the refcount to 1, so a host
 * VLAN stays deliverable), and so does the bridge for an interactive
 * delete. On the flush and unoffload paths the bridge forgets the VLAN
 * anyway: the port then stays a member until it next joins a bridge
 * (rtl8372n_port_vlan_drop_stale()). VLAN 1 keeps its forced members.
 */
int rtl8372n_port_vlan_del(struct dsa_switch *ds, int port,
			   const struct switchdev_obj_port_vlan *vlan)
{
	struct rtl837x_priv *priv = ds->priv;
	struct rtl8372n_vlan *state = priv->vlan;
	u16 vid = vlan->vid, pvid;
	int ret;

	ret = rtl8372n_vlan_check(ds, port, vid, NULL);
	if (ret || !vid)
		return ret;

	mutex_lock(&priv->vlan_lock);
	/* The delete's own result, not the check's 0 carried over. */
	ret = 0;
	if (dsa_is_user_port(ds, port) && state->pvid[port] == vid) {
		state->pvid[port] = 0;
		ret = rtl8372n_vlan_port_pvid_commit(ds, port);
	}
	if (!ret)
		ret = rtl8372n_vlan_member_del(ds, port, vid);
	pvid = state->pvid[port];
	mutex_unlock(&priv->vlan_lock);

	if (ret)
		dev_err(priv->dev, "port %d: VLAN %u delete failed, membership kept: %d\n",
			port, vid, ret);
	else
		dev_dbg(priv->dev, "port %d: VLAN %u deleted, bridge PVID %u\n",
			port, vid, pvid);
	return ret;
}

/*
 * A port enters a bridge with no VLAN of its own: DSA replays the bridge's
 * VLANs only after port_bridge_join, and a port with an 8021q upper is not
 * offloaded. Any membership or PVID the shadow still holds for it is left
 * from a delete that failed while the bridge forgot the VLAN. Join calls
 * this before linking the port, so that it never carries a VLAN its new
 * bridge does not give it.
 */
int rtl8372n_port_vlan_drop_stale(struct dsa_switch *ds, int port)
{
	struct rtl837x_priv *priv = ds->priv;
	struct rtl8372n_vlan *state = priv->vlan;
	int ret;
	u16 vid;

	lockdep_assert_held(&priv->vlan_lock);

	if (state->pvid[port]) {
		dev_warn(priv->dev, "port %d: stale bridge PVID %u dropped\n",
			 port, state->pvid[port]);
		state->pvid[port] = 0;
		ret = rtl8372n_vlan_port_pvid_commit(ds, port);
		if (ret)
			return ret;
	}
	for (vid = 1; vid < VLAN_VID_MASK; vid++) {
		if (!(state->entry[vid].member & BIT(port)))
			continue;
		dev_warn(priv->dev, "port %d: stale VLAN %u membership dropped\n",
			 port, vid);
		ret = rtl8372n_vlan_member_del(ds, port, vid);
		if (ret)
			return ret;
	}
	return 0;
}

static void rtl8372n_vlan_free(void *data)
{
	kvfree(data);
}

/*
 * Write VLAN_CTRL fields. TABLE_RST is always written 0, so that no
 * read-modify-write of the register starts another table reset if the bit
 * still reads back set.
 */
int rtl8372n_vlan_ctrl_update(struct rtl837x_priv *priv, u32 mask, u32 val)
{
	return regmap_update_bits(priv->map, RTL837X_VLAN_CTRL,
				  mask | RTL837X_VLAN_CTRL_TABLE_RST,
				  val & ~RTL837X_VLAN_CTRL_TABLE_RST);
}

static const u16 rtl8372n_vlan_samples[] = RTL8372N_VLAN_SAMPLE_VIDS;
/* The log lines print three samples. */
static_assert(ARRAY_SIZE(rtl8372n_vlan_samples) == 3);
/* The sample VID that holds a sentinel entry until the VLAN table reset. */
#define RTL8372N_VLAN_SENTINEL_VID	4094

/*
 * Read the sample VIDs into @words. Returns 1 if one of them has a member,
 * untag or FID/MSTI bit set (bits 23:0), 0 if none has, or an error. The
 * whole words are logged, but SVLAN check and IVL (bits 25:24) and the
 * reserved bits do not count: on an entry with no member they admit
 * nothing, and a reset that leaves them set must not cost a sweep of the
 * whole table at every boot.
 */
static int rtl8372n_vlan_sample(struct rtl837x_priv *priv, u32 *words)
{
	struct rtl837x_vlan_entry entry;
	unsigned int i;
	int ret, set = 0;

	for (i = 0; i < ARRAY_SIZE(rtl8372n_vlan_samples); i++) {
		ret = rtl837x_vlan_entry_read(priv, rtl8372n_vlan_samples[i],
					      &entry);
		if (ret)
			return ret;
		words[i] = entry.raw;
		if (entry.raw & RTL837X_VLAN_ENTRY_USED)
			set = 1;
	}
	return set;
}

/*
 * Empty the VLAN table, which the chip reset may leave populated (a warm
 * reboot from an image that used VLANs, a module reload): a leftover entry
 * would admit its VID on VLAN-aware ports behind the shadow's back, and its
 * FID/MSTI field would put the VID under an MSTI state register that nothing
 * writes. A sentinel entry, the CPU port alone, is first written to one of
 * the sample VIDs, which setup leaves empty otherwise: the samples left as
 * the chip reset had them say nothing when that was empty, and a TABLE_RST
 * that does nothing would go unseen. TABLE_RST is then set as the SDK's
 * dal_rtl8373_vlan_reset() does, by a write issued even if the bit already
 * reads 1 (regmap_set_bits() would skip it then, and no reset would run),
 * given up to 10 ms to clear itself, and cleared if it does not. The sample
 * VIDs must then hold no member, untag or FID/MSTI bit; if one does, the
 * sentinel included, every VID from 2 up is written empty instead (setup
 * writes VIDs 0 and 1 next) and the samples are read again. A sentinel that
 * cannot be written, or a TABLE_RST write that failed or that cannot be
 * cleared, is an error: the samples do not stand for the table then. This
 * sequence has not run after the chip reset; the caller only turns offload
 * off when it fails.
 */
static int rtl8372n_vlan_table_reset(struct rtl837x_priv *priv)
{
	const u16 *vid = rtl8372n_vlan_samples;
	u32 before[ARRAY_SIZE(rtl8372n_vlan_samples)];
	u32 after[ARRAY_SIZE(rtl8372n_vlan_samples)];
	int ret, set, err;
	ktime_t start;
	u32 ctrl = 0;
	s64 elapsed;

	/* Whether the chip reset alone empties the table: evidence only. */
	ret = rtl8372n_vlan_sample(priv, before);
	if (ret < 0)
		dev_warn(priv->dev, "VLAN table before reset not read: %d\n",
			 ret);
	else
		dev_info(priv->dev,
			 "VLAN table before reset: VID %u/%u/%u %#x/%#x/%#x\n",
			 vid[0], vid[1], vid[2], before[0], before[1],
			 before[2]);

	/*
	 * The sentinel VLAN delivers to the CPU port only. It lasts until the
	 * reset, or the sweep below if the reset leaves it; a sweep that fails
	 * leaves it with offload off, where every port is VLAN-unaware.
	 */
	ret = rtl837x_vlan_entry_write(priv, RTL8372N_VLAN_SENTINEL_VID,
				       dsa_cpu_ports(priv->ds), 0, NULL);
	if (ret) {
		dev_warn(priv->dev,
			 "VLAN table reset: sentinel VID %u not written: %d\n",
			 RTL8372N_VLAN_SENTINEL_VID, ret);
		return ret;
	}

	start = ktime_get();
	set = regmap_write_bits(priv->map, RTL837X_VLAN_CTRL,
				RTL837X_VLAN_CTRL_TABLE_RST,
				RTL837X_VLAN_CTRL_TABLE_RST);
	ret = set ?: regmap_read_poll_timeout(priv->map, RTL837X_VLAN_CTRL,
					      ctrl,
					      !(ctrl & RTL837X_VLAN_CTRL_TABLE_RST),
					      RTL8372N_VLAN_RESET_POLL_US,
					      RTL8372N_VLAN_RESET_TIMEOUT_US);
	elapsed = ktime_us_delta(ktime_get(), start);
	if (ret) {
		/* Still set, unknown, or written in spite of an error. */
		err = regmap_clear_bits(priv->map, RTL837X_VLAN_CTRL,
					RTL837X_VLAN_CTRL_TABLE_RST);
		if (!err)
			err = rtl837x_reg_read(priv, RTL837X_VLAN_CTRL, &ctrl);
		if (!err && (ctrl & RTL837X_VLAN_CTRL_TABLE_RST))
			err = -EBUSY;
		dev_warn(priv->dev,
			 "VLAN table reset: TABLE_RST %s after %lld us: %d; cleared: %d\n",
			 set ? "write failed" : "still set", elapsed, ret, err);
		if (err || set)
			return err ?: set;
	}

	ret = rtl8372n_vlan_sample(priv, after);
	if (ret < 0)
		return ret;
	dev_info(priv->dev,
		 "VLAN table reset in %lld us: VID %u/%u/%u %#x/%#x/%#x\n",
		 elapsed, vid[0], vid[1], vid[2], after[0], after[1], after[2]);
	if (!ret)
		return 0;

	start = ktime_get();
	ret = rtl837x_vlan_table_zero(priv, RTL8372N_FIRST_FREE_VID,
				      VLAN_N_VID - 1);
	if (!ret) {
		ret = rtl8372n_vlan_sample(priv, after);
		if (ret > 0)
			ret = -EIO;
	}
	dev_warn(priv->dev,
		 "VLAN table reset left entries set: writing VIDs %u-%u empty took %lld ms: %d (VID %u/%u/%u %#x/%#x/%#x)\n",
		 RTL8372N_FIRST_FREE_VID, VLAN_N_VID - 1,
		 ktime_ms_delta(ktime_get(), start), ret, vid[0], vid[1],
		 vid[2], after[0], after[1], after[2]);
	return ret;
}

/*
 * Pin the VLAN controls whose reset values classification and learning
 * would otherwise depend on; P0 wrote none of them:
 * - VID0_TYPE 0: a priority-tagged frame is classified to the PVID like an
 *   untagged one, as 802.1Q and the bridge want (a VLAN-aware port without
 *   a PVID still drops it in the empty VID 0); VID4095_TYPE 1: a frame
 *   tagged VID 4095 keeps that VID, whose entry stays empty, so a
 *   VLAN-aware port never admits it into its PVID;
 * - both per-VID learning-disable slots empty, as the SDK's vlan_init
 *   leaves them, so that learning never stops silently in one VLAN;
 * - no port-based FID on the available ports: it would key their frames by
 *   FID, as the public ZTE driver does for VLAN-unaware ports, instead of
 *   the {MAC, VID} (IVL) keys of the FDB code.
 */
static int rtl8372n_vlan_pin_controls(struct dsa_switch *ds)
{
	struct rtl837x_priv *priv = ds->priv;
	struct dsa_port *dp;
	u32 ports = 0;
	int i, ret;

	ret = rtl8372n_vlan_ctrl_update(priv, RTL837X_VLAN_CTRL_VID0_TYPE |
					RTL837X_VLAN_CTRL_VID4095_TYPE,
					RTL837X_VLAN_CTRL_VID4095_TYPE);
	if (ret)
		return ret;
	for (i = 0; i < RTL837X_VLAN_L2_LRN_DIS_SLOTS; i++) {
		ret = rtl837x_reg_write(priv, RTL837X_VLAN_L2_LRN_DIS(i), 0);
		if (ret)
			return ret;
	}
	dsa_switch_for_each_available_port(dp, ds)
		ports |= BIT(dp->index);
	return regmap_clear_bits(priv->map, RTL837X_PORT_BASED_FID_EN, ports);
}

/*
 * Start from an empty table and shadow: empty the VLAN table, pin the VLAN
 * controls, write VID 0 empty (the no-PVID sink), VLAN 1 with the CPU port and
 * every user port, and PVID 1 on every available port. Setup then makes each
 * port VLAN-unaware. The shadow is allocated once per bound device and
 * cleared on every setup, the mode record (unknown until setup makes a port
 * VLAN-unaware) with it.
 *
 * Only bridge offload relies on the table reset, on the pinned controls
 * (VLAN-aware classification, and learning, which only bridged ports do) and
 * on VID 0, none of which P0 wrote: if one fails, offload is turned off and
 * setup goes on, standalone ports working as in P0 with those controls at
 * their reset values (rtl8372n_disable_offload()). VLAN 1 is P0's entry and
 * stays required: an I/O error or a timeout fails setup, a readback that
 * differs only turns offload off.
 */
int rtl8372n_vlan_setup(struct dsa_switch *ds)
{
	struct rtl837x_priv *priv = ds->priv;
	struct rtl8372n_vlan *vlan = priv->vlan;
	struct dsa_port *dp;
	bool mismatch;
	int port, ret;

	if (!vlan) {
		vlan = kvzalloc(sizeof(*vlan), GFP_KERNEL);
		if (!vlan)
			return -ENOMEM;
		ret = devm_add_action_or_reset(priv->dev, rtl8372n_vlan_free,
					       vlan);
		if (ret)
			return ret;
		priv->vlan = vlan;
	}

	mutex_lock(&priv->vlan_lock);
	memset(vlan, 0, sizeof(*vlan));
	for (port = 0; port < RTL8372N_NUM_PORTS; port++)
		priv->vlan_mode[port] = RTL8372N_VLAN_MODE_UNKNOWN;
	if (rtl8372n_vlan_table_reset(priv))
		rtl8372n_disable_offload(priv, "VLAN table reset failed");
	ret = rtl8372n_vlan_pin_controls(ds);
	if (ret) {
		dev_warn(priv->dev, "VLAN controls not pinned: %d\n", ret);
		rtl8372n_disable_offload(priv, "VLAN controls not pinned");
	}
	if (rtl8372n_vlan_commit(ds, RTL8372N_NO_PVID_VID))
		rtl8372n_disable_offload(priv, "VID 0 entry failed");
	ret = __rtl8372n_vlan_commit(ds, RTL8372N_UNAWARE_VID, &mismatch);
	if (ret && mismatch) {
		vlan->vid1_unverified = true;
		rtl8372n_disable_offload(priv, "VLAN 1 readback mismatch");
		ret = 0;
	}
	if (ret)
		goto out;
	dsa_switch_for_each_available_port(dp, ds) {
		ret = rtl8372n_vlan_step_pvid(ds, dp->index, false);
		if (ret)
			goto out;
	}
out:
	mutex_unlock(&priv->vlan_lock);
	return ret;
}
