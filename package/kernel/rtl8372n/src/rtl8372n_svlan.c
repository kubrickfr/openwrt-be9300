// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * S-R steering, switch side (WP1-DESIGN §4, §5): the kernel glue around the
 * pure sequences of rtl8372n_svlan_seq.c.
 *
 * svlan=1 (latched at probe) binds the rtl8_4s tagger; arming is then an
 * explicit control, debugfs rtl8372n/svlan: "arm <deadline_s>", "renew
 * <deadline_s>", "disarm", "disarm force", and the bench verbs "disarm force
 * nophy" and "isolate cpu <s>". An arm writes 8 driver-owned VLAN entries
 * ({p untagged, CPU} at 3900 + p and 3910 + p, outside the bridge shadow),
 * then, inside an accept-NONE window and with the uplink last, the S-VLAN
 * block, verifies everything and hands the tagger accept S. Every disarm
 * clears the uplink first and alone, removes the entries and fast-ages the
 * user ports. Every arm has a deadline, which "renew" moves later (the
 * daily lease). The switch does not enforce C-VLAN membership on S-R frames
 * (CPU-source transparency is left as it is): that is the WP2 resolver's
 * job.
 *
 * A disarm clears steering only from PPE OFF. Over a PPE claim, REMOVE,
 * SHUTDOWN and "disarm force" take the forced path instead (WP3-DESIGN
 * §2.2): FAULT, the CPU port isolated and the S-R PHYs powered down, each
 * read back, S steering kept; the deadline work then completes the disarm
 * once the PPE is off (the follow-up), and quiesce keeps the S block while
 * forced steering is live. A REMOVE that took the forced path refuses every
 * later bind until the module is reloaded (D16), which the PPE's reference
 * on this module (it holds ds->dev->driver->owner while it may emit S-R
 * frames) refuses until it is off.
 *
 * The tagger shares struct rtl8_4s_tagger_data (<linux/dsa/rtl8_4s.h>,
 * ABI 1): this driver writes accept, s_ports, disarm_at, sw_gen and the sw
 * byte of the state word; the PPE owns the ppe byte. tagger_data is only
 * looked up inside the control window, under the svlan lock: it connects
 * inside dsa_register_switch(), before rtl8372n_svlan_ctl_start(), and
 * disconnects inside dsa_unregister_switch(), after
 * rtl8372n_svlan_ctl_stop(); shutdown never frees it. It is never stored.
 *
 * Lock order: RTNL -> svlan lock -> vlan_lock -> table_lock -> map_lock ->
 * MII bus, and svlan lock -> phy_lock -> map_lock with vlan_lock released
 * (F2). Only the arm takes RTNL (trylock, restarting the write), before the
 * svlan lock, for the topology check; it only trylocks the svlan lock under
 * RTNL, and waits for a busy one killably without RTNL. Nothing takes RTNL
 * under the svlan lock. The deadline and isolate cpu works take the svlan
 * lock only.
 * svlan_counters reads memory only, so it may be read under load; svlan
 * reads the switch and is for idle use.
 */
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/if_vlan.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mdio.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/regmap.h>
#include <linux/rtnetlink.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "rtl837x.h"

/* One locked operation's view: tagger_data is looked up, never kept. */
struct sv_ctx {
	struct rtl837x_priv *priv;
	struct rtl8_4s_tagger_data *td;
};

static int sv_io_rd(void *ctx, u32 reg, u32 *val)
{
	struct sv_ctx *c = ctx;

	return regmap_read(c->priv->map, reg, val);
}

static int sv_io_wr(void *ctx, u32 reg, u32 mask, u32 val)
{
	struct sv_ctx *c = ctx;

	return regmap_update_bits(c->priv->map, reg, mask, val);
}

static int sv_io_entry_wr(void *ctx, u16 vid, u16 member, u16 untag)
{
	struct sv_ctx *c = ctx;

	return rtl837x_vlan_entry_write(c->priv, vid, member, untag, NULL);
}

static int sv_io_entry_rd(void *ctx, u16 vid, u32 *raw)
{
	struct sv_ctx *c = ctx;
	struct rtl837x_vlan_entry entry;
	int ret;

	ret = rtl837x_vlan_entry_read(c->priv, vid, &entry);
	if (!ret)
		*raw = entry.raw;
	return ret;
}

static void sv_io_fast_age(void *ctx, int port)
{
	struct sv_ctx *c = ctx;

	/* Logs its own failure. */
	rtl8372n_port_fast_age(c->priv->ds, port);
}

static void sv_io_sw_set(void *ctx, u8 sw)
{
	struct sv_ctx *c = ctx;

	if (c->td)
		rtl8_4s_sw_set(c->td, sw);
}

static bool sv_io_sw_begin_disarm(void *ctx, u8 *ppe)
{
	struct sv_ctx *c = ctx;

	if (!c->td) {
		*ppe = RTL8_4S_PPE_OFF;
		return true;
	}
	return rtl8_4s_sw_begin_disarm(c->td, ppe);
}

/* s_ports and disarm_at are written before accept, which is released. */
static void sv_io_set_accept(void *ctx, u8 accept, u16 s_ports,
			     unsigned long disarm_at)
{
	struct sv_ctx *c = ctx;

	if (!c->td)
		return;
	WRITE_ONCE(c->td->s_ports, s_ports);
	WRITE_ONCE(c->td->disarm_at, disarm_at);
	/* Pairs with the tagger's smp_load_acquire(): s_ports first. */
	smp_store_release(&c->td->accept, accept);
}

