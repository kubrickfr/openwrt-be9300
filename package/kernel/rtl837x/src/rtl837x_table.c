// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8372N indirect table access engine and VLAN/L2 entry codecs.
 *
 * The table mutex, the idle wait before every command, the single full
 * command write and the verified VLAN write come from Maksims Neroba's
 * rtl837x_table.c in MNeroba/openwrt-flint3#1. The L2 key lookup and its hit
 * check come from his rtl837x_l2.c in MNeroba/openwrt-flint3#2, which was
 * bench-verified on a GL-BE9300 with a static multicast entry. The
 * transaction shape follows rtl8365mb_table_query() in Linux
 * drivers/net/dsa/realtek/rtl8365mb_table.c. The per-port flush follows his
 * rtl837x_l2_flush_port() in rtl837x_stp.c (#2): completion is the BUSY bit
 * alone, and the flush mode is restored only once the engine is idle; its
 * completion was bench-verified on ports 4-7. PROVENANCE.md lists the source
 * of each field; the unicast add, delete and walk paths have not yet run on
 * hardware.
 */
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/lockdep.h>
#include <linux/regmap.h>
#include <linux/string.h>

#include "rtl837x.h"

/* Every poll read costs several MDIO frames; 10 ms as in rtl8365mb. */
#define RTL837X_TABLE_POLL_US		10
#define RTL837X_TABLE_TIMEOUT_US	10000
/* The old driver never waited for a LUT clear, so its duration is unknown. */
#define RTL837X_L2_CLEAR_TIMEOUT_US	1000000
/* MNeroba#2's bound, within which its bench flushes completed. */
#define RTL837X_L2_FLUSH_TIMEOUT_US	1000000
#define RTL837X_L2_FLUSH_MODE_FIELDS	(RTL837X_L2_FLUSH_MODE_STATIC | \
					 RTL837X_L2_FLUSH_MODE_SEL)

#define RTL837X_VLAN_WORDS		1
#define RTL837X_L2_WORDS		3
#define RTL837X_L2_INDEX_MAX		FIELD_MAX(RTL837X_TABLE_L2_INDEX)
/* Age the SDK gives added entries; static entries are not aged out. */
#define RTL837X_L2_ADD_AGE		6
/* Word 2 bits compared after an add; how age reads back is unknown. */
#define RTL837X_L2_ADD_CHECK_W2		(RTL837X_L2_W2_STATIC | \
					 RTL837X_L2_W2_AUTH | \
					 RTL837X_L2_W2_SPA_HI)

/* Values of the command register's type field. */
enum rtl837x_table {
	RTL837X_TABLE_CVLAN = 3,
	RTL837X_TABLE_L2 = 4,
};

enum rtl837x_table_op {
	RTL837X_TABLE_OP_READ,
	RTL837X_TABLE_OP_WRITE,
};

/* Values of the L2 control register's read-method field. */
enum rtl837x_l2_method {
	RTL837X_L2_METHOD_MAC = 0,
	RTL837X_L2_METHOD_NEXT_UC_PORT = 7,
};

static int rtl837x_table_wait_idle(struct rtl837x_priv *priv, u32 *ctrl)
{
	return regmap_read_poll_timeout(priv->map, RTL837X_TABLE_CTRL, *ctrl,
					!(*ctrl & RTL837X_TABLE_CTRL_EXECUTE),
					RTL837X_TABLE_POLL_US,
					RTL837X_TABLE_TIMEOUT_US);
}

/*
 * Run one table transaction. For the VLAN table @addr is the VID. For an L2
 * next-entry read it is the start index; after any L2 operation the engine
 * completes (ACT_STS), it returns the entry index, and @l2_status, unless
 * NULL, the whole status word. @data holds the entry for a write, the key
 * for an L2 key lookup and the result of a read. An L2 operation the engine
 * does not complete returns -ENOENT: a lookup miss, or a write that stored
 * nothing. The caller maps that to its own error.
 */