static void sv_io_sync(void *ctx)
{
	synchronize_net();
}

static void sv_io_sleep_ms(void *ctx, unsigned int ms)
{
	msleep(ms);
}

static unsigned long sv_io_jiffies(void *ctx)
{
	return jiffies;
}

static u64 sv_io_ms(void *ctx)
{
	return ktime_to_ms(ktime_get());
}

static u64 sv_io_us(void *ctx)
{
	return ktime_to_us(ktime_get());
}

static void sv_io_timer(void *ctx, bool mod, unsigned long delay)
{
	struct sv_ctx *c = ctx;

	/* Never queue_delayed_work(): a no-op while an old work is pending. */
	if (mod)
		mod_delayed_work(system_percpu_wq, &c->priv->sv.deadline, delay);
	else
		cancel_delayed_work(&c->priv->sv.deadline);
}

static void sv_io_iso_timer(void *ctx, bool mod, unsigned long delay)
{
	struct sv_ctx *c = ctx;

	if (mod)
		mod_delayed_work(system_percpu_wq, &c->priv->sv.iso_bench, delay);
	else
		cancel_delayed_work(&c->priv->sv.iso_bench);
}

/*
 * F1 and isolate cpu (struct sv_io). vlan_lock covers the isolation words;
 * only setup, quiesce and these write the CPU port's (link and unlink write
 * user ports' words). As sv_clear(): a read-back that shows @val succeeds
 * whatever the write returned.
 */
static int sv_io_cpu_word(void *ctx, unsigned int cpu, u32 *was, u32 val)
{
	struct sv_ctx *c = ctx;
	struct rtl837x_priv *priv = c->priv;
	u32 reg = RTL837X_PORT_ISOLATION_REG(cpu), now = 0;
	int ret, wret;

	mutex_lock(&priv->vlan_lock);
	if (was && regmap_read(priv->map, reg, was))
		*was = SV_WORD_UNREAD;
	wret = regmap_write(priv->map, reg, val);
	ret = regmap_read(priv->map, reg, &now);
	mutex_unlock(&priv->vlan_lock);
	if (ret)
		return ret;
	if ((now & RTL837X_PORT_ISOLATION_MASK) ==
	    (val & RTL837X_PORT_ISOLATION_MASK))
		return 0;
	return wret ?: -EIO;
}

/*
 * F2 (struct sv_io): the setup and quiesce broadcast of PMA low power, then
 * each PHY read back. Called with the svlan lock only (phy_lock ->
 * map_lock); phylib may power a PHY up again later (WP3-DESIGN K-P).
 */
static int sv_io_phy_down(void *ctx, u16 mask)
{
	struct sv_ctx *c = ctx;
	unsigned long m = mask;
	unsigned int p;
	int ret, wret;
	u16 v;

	wret = rtl837x_phys_write_c45(c->priv, mask, MDIO_MMD_VEND2,
				      RTL8372N_PHY_OCP_PMA_CTRL1,
				      RTL8372N_PHY_PMA_LOWPOWER);
	for_each_set_bit(p, &m, RTL8372N_NUM_PORTS) {
		v = 0;
		ret = rtl837x_phy_read_c45(c->priv, p, MDIO_MMD_VEND2,
					   RTL8372N_PHY_OCP_PMA_CTRL1, &v);
		if (ret)
			return ret;
		if (!(v & MDIO_CTRL1_LPOWER))
			return wret ?: -EIO;
	}
	return 0;
}

static void sv_io_warn(void *ctx)
{
	WARN_ON_ONCE(1);
}

static __printf(3, 4) void sv_io_log(void *ctx, int level, const char *fmt,
				     ...)
{
	struct sv_ctx *c = ctx;
	struct device *dev = c->priv->dev;
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	switch (level) {
	case SV_LOG_ERR:
		dev_err(dev, "%pV", &vaf);
		break;
	case SV_LOG_WARN:
		dev_warn(dev, "%pV", &vaf);
		break;
	case SV_LOG_INFO:
		dev_info(dev, "%pV", &vaf);
		break;
	default:
		dev_dbg(dev, "%pV", &vaf);
		break;
	}
	va_end(args);
}

static const struct sv_io sv_io_template = {
	.rd = sv_io_rd,
	.wr = sv_io_wr,
	.entry_wr = sv_io_entry_wr,
	.entry_rd = sv_io_entry_rd,
	.fast_age = sv_io_fast_age,
	.sw_set = sv_io_sw_set,
	.sw_begin_disarm = sv_io_sw_begin_disarm,
	.set_accept = sv_io_set_accept,
	.sync = sv_io_sync,
	.sleep_ms = sv_io_sleep_ms,
	.jiffies = sv_io_jiffies,
	.ms = sv_io_ms,
	.us = sv_io_us,
	.timer = sv_io_timer,
	.iso_timer = sv_io_iso_timer,
	.cpu_word = sv_io_cpu_word,
	.phy_down = sv_io_phy_down,
	.warn = sv_io_warn,
	.log = sv_io_log,
};

/*
 * Bind the sequences to @priv. @with_td only inside the control window, or
 * in rtl8372n_svlan_ctl_stop() before dsa_unregister_switch(), with the
 * svlan lock held; setup, quiesce and the svlan read touch registers only.
 */
static void sv_bind(struct rtl837x_priv *priv, struct sv_io *io,
		    struct sv_ctx *c, struct sv_cfg *cfg, bool with_td)
{
	struct dsa_switch *ds = priv->ds;
	u32 cpu = dsa_cpu_ports(ds);

	c->priv = priv;
	c->td = with_td && priv->sv.enabled ? rtl8_4s_ds_td(ds) : NULL;
	*io = sv_io_template;
	io->ctx = c;
	cfg->cpu = cpu ? __ffs(cpu) : 0;
	cfg->user_ports = dsa_user_ports(ds);
	cfg->phy_ports = cfg->user_ports & RTL8372N_PHY_PORT_MASK;
	/* What rtl8372n_setup_cpu_forwarding() writes to the CPU port. */
	cfg->user_mask = dsa_user_ports(ds);
	cfg->hz = HZ;
}

static void sv_deadline_fn(struct work_struct *work)
{
	struct rtl8372n_svlan *sv = container_of(to_delayed_work(work),
						 struct rtl8372n_svlan,
						 deadline);
	struct rtl837x_priv *priv = container_of(sv, struct rtl837x_priv, sv);
	struct sv_ctx c;
	struct sv_cfg cfg;
	struct sv_io io;

	mutex_lock(&sv->lock);
	if (sv->ctl_ready) {
		sv_bind(priv, &io, &c, &cfg, true);
		sv_seq_work(&sv->seq, &io, &cfg);
	}
	mutex_unlock(&sv->lock);
}

/* The isolate cpu restore; a forced path in the window cancels it. */
static void sv_iso_bench_fn(struct work_struct *work)
{
	struct rtl8372n_svlan *sv = container_of(to_delayed_work(work),
						 struct rtl8372n_svlan,
						 iso_bench);
	struct rtl837x_priv *priv = container_of(sv, struct rtl837x_priv, sv);
	struct sv_ctx c;
	struct sv_cfg cfg;
	struct sv_io io;

	mutex_lock(&sv->lock);
	if (sv->ctl_ready) {
		sv_bind(priv, &io, &c, &cfg, true);
		sv_seq_iso_restore(&sv->seq, &io, &cfg);
	}
	mutex_unlock(&sv->lock);
}

/*
 * RTNL held: every user port, up or down, must be in the offloaded bridge
 * and have no 802.1ad upper and no 8021q upper with a reserved VID.
 */
static void sv_topology(struct rtl837x_priv *priv, struct sv_arm_req *req)
{
	struct net_device *upper;
	struct list_head *iter;
	struct sv_port_info *pi;
	struct dsa_port *dp;
	u16 vid;

	ASSERT_RTNL();

	dsa_switch_for_each_user_port(dp, priv->ds) {
		if (dp->index >= RTL8_4S_NUM_PORTS)
			continue;
		pi = &req->port[dp->index];
		pi->bridged = !!dsa_port_bridge_dev_get(dp);
		if (!dp->user)
			continue;
		rcu_read_lock();
		netdev_for_each_upper_dev_rcu(dp->user, upper, iter) {
			if (!is_vlan_dev(upper))
				continue;
			vid = vlan_dev_vlan_id(upper);
			if (vlan_dev_vlan_proto(upper) == htons(ETH_P_8021AD))
				pi->upper_8021ad = true;
			else if (rtl8_4s_vid_reserved(vid))
				pi->upper_rsv_vid = vid;
		}
		rcu_read_unlock();
	}
}

/* vlan_lock: the hardware VLAN mode record and the shadow's use of 39xx. */
static void sv_collision(struct rtl837x_priv *priv, struct sv_arm_req *req)
{
	struct dsa_port *dp;
	u16 pvid = 0;
	int port;

	mutex_lock(&priv->vlan_lock);
	dsa_switch_for_each_user_port(dp, priv->ds) {
		if (dp->index < RTL8_4S_NUM_PORTS)
			req->port[dp->index].aware =
				priv->vlan_mode[dp->index] ==
				RTL8372N_VLAN_MODE_AWARE;
	}
	req->shadow_vid = rtl8372n_vlan_shadow_used(priv, RTL8_4S_VID_RSV_FIRST,
						    RTL8_4S_VID_RSV_LAST);
	port = rtl8372n_vlan_pvid_in(priv, RTL8_4S_VID_RSV_FIRST,
				     RTL8_4S_VID_RSV_LAST, &pvid);
	if (port >= 0) {
		req->pvid = pvid;
		req->pvid_port = port;
	}
	mutex_unlock(&priv->vlan_lock);
}