static int rtl837x_table_query_locked(struct rtl837x_priv *priv,
				      enum rtl837x_table table,
				      enum rtl837x_table_op op, u16 *addr,
				      enum rtl837x_l2_method method, int port,
				      u32 *data, unsigned int nwords,
				      u32 *l2_status)
{
	bool is_l2 = table == RTL837X_TABLE_L2;
	bool is_read = op == RTL837X_TABLE_OP_READ;
	bool key_lookup = is_l2 && is_read && method == RTL837X_L2_METHOD_MAC;
	bool addr_in = !is_l2 || (is_read && !key_lookup);
	u32 cmd, ctrl = 0, status, l2_ctrl;
	unsigned int i;
	int ret;

	lockdep_assert_held(&priv->table_lock);

	if (!nwords || nwords > RTL837X_TABLE_MAX_WORDS ||
	    (addr_in && *addr > FIELD_MAX(RTL837X_TABLE_CTRL_ADDR)) ||
	    port < 0 || port > FIELD_MAX(RTL837X_TABLE_L2_PORT))
		return -EINVAL;

	/* An L2 write and a key lookup place the entry by its hashed key. */
	cmd = FIELD_PREP(RTL837X_TABLE_CTRL_ADDR, addr_in ? *addr : 0) |
	      FIELD_PREP(RTL837X_TABLE_CTRL_TYPE, table) |
	      (is_read ? 0 : RTL837X_TABLE_CTRL_WRITE) |
	      RTL837X_TABLE_CTRL_EXECUTE;

	/* A command that timed out earlier may still be running. */
	ret = rtl837x_table_wait_idle(priv, &ctrl);
	if (ret)
		goto err;

	if (is_l2) {
		/*
		 * Method, port filter and entry clear persist between
		 * operations, so set all three every time. With ENTRY_CLR set
		 * a write would clear the entry at the command's index.
		 */
		l2_ctrl = FIELD_PREP(RTL837X_TABLE_L2_METHOD,
				     is_read ? method : 0);
		if (is_read && method == RTL837X_L2_METHOD_NEXT_UC_PORT)
			l2_ctrl |= FIELD_PREP(RTL837X_TABLE_L2_PORT, port);
		ret = regmap_update_bits(priv->map, RTL837X_TABLE_L2_CTRL,
					 RTL837X_TABLE_L2_PORT |
					 RTL837X_TABLE_L2_ENTRY_CLR |
					 RTL837X_TABLE_L2_METHOD, l2_ctrl);
		if (ret)
			goto err;
	}

	if (!is_read || key_lookup) {
		for (i = 0; i < nwords; i++) {
			ret = rtl837x_reg_write(priv, RTL837X_TABLE_WRITE_DATA(i),
						data[i]);
			if (ret)
				goto err;
		}
	}

	ret = rtl837x_reg_write(priv, RTL837X_TABLE_CTRL, cmd);
	if (ret)
		goto err;
	ret = rtl837x_table_wait_idle(priv, &ctrl);
	if (ret)
		goto err;

	if (is_l2) {
		ret = rtl837x_reg_read(priv, RTL837X_TABLE_L2_CTRL, &status);
		if (ret)
			goto err;
		if (!(status & RTL837X_TABLE_L2_HIT))
			return -ENOENT;
		*addr = FIELD_GET(RTL837X_TABLE_L2_INDEX, status);
		if (l2_status)
			*l2_status = status;
	}

	if (is_read) {
		for (i = 0; i < nwords; i++) {
			ret = rtl837x_reg_read(priv, RTL837X_TABLE_READ_DATA(i),
					       &data[i]);
			if (ret)
				goto err;
		}
	}

	return 0;

err:
	dev_err_ratelimited(priv->dev,
			    "%s table command %#010x failed: %d (ctrl %#010x)\n",
			    is_l2 ? "L2" : "VLAN", cmd, ret, ctrl);
	return ret;
}

static int rtl837x_vlan_read_locked(struct rtl837x_priv *priv, u16 vid,
				    u32 *word)
{
	u16 addr = vid;

	return rtl837x_table_query_locked(priv, RTL837X_TABLE_CVLAN,
					  RTL837X_TABLE_OP_READ, &addr,
					  RTL837X_L2_METHOD_MAC, 0, word,
					  RTL837X_VLAN_WORDS, NULL);
}

/*
 * Write the canonical entry for @vid and read it back. An entry that reads
 * back otherwise fails with -EIO, as a failed transaction can; *@mismatch,
 * unless @mismatch is NULL, is set only in the first case. The VID policy (0
 * and 4095) belongs to the callers; setup may sample any entry.
 */