int rtl8372n_svlan_arm(struct rtl837x_priv *priv, unsigned int deadline_s)
{
	struct rtl8372n_svlan *sv = &priv->sv;
	struct sv_arm_req req = { };
	struct sv_ctx c;
	struct sv_cfg cfg;
	struct sv_io io;
	int ret;

	/*
	 * rtnl_lock_killable() is not exported to modules (6.18): restart the
	 * write instead of sleeping on RTNL, as DSA's own tagging write does,
	 * after one killable tick so as not to spin. Never sleep on the svlan
	 * lock under RTNL either (its holders run for up to seconds): wait for
	 * it killably without RTNL, then restart. A blocked writer stays
	 * killable, and RTNL is held only while the arm itself runs.
	 */
	if (!rtnl_trylock()) {
		schedule_timeout_killable(1);
		return restart_syscall();
	}
	if (!mutex_trylock(&sv->lock)) {
		rtnl_unlock();
		if (mutex_lock_killable(&sv->lock))
			return -EINTR;
		mutex_unlock(&sv->lock);
		return restart_syscall();
	}
	req.enabled = sv->enabled;
	req.ctl_ready = sv->ctl_ready;
	req.running = system_state == SYSTEM_RUNNING;
	req.deadline_s = deadline_s;
	if (sv->enabled && sv->ctl_ready) {
		req.td_present = !!rtl8_4s_ds_td(priv->ds);
		req.offload_disabled = priv->offload_disabled;
		sv_topology(priv, &req);
		sv_collision(priv, &req);
	}
	sv_bind(priv, &io, &c, &cfg, sv->enabled && sv->ctl_ready);
	ret = sv_seq_arm(&sv->seq, &io, &cfg, &req);
	mutex_unlock(&sv->lock);
	rtnl_unlock();
	return ret;
}

/*
 * The debugfs disarm, without RTNL. @flags: SV_DISARM_FORCE takes the
 * forced path over a PPE claim, SV_DISARM_NOPHY skips its PHY power-down.
 */
int rtl8372n_svlan_disarm(struct rtl837x_priv *priv, unsigned int flags)
{
	struct rtl8372n_svlan *sv = &priv->sv;
	struct sv_ctx c;
	struct sv_cfg cfg;
	struct sv_io io;
	int ret;

	if (!sv->enabled) {
		dev_err(priv->dev, "svlan: disarm: S mode is off (svlan=0 at bind)\n");
		return -EOPNOTSUPP;
	}
	if (mutex_lock_killable(&sv->lock))
		return -EINTR;
	if (sv->ctl_ready) {
		sv_bind(priv, &io, &c, &cfg, true);
		ret = sv_seq_disarm(&sv->seq, &io, &cfg, SV_TRIG_DEBUGFS, flags);
	} else {
		ret = -ESHUTDOWN;
	}
	mutex_unlock(&sv->lock);
	return ret;
}

/* "renew <s>": the lease (WP3-DESIGN §2.5), without RTNL or registers. */
int rtl8372n_svlan_renew(struct rtl837x_priv *priv, unsigned int secs)
{
	struct rtl8372n_svlan *sv = &priv->sv;
	struct sv_ctx c;
	struct sv_cfg cfg;
	struct sv_io io;
	int ret;

	if (!sv->enabled) {
		dev_err(priv->dev, "svlan: renew: S mode is off (svlan=0 at bind)\n");
		return -EOPNOTSUPP;
	}
	if (mutex_lock_killable(&sv->lock))
		return -EINTR;
	if (sv->ctl_ready) {
		sv_bind(priv, &io, &c, &cfg, true);
		ret = sv_seq_renew(&sv->seq, &io, &cfg, secs);
	} else {
		ret = -ESHUTDOWN;
	}
	mutex_unlock(&sv->lock);
	return ret;
}

/* "isolate cpu <s>", a bench verb: svlan lock, then vlan_lock (cpu_word). */
int rtl8372n_svlan_iso_bench(struct rtl837x_priv *priv, unsigned int secs)
{
	struct rtl8372n_svlan *sv = &priv->sv;
	u8 ppe = RTL8_4S_PPE_OFF;
	struct sv_ctx c;
	struct sv_cfg cfg;
	struct sv_io io;
	int ret;

	if (!sv->enabled) {
		dev_err(priv->dev, "svlan: isolate cpu: S mode is off (svlan=0 at bind)\n");
		return -EOPNOTSUPP;
	}
	if (mutex_lock_killable(&sv->lock))
		return -EINTR;
	if (sv->ctl_ready) {
		sv_bind(priv, &io, &c, &cfg, true);
		if (c.td)
			ppe = rtl8_4s_word_ppe(atomic_read(&c.td->st));
		ret = sv_seq_iso_bench(&sv->seq, &io, &cfg, secs, ppe);
	} else {
		ret = -ESHUTDOWN;
	}
	mutex_unlock(&sv->lock);
	return ret;
}

/*
 * D16 (WP3-DESIGN): set by a REMOVE that took the forced path, read by
 * every probe. Only a module reload clears it; the PPE's reference on this
 * module refuses that while it may still emit S-R frames.
 */
static struct sv_latch sv_latch;

/* rtl837x_mdio_probe(), before any register access. */
int rtl8372n_svlan_bind_check(struct device *dev)
{
	int ret = sv_latch_check(&sv_latch);

	if (ret)
		dev_err(dev,
			"svlan: bind refused: a forced remove kept S-R steering; reload rtl8372n_dsa once the PPE is off\n");
	return ret;
}