int rtl837x_vlan_entry_write(struct rtl837x_priv *priv, u16 vid, u16 member,
			     u16 untag, bool *mismatch)
{
	u32 word, readback;
	u16 addr = vid;
	int ret;

	if (mismatch)
		*mismatch = false;
	if (vid >= VLAN_N_VID ||
	    member > FIELD_MAX(RTL837X_VLAN_ENTRY_MEMBER) || (untag & ~member))
		return -EINVAL;

	word = rtl837x_vlan_entry_canonical(member, untag);

	mutex_lock(&priv->table_lock);
	ret = rtl837x_table_query_locked(priv, RTL837X_TABLE_CVLAN,
					 RTL837X_TABLE_OP_WRITE, &addr,
					 RTL837X_L2_METHOD_MAC, 0, &word,
					 RTL837X_VLAN_WORDS, NULL);
	/* Read the same VID back before another transaction can run. */
	if (!ret)
		ret = rtl837x_vlan_read_locked(priv, vid, &readback);
	mutex_unlock(&priv->table_lock);
	if (ret)
		return ret;

	if ((readback & RTL837X_VLAN_ENTRY_FIELDS) != word) {
		dev_err(priv->dev,
			"VLAN %u readback mismatch: wrote %#010x, read %#010x\n",
			vid, word, readback);
		if (mismatch)
			*mismatch = true;
		return -EIO;
	}
	if (readback & ~RTL837X_VLAN_ENTRY_FIELDS)
		dev_warn_once(priv->dev,
			      "VLAN %u reads back reserved bits: %#010x\n",
			      vid, readback);

	dev_dbg(priv->dev, "VLAN %u entry %#010x\n", vid, word);
	return 0;
}

/*
 * Write all-zero entries (no member, SVL, FID 0) to VIDs @first to @last, as
 * a VLAN table reset is expected to leave them, without reading them back:
 * for setup, when a table reset left some entry set. Stops at the first
 * failed transaction and returns its error.
 */
int rtl837x_vlan_table_zero(struct rtl837x_priv *priv, u16 first, u16 last)
{
	unsigned int vid;
	u32 word;
	u16 addr;
	int ret = 0;

	if (first > last || last >= VLAN_N_VID)
		return -EINVAL;

	for (vid = first; vid <= last; vid++) {
		addr = vid;
		word = 0;
		mutex_lock(&priv->table_lock);
		ret = rtl837x_table_query_locked(priv, RTL837X_TABLE_CVLAN,
						 RTL837X_TABLE_OP_WRITE, &addr,
						 RTL837X_L2_METHOD_MAC, 0, &word,
						 RTL837X_VLAN_WORDS, NULL);
		mutex_unlock(&priv->table_lock);
		if (ret) {
			dev_err(priv->dev, "VLAN %u not zeroed: %d\n", vid, ret);
			break;
		}
	}
	return ret;
}

int rtl837x_vlan_entry_read(struct rtl837x_priv *priv, u16 vid,
			    struct rtl837x_vlan_entry *entry)
{
	u32 word;
	int ret;

	if (vid >= VLAN_N_VID || !entry)
		return -EINVAL;

	mutex_lock(&priv->table_lock);
	ret = rtl837x_vlan_read_locked(priv, vid, &word);
	mutex_unlock(&priv->table_lock);
	if (ret)
		return ret;

	entry->raw = word;
	entry->member = FIELD_GET(RTL837X_VLAN_ENTRY_MEMBER, word);
	entry->untag = FIELD_GET(RTL837X_VLAN_ENTRY_UNTAG, word);
	entry->fid_msti = FIELD_GET(RTL837X_VLAN_ENTRY_FID_MSTI, word);
	entry->svlan_chk = FIELD_GET(RTL837X_VLAN_ENTRY_SVLAN_CHK, word);
	entry->ivl = FIELD_GET(RTL837X_VLAN_ENTRY_IVL, word);
	return 0;
}

static void rtl837x_l2_uc_encode(const struct rtl837x_l2_uc_entry *entry,
				 u32 *words)
{
	words[0] = FIELD_PREP(RTL837X_L2_W0_MAC2, entry->mac[2]) |
		   FIELD_PREP(RTL837X_L2_W0_MAC3, entry->mac[3]) |
		   FIELD_PREP(RTL837X_L2_W0_MAC4, entry->mac[4]) |
		   FIELD_PREP(RTL837X_L2_W0_MAC5, entry->mac[5]);
	words[1] = FIELD_PREP(RTL837X_L2_W1_SPA_LO, entry->port & 3) |
		   FIELD_PREP(RTL837X_L2_W1_IVL, entry->ivl) |
		   FIELD_PREP(RTL837X_L2_W1_VID_FID, entry->vid) |
		   FIELD_PREP(RTL837X_L2_W1_MAC0, entry->mac[0]) |
		   FIELD_PREP(RTL837X_L2_W1_MAC1, entry->mac[1]);
	words[2] = FIELD_PREP(RTL837X_L2_W2_STATIC, entry->is_static) |
		   FIELD_PREP(RTL837X_L2_W2_AUTH, entry->auth) |
		   FIELD_PREP(RTL837X_L2_W2_AGE, entry->age) |
		   FIELD_PREP(RTL837X_L2_W2_SPA_HI, entry->port >> 2);
}

/* Multicast and L3 entries share the words with another layout. */
static bool rtl837x_l2_uc_decode(const u32 *words,
				 struct rtl837x_l2_uc_entry *entry)
{
	if (words[1] & (RTL837X_L2_W1_L3 | RTL837X_L2_W1_MULTICAST))
		return false;

	entry->mac[0] = FIELD_GET(RTL837X_L2_W1_MAC0, words[1]);
	entry->mac[1] = FIELD_GET(RTL837X_L2_W1_MAC1, words[1]);
	entry->mac[2] = FIELD_GET(RTL837X_L2_W0_MAC2, words[0]);
	entry->mac[3] = FIELD_GET(RTL837X_L2_W0_MAC3, words[0]);
	entry->mac[4] = FIELD_GET(RTL837X_L2_W0_MAC4, words[0]);
	entry->mac[5] = FIELD_GET(RTL837X_L2_W0_MAC5, words[0]);
	entry->vid = FIELD_GET(RTL837X_L2_W1_VID_FID, words[1]);
	entry->ivl = FIELD_GET(RTL837X_L2_W1_IVL, words[1]);
	entry->port = FIELD_GET(RTL837X_L2_W2_SPA_HI, words[2]) << 2 |
		      FIELD_GET(RTL837X_L2_W1_SPA_LO, words[1]);
	entry->age = FIELD_GET(RTL837X_L2_W2_AGE, words[2]);
	entry->auth = FIELD_GET(RTL837X_L2_W2_AUTH, words[2]);
	entry->is_static = FIELD_GET(RTL837X_L2_W2_STATIC, words[2]);
	return true;
}

/* The SDK treats an entry with no age, static or auth bit as empty. */
static bool rtl837x_l2_uc_valid(const struct rtl837x_l2_uc_entry *entry)
{
	return entry->age || entry->is_static || entry->auth;
}

static bool rtl837x_l2_key_valid(const u8 *mac, u16 vid)
{
	return mac && !is_multicast_ether_addr(mac) && vid < VLAN_N_VID;
}

/*
 * Look up the IVL key {@mac, @vid}. Returns -ENOENT unless a valid unicast
 * entry matches; @words and @entry then hold the stored entry. The entry's
 * index, and status bit 13 with it, is not used.
 */
static int rtl837x_l2_lookup_locked(struct rtl837x_priv *priv, const u8 *mac,
				    u16 vid, u32 *words,
				    struct rtl837x_l2_uc_entry *entry)
{
	struct rtl837x_l2_uc_entry key = { .vid = vid, .ivl = true };
	u16 index = 0;
	int ret;

	memcpy(key.mac, mac, ETH_ALEN);
	rtl837x_l2_uc_encode(&key, words);
	ret = rtl837x_table_query_locked(priv, RTL837X_TABLE_L2,
					 RTL837X_TABLE_OP_READ, &index,
					 RTL837X_L2_METHOD_MAC, 0, words,
					 RTL837X_L2_WORDS, NULL);
	if (ret)
		return ret;

	if (!rtl837x_l2_uc_decode(words, entry) ||
	    !rtl837x_l2_uc_valid(entry)) {
		dev_dbg(priv->dev,
			"L2 %pM vid %u: index %u holds no unicast entry: %#010x %#010x %#010x\n",
			mac, vid, index, words[0], words[1], words[2]);
		return -ENOENT;
	}
	if (!ether_addr_equal_unaligned(entry->mac, mac) ||
	    entry->vid != vid || !entry->ivl) {
		dev_err(priv->dev,
			"L2 %pM vid %u: lookup returned another key: %#010x %#010x %#010x\n",
			mac, vid, words[0], words[1], words[2]);
		return -EIO;
	}
	return 0;
}