void rtl8372n_svlan_probe_init(struct rtl837x_priv *priv)
{
	struct rtl8372n_svlan *sv = &priv->sv;

	mutex_init(&sv->lock);
	INIT_DELAYED_WORK(&sv->deadline, sv_deadline_fn);
	INIT_DELAYED_WORK(&sv->iso_bench, sv_iso_bench_fn);
	atomic_set(&sv->topo_warn, 0);
	memset(&sv->seq, 0, sizeof(sv->seq));
	sv->seq.state = RTL8_4S_SW_OFF;
	sv->seq.accept = RTL8_4S_ACCEPT_8899;
	sv->enabled = READ_ONCE(rtl8372n_svlan_param);
	/* The pure entry word must be the driver's canonical one. */
	WARN_ON_ONCE(sv_entry_word(BIT(4) | BIT(3), BIT(4)) !=
		     rtl837x_vlan_entry_canonical(BIT(4) | BIT(3), BIT(4)));
	if (sv->enabled)
		dev_info(priv->dev,
			 "svlan: S-R steering mode: rtl8_4s, armed through debugfs only\n");
}

static int svlan_counters_show(struct seq_file *m, void *unused);
static int svlan_show(struct seq_file *m, void *unused);

static int svlan_counters_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, svlan_counters_show, inode->i_private,
				8192);
}

static const struct file_operations svlan_counters_fops = {
	.owner = THIS_MODULE,
	.open = svlan_counters_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int svlan_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, svlan_show, inode->i_private, 8192);
}