int rtl837x_l2_lookup(struct rtl837x_priv *priv, const u8 *mac, u16 vid,
		      struct rtl837x_l2_uc_entry *entry)
{
	u32 words[RTL837X_L2_WORDS];
	int ret;

	if (!rtl837x_l2_key_valid(mac, vid) || !entry)
		return -EINVAL;

	mutex_lock(&priv->table_lock);
	ret = rtl837x_l2_lookup_locked(priv, mac, vid, words, entry);
	mutex_unlock(&priv->table_lock);
	return ret;
}

/*
 * Add or replace the static IVL entry {@mac, @vid} -> @port. The hardware
 * hashes the key: an entry with the same key is updated in place, otherwise
 * the bucket's free or learned slot is used. As in the SDK, a lookup after
 * the write tells a full bucket from success.
 */
int rtl837x_l2_add_static(struct rtl837x_priv *priv, const u8 *mac, u16 vid,
			  int port)
{
	struct rtl837x_l2_uc_entry found, entry = {
		.vid = vid,
		.ivl = true,
		.port = port,
		.age = RTL837X_L2_ADD_AGE,
		.auth = true,
		.is_static = true,
	};
	u32 words[RTL837X_L2_WORDS], stored[RTL837X_L2_WORDS];
	u16 index = 0;
	int ret;

	if (!rtl837x_l2_key_valid(mac, vid) || port < 0 ||
	    port >= priv->num_ports)
		return -EINVAL;

	memcpy(entry.mac, mac, ETH_ALEN);
	rtl837x_l2_uc_encode(&entry, words);

	mutex_lock(&priv->table_lock);
	ret = rtl837x_table_query_locked(priv, RTL837X_TABLE_L2,
					 RTL837X_TABLE_OP_WRITE, &index,
					 RTL837X_L2_METHOD_MAC, 0, words,
					 RTL837X_L2_WORDS, NULL);
	if (!ret)
		ret = rtl837x_l2_lookup_locked(priv, mac, vid, stored, &found);
	mutex_unlock(&priv->table_lock);
	/* Not stored by the write, or missing afterwards: the bucket is full. */
	if (ret == -ENOENT)
		return -ENOSPC;
	if (ret)
		return ret;

	if (stored[0] != words[0] || stored[1] != words[1] ||
	    ((stored[2] ^ words[2]) & RTL837X_L2_ADD_CHECK_W2)) {
		dev_err(priv->dev,
			"L2 %pM vid %u port %d: wrote %#010x %#010x %#010x, read %#010x %#010x %#010x\n",
			mac, vid, port, words[0], words[1], words[2],
			stored[0], stored[1], stored[2]);
		return -EIO;
	}

	dev_dbg(priv->dev, "L2 %pM vid %u: static entry for port %d\n",
		mac, vid, port);
	return 0;
}

/*
 * Remove the static entry {@mac, @vid} if it belongs to @port. A missing
 * entry, another port's entry and a learned entry are left alone and count
 * as success. The switch may learn the address again as soon as the static
 * entry is gone, so only that static entry, still found afterwards, fails
 * the delete.
 */
int rtl837x_l2_del_static(struct rtl837x_priv *priv, const u8 *mac, u16 vid,
			  int port)
{
	struct rtl837x_l2_uc_entry found, key = { .vid = vid, .ivl = true };
	u32 words[RTL837X_L2_WORDS];
	u16 index = 0;
	int ret;

	if (!rtl837x_l2_key_valid(mac, vid) || port < 0 ||
	    port >= priv->num_ports)
		return -EINVAL;

	mutex_lock(&priv->table_lock);
	ret = rtl837x_l2_lookup_locked(priv, mac, vid, words, &found);
	if (ret)
		goto out;
	if (found.port != port || !found.is_static) {
		dev_dbg(priv->dev,
			"L2 %pM vid %u: kept entry of port %u (static %d)\n",
			mac, vid, found.port, found.is_static);
		goto out;
	}

	/*
	 * The SDK deletes by rewriting the key with no port, age, auth or
	 * static bit. The completion status of that write is not reliable
	 * (rtl8365mb reports a hit), so a second lookup decides. A learned
	 * entry found there was learned after the delete, on this port or
	 * another one, and is left alone like any learned entry.
	 */
	memcpy(key.mac, mac, ETH_ALEN);
	rtl837x_l2_uc_encode(&key, words);
	ret = rtl837x_table_query_locked(priv, RTL837X_TABLE_L2,
					 RTL837X_TABLE_OP_WRITE, &index,
					 RTL837X_L2_METHOD_MAC, 0, words,
					 RTL837X_L2_WORDS, NULL);
	if (ret && ret != -ENOENT)
		goto out;
	ret = rtl837x_l2_lookup_locked(priv, mac, vid, words, &found);
	if (!ret && found.is_static && found.port == port) {
		dev_err(priv->dev,
			"L2 %pM vid %u port %d: static entry still present after delete\n",
			mac, vid, port);
		ret = -EIO;
	} else if (!ret) {
		dev_dbg(priv->dev,
			"L2 %pM vid %u: deleted, then found on port %u (static %d)\n",
			mac, vid, found.port, found.is_static);
	}
out:
	mutex_unlock(&priv->table_lock);
	return ret == -ENOENT ? 0 : ret;
}

/*
 * Find the first valid unicast entry of @port at or after *@index and store
 * its index there. Returns -ENOENT at the end of the table; the search wraps,
 * so an index below the start also ends the walk. The 12-bit status index
 * cannot name entries above 4095, so a start past 4095 (the next index after
 * a hit at 4095) is also the end. A hit with status bit 13 set ends the walk
 * too, with a warning: if that bit flags an entry of the 64-entry overflow
 * CAM (4096-4159), as it may, the index read is not the entry's, so neither
 * the entry's place nor the next start can be trusted. @entry is undefined
 * on error.
 */
int rtl837x_l2_next_uc(struct rtl837x_priv *priv, u16 *index, int port,
		       struct rtl837x_l2_uc_entry *entry)
{
	u32 words[RTL837X_L2_WORDS], status = 0;
	u16 start, addr;
	int ret;

	if (!index || !entry || port < 0 || port >= priv->num_ports)
		return -EINVAL;
	if (*index > RTL837X_L2_INDEX_MAX)
		return -ENOENT;

	/*
	 * Every pass moves the start forward, so this ends by index 4095. The
	 * lock is held for one engine step at a time, so that FDB adds and
	 * deletes and the flush do not wait for a scan over many entries of
	 * other ports; a walk is not atomic anyway, as the switch learns and
	 * ages on its own.
	 */
	for (start = *index; ; start = addr + 1) {
		addr = start;
		mutex_lock(&priv->table_lock);
		ret = rtl837x_table_query_locked(priv, RTL837X_TABLE_L2,
						 RTL837X_TABLE_OP_READ, &addr,
						 RTL837X_L2_METHOD_NEXT_UC_PORT,
						 port, words, RTL837X_L2_WORDS,
						 &status);
		mutex_unlock(&priv->table_lock);
		if (ret)
			break;
		if (status & RTL837X_TABLE_L2_TYPE) {
			dev_warn_ratelimited(priv->dev,
					     "port %d: L2 walk from %u stopped at status %#010x (bit 13): %#010x %#010x %#010x\n",
					     port, start, status, words[0],
					     words[1], words[2]);
			ret = -ENOENT;
			break;
		}
		if (addr < start) {
			ret = -ENOENT;
			break;
		}
		/* Filter in software too, in case the engine ignores the port. */
		if (rtl837x_l2_uc_decode(words, entry) &&
		    rtl837x_l2_uc_valid(entry) && entry->port == port) {
			*index = addr;
			break;
		}
		if (addr == RTL837X_L2_INDEX_MAX) {
			ret = -ENOENT;
			break;
		}
	}
	return ret;
}