static ssize_t svlan_write(struct file *file, const char __user *ubuf,
			   size_t len, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct rtl837x_priv *priv = m->private;
	unsigned int secs;
	/* Zeroed for -Wmaybe-uninitialized (see the Makefile). */
	char buf[32] = { }, *cmd;
	int ret;

	if (!len || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	cmd = strim(buf);
	if (str_has_prefix(cmd, "arm ")) {
		if (kstrtouint(skip_spaces(cmd + 4), 10, &secs))
			return -EINVAL;
		ret = rtl8372n_svlan_arm(priv, secs);
	} else if (str_has_prefix(cmd, "renew ")) {
		if (kstrtouint(skip_spaces(cmd + 6), 10, &secs))
			return -EINVAL;
		ret = rtl8372n_svlan_renew(priv, secs);
	} else if (str_has_prefix(cmd, "isolate cpu ")) {
		if (kstrtouint(skip_spaces(cmd + 12), 10, &secs))
			return -EINVAL;
		ret = rtl8372n_svlan_iso_bench(priv, secs);
	} else if (!strcmp(cmd, "disarm")) {
		ret = rtl8372n_svlan_disarm(priv, 0);
	} else if (!strcmp(cmd, "disarm force")) {
		ret = rtl8372n_svlan_disarm(priv, SV_DISARM_FORCE);
	} else if (!strcmp(cmd, "disarm force nophy")) {
		ret = rtl8372n_svlan_disarm(priv, SV_DISARM_FORCE |
					    SV_DISARM_NOPHY);
	} else {
		return -EINVAL;
	}
	return ret ? ret : (ssize_t)len;
}

static const struct file_operations svlan_fops = {
	.owner = THIS_MODULE,
	.open = svlan_open,
	.read = seq_read,
	.write = svlan_write,
	.llseek = seq_lseek,
	.release = single_release,
};

void rtl8372n_svlan_ctl_start(struct rtl837x_priv *priv)
{
	struct rtl8372n_svlan *sv = &priv->sv;
	struct rtl8_4s_tagger_data *td;
	struct dentry *dir;

	dir = debugfs_create_dir("rtl8372n", NULL);
	if (IS_ERR(dir)) {
		dev_warn(priv->dev,
			 "svlan: no debugfs directory (%ld): S-R steering cannot be armed\n",
			 PTR_ERR(dir));
		return;
	}
	debugfs_create_file("svlan", 0600, dir, priv, &svlan_fops);
	debugfs_create_file("svlan_counters", 0444, dir, priv,
			    &svlan_counters_fops);

	mutex_lock(&sv->lock);
	sv->dir = dir;
	if (sv->enabled) {
		td = rtl8_4s_ds_td(priv->ds);
		if (!td)
			dev_err(priv->dev,
				"svlan: svlan=1 but no rtl8_4s tagger data\n");
		else if (sv->seq.state != RTL8_4S_SW_OFF)
			/* A FAULT latched by setup, before the tagger connected. */
			rtl8_4s_sw_set(td, sv->seq.state);
	}
	sv->ctl_ready = true;
	mutex_unlock(&sv->lock);
}

/*
 * Remove and shutdown: close the window, remove debugfs (which waits for
 * handlers in flight), cancel the works outside the lock, then disarm:
 * over a PPE claim that is the forced path (no work is queued again), and a
 * REMOVE that took it latches every later bind (D16). tagger_data is still
 * connected: dsa_unregister_switch() comes after, and shutdown never
 * disconnects it. An isolate cpu window ends without its restore: the
 * forced path and a remove's quiesce isolate the CPU port anyway, and a
 * shutdown is followed by a reset.
 */
void rtl8372n_svlan_ctl_stop(struct rtl837x_priv *priv, bool shutdown)
{
	enum sv_trigger trigger = shutdown ? SV_TRIG_SHUTDOWN : SV_TRIG_REMOVE;
	struct rtl8372n_svlan *sv = &priv->sv;
	struct dentry *dir;
	struct sv_ctx c;
	struct sv_cfg cfg;
	struct sv_io io;
	int ret;

	mutex_lock(&sv->lock);
	sv->ctl_ready = false;
	dir = sv->dir;
	sv->dir = NULL;
	mutex_unlock(&sv->lock);
	debugfs_remove_recursive(dir);
	cancel_delayed_work_sync(&sv->deadline);
	cancel_delayed_work_sync(&sv->iso_bench);

	if (!sv->enabled)
		return;
	if (shutdown && !READ_ONCE(rtl8372n_svlan_shutdown_disarm)) {
		dev_info(priv->dev,
			 "svlan: shutdown disarm (skipped: svlan_shutdown_disarm=0, state %s)\n",
			 sv_sw_state_name(READ_ONCE(sv->seq.state)));
		return;
	}
	mutex_lock(&sv->lock);
	sv_bind(priv, &io, &c, &cfg, true);
	ret = sv_seq_disarm(&sv->seq, &io, &cfg, trigger, 0);
	sv_latch_note(&sv_latch, &sv->seq);
	mutex_unlock(&sv->lock);
	if (shutdown)
		dev_info(priv->dev, "svlan: shutdown disarm (%d, state %s)\n",
			 ret, sv_sw_state_name(READ_ONCE(sv->seq.state)));
}

void rtl8372n_svlan_setup_pre_reset(struct rtl837x_priv *priv)
{
	struct sv_ctx c;
	struct sv_cfg cfg;
	struct sv_io io;

	sv_bind(priv, &io, &c, &cfg, false);
	sv_seq_dump(&io, "pre-reset");
}

/*
 * After the chip reset, svlan=0 too: log the block, then write the off
 * values, uplink first. Returns the uplink's error (setup fails, D13); any
 * other off field left set turns offload off and latches FAULT off_inert.
 */
int rtl8372n_svlan_setup_post_reset(struct rtl837x_priv *priv)
{
	struct rtl8372n_svlan *sv = &priv->sv;
	struct sv_ctx c;
	struct sv_cfg cfg;
	struct sv_io io;
	bool rest_ok;
	int ret;

	sv_bind(priv, &io, &c, &cfg, false);
	sv_seq_dump(&io, "post-reset");
	ret = sv_seq_off(&io, &cfg, "off", &rest_ok);
	if (ret)
		return ret;
	if (!rest_ok) {
		rtl8372n_disable_offload(priv, "S-VLAN block not off");
		mutex_lock(&sv->lock);
		sv_seq_setup_fault(&sv->seq);
		mutex_unlock(&sv->lock);
	}
	return 0;
}

/*
 * Teardown and setup failure, after rtl8372n_quiesce() has isolated every
 * port and powered the PHYs down: best effort; no tagger_data, no state.
 * The S block stays as it is while a forced path's steering is live
 * (WP3-DESIGN D3): the PPE may still emit S-R frames, which the tagger can
 * no longer report, and the next setup writes the off values after its
 * reset. A setup failure never keeps it: the probe zeroed the state.
 */
void rtl8372n_svlan_quiesce(struct rtl837x_priv *priv)
{
	struct sv_ctx c;
	struct sv_cfg cfg;
	struct sv_io io;
	bool rest_ok;

	if (sv_seq_quiesce_keep_s(&priv->sv.seq)) {
		dev_warn(priv->dev, "svlan: quiesce: S steering kept (forced)\n");
		return;
	}
	sv_bind(priv, &io, &c, &cfg, false);
	sv_seq_off(&io, &cfg, "quiesce", &rest_ok);
}

void rtl8372n_svlan_report(struct rtl837x_priv *priv)
{
	struct sv_ctx c;
	struct sv_cfg cfg;
	struct sv_io io;

	sv_bind(priv, &io, &c, &cfg, false);
	sv_seq_report(&io, &cfg);
}

/*
 * svlan=1, user ports, RTNL (which serialises this with the arm's upper
 * check): an 8021q upper with a reserved VID is always refused, an 802.1ad
 * upper while S-R steering is not OFF. That includes every FAULT, live or
 * not: the S block is then not verified off (that its other fields are
 * inert with the uplink 0 is inferred, not measured), and a FAULT lasts
 * until the next bind anyway.
 */
int rtl8372n_svlan_prechangeupper(struct dsa_switch *ds, int port,
				  struct netdev_notifier_changeupper_info *info)
{
	struct rtl837x_priv *priv = ds->priv;
	struct net_device *upper = info->upper_dev;
	__be16 proto;

	if (!priv->sv.enabled || !info->linking || !is_vlan_dev(upper) ||
	    !dsa_is_user_port(ds, port))
		return 0;
	proto = vlan_dev_vlan_proto(upper);
	if (proto == htons(ETH_P_8021Q) &&
	    rtl8_4s_vid_reserved(vlan_dev_vlan_id(upper))) {
		NL_SET_ERR_MSG_MOD(info->info.extack,
				   "VLANs 3900-3919 are reserved for S-R steering");
		return -EBUSY;
	}
	if (proto == htons(ETH_P_8021AD) &&
	    READ_ONCE(priv->sv.seq.state) != RTL8_4S_SW_OFF) {
		NL_SET_ERR_MSG_MOD(info->info.extack,
				   "802.1ad uppers are refused while S-R steering is armed or in FAULT");
		return -EBUSY;
	}
	return 0;
}

/*
 * A user port left the VLAN-aware bridge, or the bridge stopped filtering,
 * while S-R steering may be live (sv_seq_may_be_live(): not OFF, and not a
 * FAULT with the uplink verified clear): warn and count, refuse nothing.
 * Lock-free, outside vlan_lock. The arm checked every user port, so a port
 * that comes up later is already in the bridge and VLAN-aware.
 */
void rtl8372n_svlan_topology_note(struct dsa_switch *ds, int port)
{
	struct rtl837x_priv *priv = ds->priv;
	struct dsa_port *dp;

	if (!priv->sv.enabled || !dsa_is_user_port(ds, port) ||
	    !sv_seq_may_be_live(&priv->sv.seq))
		return;
	dp = dsa_to_port(ds, port);
	atomic_inc(&priv->sv.topo_warn);
	dev_warn(priv->dev,
		 "svlan: %s left the VLAN-aware bridge while armed\n",
		 dp->user ? netdev_name(dp->user) : "?");
}

/* Names for svlan_counters. */
static const char *sv_state_name(enum rtl8_4s_state st)
{
	static const char * const names[] = {
		[RTL8_4S_ST_OFF] = "off",
		[RTL8_4S_ST_SWITCH_READY] = "switch_ready",
		[RTL8_4S_ST_SR_READY] = "sr_ready",
		[RTL8_4S_ST_ACTIVE] = "active",
		[RTL8_4S_ST_DISARMING] = "disarming",
		[RTL8_4S_ST_FAULT] = "fault",
	};

	return (unsigned int)st < ARRAY_SIZE(names) ? names[st] : "?";
}

static const char * const sv_rx_names[RTL8_4S_RX_NUM] = {
	[RTL8_4S_RX_S] = "rx_s",
	[RTL8_4S_RX_S_NESTED] = "rx_s_nested",
	[RTL8_4S_RX_RT_FWD] = "rx_rt_fwd",
	[RTL8_4S_RX_RT_TRAP] = "rx_rt_trap",
};

static const char * const sv_drop_names[RTL8_4S_DROP_NUM] = {
	[RTL8_4S_DROP_NOT_READY] = "not_ready",
	[RTL8_4S_DROP_S_OFF] = "s_off",
	[RTL8_4S_DROP_HWACCEL] = "hwaccel",
	[RTL8_4S_DROP_TRUNC] = "trunc",
	[RTL8_4S_DROP_ETYPE] = "etype",
	[RTL8_4S_DROP_RT] = "rt",
	[RTL8_4S_DROP_RT_REASON0] = "rt_reason0",
	[RTL8_4S_DROP_SVID_DN] = "svid_dn",
	[RTL8_4S_DROP_SVID_OTHER] = "svid_other",
	[RTL8_4S_DROP_SVID_PORT] = "svid_port",
	[RTL8_4S_DROP_NO_USER] = "no_user",
	[RTL8_4S_DROP_INNER_RSV] = "inner_rsv",
};

static unsigned long sv_secs_left(unsigned long now, unsigned long until)
{
	return until && time_before(now, until) ? (until - now) / HZ : 0;
}

/*
 * debugfs svlan_counters: memory only (no register or MDIO access, which the
 * harness checks), so it may be read under load. state, sw_state and
 * ppe_state come from one read of the state word. The forced_* lines
 * describe the last forced path ("none" and "-" before the first);
 * steer_live is sv_seq_may_be_live(), spelt out (calls here stay on the
 * harness's memory-only list).
 */
static int svlan_counters_show(struct seq_file *m, void *unused)
{
	struct rtl837x_priv *priv = m->private;
	struct rtl8372n_svlan *sv = &priv->sv;
	struct rtl8_4s_tagger_data *td = NULL;
	const struct sv_seq *s = &sv->seq;
	const char *iso_cpu = "-", *iso_phy = "-";
	struct rtl8_4s_counters *cnt;
	unsigned long now = jiffies;
	unsigned int i, j;
	bool steer_live;
	u8 accept;
	int st, ret;

	cnt = kzalloc(sizeof(*cnt), GFP_KERNEL);
	if (!cnt)
		return -ENOMEM;
	ret = mutex_lock_killable(&sv->lock);
	if (ret) {
		kfree(cnt);
		return ret;
	}
	if (sv->ctl_ready && sv->enabled)
		td = rtl8_4s_ds_td(priv->ds);
	if (td) {
		st = atomic_read(&td->st);
		/* Pairs with sv_io_set_accept()'s smp_store_release(). */
		accept = smp_load_acquire(&td->accept);
		rtl8_4s_counters_sum(td, cnt);
	} else {
		st = RTL8_4S_WORD(s->state, RTL8_4S_PPE_OFF);
		accept = s->accept;
	}

	seq_printf(m, "abi %u\n", td ? td->abi : 0);
	seq_printf(m, "bound %d\n", td ? 1 : 0);
	seq_printf(m, "state %s\n", sv_state_name(rtl8_4s_word_state(st)));
	seq_printf(m, "sw_state %s\n", sv_sw_state_name(rtl8_4s_word_sw(st)));
	seq_printf(m, "fault%s%s%s%s%s\n", s->fault ? "" : " none",
		   s->fault & SV_FAULT_DISARM_INCOMPLETE ? " disarm_incomplete" : "",
		   s->fault & SV_FAULT_OFF_INERT ? " off_inert" : "",
		   s->fault & SV_FAULT_ENTRIES ? " entries" : "",
		   s->fault & SV_FAULT_FORCED_PPE ? " forced_ppe" : "");
	seq_printf(m, "accept %s\n", sv_accept_name(accept));
	seq_printf(m, "s_ports %#x\n", td ? READ_ONCE(td->s_ports) : s->s_ports);
	seq_printf(m, "ppe_state %s\n", sv_ppe_name(rtl8_4s_word_ppe(st)));
	seq_printf(m, "sw_gen %u\n", td ? READ_ONCE(td->sw_gen) : 0);
	seq_printf(m, "disarm_in_s %lu\n",
		   s->state == RTL8_4S_SW_READY ?
		   sv_secs_left(now, s->disarm_at) : 0);
	seq_printf(m, "none_ms %u\n", s->none_ms);
	seq_printf(m, "drain_ms %u\n", SV_DRAIN_MS);
	seq_printf(m, "entries_written %#x\n", s->entries_written);
	seq_printf(m, "retry_in_s %lu\n", sv_secs_left(now, s->retry_until));
	seq_printf(m, "arm_ok %u\n", s->arm_ok);
	seq_printf(m, "arm_fail %u\n", s->arm_fail);
	for (i = 0; i < SV_REF_NUM; i++)
		seq_printf(m, "refused_%s %u\n", sv_refusal_name(i),
			   s->refused[i]);
	for (i = 0; i < SV_TRIG_NUM; i++)
		seq_printf(m, "disarm_%s %u\n", sv_trigger_name(i),
			   s->disarm[i]);
	seq_printf(m, "force_requests %u\n", s->force_requests);
	seq_printf(m, "topo_warn %d\n", atomic_read(&sv->topo_warn));
	if (s->forced_n) {
		iso_cpu = s->forced_iso & SV_ISO_CPU_OK ? "ok" : "FAILED";
		if (s->forced_iso & SV_ISO_PHY_SKIPPED)
			iso_phy = "skipped";
		else
			iso_phy = s->forced_iso & SV_ISO_PHY_OK ? "ok" : "FAILED";
	}
	steer_live = s->state != RTL8_4S_SW_OFF &&
		     (s->state != RTL8_4S_SW_FAULT || s->incomplete ||
		      s->forced_live);
	seq_printf(m, "forced_n %u\n", s->forced_n);
	seq_printf(m, "forced_trig %s\n",
		   s->forced_n ? sv_trigger_name(s->forced_trig) : "none");
	seq_printf(m, "forced_ppe_state %s\n",
		   s->forced_n ? sv_ppe_name(s->forced_ppe) : "none");
	seq_printf(m, "forced_iso cpu %s phy %s\n", iso_cpu, iso_phy);
	seq_printf(m, "forced_cpu_was %#x\n", s->forced_cpu_was);
	seq_printf(m, "forced_us %u\n", s->forced_us);
	seq_printf(m, "steer_live %d\n", steer_live);
	seq_printf(m, "iso_failed %d\n", s->iso_failed);
	seq_printf(m, "renew_ok %u\n", s->renew_ok);
	seq_printf(m, "renew_refused_range %u\n",
		   s->renew_refused[SV_RENEW_RANGE]);
	seq_printf(m, "renew_refused_state %u\n",
		   s->renew_refused[SV_RENEW_STATE]);
	seq_printf(m, "renew_refused_shorter %u\n",
		   s->renew_refused[SV_RENEW_SHORTER]);
	seq_printf(m, "followup_n %u\n", s->followup_n);
	seq_printf(m, "bind_refused %u\n", READ_ONCE(sv_latch.refused));
	seq_printf(m, "iso_bench_n %u\n", s->iso_bench_n);
	seq_printf(m, "last_err %d %s\n", s->last_err,
		   sv_step_name(s->last_step));
	for (i = 0; i < RTL8_4S_RX_NUM; i++)
		for (j = 0; j < RTL8_4S_PORT_SLOTS; j++)
			seq_printf(m, "%s_p%u %llu\n", sv_rx_names[i], j,
				   cnt->rx[i][j]);
	for (i = 0; i < RTL8_4S_DROP_NUM; i++)
		seq_printf(m, "drop_%s %llu\n", sv_drop_names[i], cnt->drop[i]);
	mutex_unlock(&sv->lock);
	kfree(cnt);
	return 0;
}

/*
 * debugfs svlan, read: a live read-back of the S block and of the CPU
 * port's isolation word (idle only).
 */
static __printf(2, 3) void sv_out_print(void *ctx, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	seq_vprintf(ctx, fmt, args);
	va_end(args);
}

static int svlan_show(struct seq_file *m, void *unused)
{
	struct rtl837x_priv *priv = m->private;
	struct rtl8372n_svlan *sv = &priv->sv;
	struct sv_out out = { .ctx = m, .print = sv_out_print };
	struct sv_ctx c;
	struct sv_cfg cfg;
	struct sv_io io;
	int ret;

	ret = mutex_lock_killable(&sv->lock);
	if (ret)
		return ret;
	seq_printf(m, "svlan %d sw_state %s accept %s\n", sv->enabled,
		   sv_sw_state_name(sv->seq.state),
		   sv_accept_name(sv->seq.accept));
	sv_bind(priv, &io, &c, &cfg, false);
	sv_seq_show(&sv->seq, &io, &cfg, &out);
	mutex_unlock(&sv->lock);
	return 0;
}