/* Remove every static and dynamic L2 entry. */
int rtl837x_l2_clear(struct rtl837x_priv *priv)
{
	u32 ctrl = 0, clear = 0;
	ktime_t start;
	int ret;

	mutex_lock(&priv->table_lock);
	ret = rtl837x_table_wait_idle(priv, &ctrl);
	if (ret)
		goto out;
	ret = regmap_read_poll_timeout(priv->map, RTL837X_L2_FLUSH_ALL, clear,
				       !(clear & RTL837X_L2_FLUSH_ALL_EXEC),
				       RTL837X_TABLE_POLL_US,
				       RTL837X_L2_CLEAR_TIMEOUT_US);
	if (ret)
		goto out;

	start = ktime_get();
	ret = regmap_set_bits(priv->map, RTL837X_L2_FLUSH_ALL,
			      RTL837X_L2_FLUSH_ALL_EXEC);
	if (ret)
		goto out;
	ret = regmap_read_poll_timeout(priv->map, RTL837X_L2_FLUSH_ALL, clear,
				       !(clear & RTL837X_L2_FLUSH_ALL_EXEC),
				       RTL837X_TABLE_POLL_US,
				       RTL837X_L2_CLEAR_TIMEOUT_US);
	if (!ret)
		dev_dbg(priv->dev, "L2 table cleared in %lld us\n",
			ktime_us_delta(ktime_get(), start));
out:
	mutex_unlock(&priv->table_lock);
	if (ret)
		dev_err(priv->dev,
			"L2 table clear failed: %d (table ctrl %#010x, clear %#010x)\n",
			ret, ctrl, clear);
	return ret;
}

/* Only BUSY tells completion: the port mask stays latched afterwards. */
static int rtl837x_l2_flush_wait_idle(struct rtl837x_priv *priv, u32 *cmd)
{
	return regmap_read_poll_timeout(priv->map, RTL837X_L2_FLUSH_CMD, *cmd,
					!(*cmd & RTL837X_L2_FLUSH_CMD_BUSY),
					RTL837X_TABLE_POLL_US,
					RTL837X_L2_FLUSH_TIMEOUT_US);
}

/*
 * Remove the dynamic (learned) unicast entries of @port; static entries stay.
 * A mode other than "by port, dynamic only" (never seen: bits 2:0 read 0
 * around every bench flush) is replaced for the command, checked, and put
 * back. It is put back only while the engine is known to be idle: under a
 * flush that may still run, a restored STATIC bit could widen it to static
 * entries, so a busy timeout leaves the narrowest mode instead.
 */
int rtl837x_l2_flush_port(struct rtl837x_priv *priv, int port)
{
	u32 ctrl = 0, cmd = 0, mode = 0, check = 0;
	bool restore = false;
	int ret, err;

	if (port < 0 || port >= priv->num_ports)
		return -EINVAL;

	mutex_lock(&priv->table_lock);
	ret = rtl837x_table_wait_idle(priv, &ctrl);
	if (ret)
		goto out;
	ret = rtl837x_l2_flush_wait_idle(priv, &cmd);
	if (ret)
		goto out;
	ret = rtl837x_reg_read(priv, RTL837X_L2_FLUSH_MODE, &mode);
	if (ret)
		goto out;
	if (mode & RTL837X_L2_FLUSH_MODE_FIELDS) {
		restore = true;
		ret = regmap_update_bits(priv->map, RTL837X_L2_FLUSH_MODE,
					 RTL837X_L2_FLUSH_MODE_FIELDS, 0);
		if (!ret)
			ret = rtl837x_reg_read(priv, RTL837X_L2_FLUSH_MODE,
					       &check);
		if (!ret && (check & RTL837X_L2_FLUSH_MODE_FIELDS))
			ret = -EIO;
		if (ret)
			goto restore;
	}

	/* One plain write, as in MNeroba#2: START and this port alone. */
	ret = rtl837x_reg_write(priv, RTL837X_L2_FLUSH_CMD,
				RTL837X_L2_FLUSH_CMD_START |
				FIELD_PREP(RTL837X_L2_FLUSH_CMD_PORTS, BIT(port)));
	/* A write that reported an error may still have started a flush. */
	err = rtl837x_l2_flush_wait_idle(priv, &cmd);
	if (err) {
		ret = ret ?: err;
		if (restore)
			dev_err(priv->dev,
				"port %d: L2 flush mode left at %#x, was %#x\n",
				port, check, mode);
		goto out;
	}

restore:
	if (restore) {
		err = regmap_update_bits(priv->map, RTL837X_L2_FLUSH_MODE,
					 RTL837X_L2_FLUSH_MODE_FIELDS, mode);
		ret = ret ?: err;
	}
out:
	mutex_unlock(&priv->table_lock);
	if (ret)
		dev_err_ratelimited(priv->dev,
				    "port %d L2 flush failed: %d (table ctrl %#010x, flush cmd %#010x, mode %#010x)\n",
				    port, ret, ctrl, cmd, mode);
	else
		dev_dbg(priv->dev, "port %d: dynamic L2 entries flushed (cmd %#x)\n",
			port, cmd);
	return ret;
}
