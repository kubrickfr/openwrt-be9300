// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * S-R steering, switch side: the sequences of WP1-DESIGN §4.3-§4.8, kept
 * free of kernel calls so that the desk harness (sr/wp1/harness/switch)
 * drives them over a fake register map with a fault at every access.
 * rtl8372n_svlan.c binds struct sv_io to the regmap, the VLAN table, the
 * rtl8_4s state word, the deadline work and dev_*() logging.
 *
 * Masks: every compare covers only the field named, and logs the whole word.
 * An entry is empty when no member, untag or FID/MSTI bit is set
 * (RTL837X_VLAN_ENTRY_USED): a verified removal leaves canonical(0, 0) =
 * 0x02000000 (IVL), setup's zeroing leaves 0, and both are empty.
 */
#define RTL8_4S_PURE
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/types.h>

#include "rtl837x_regmap.h"
#include "rtl8372n_svlan_seq.h"

#define SV_ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#define SV_CPU_TAG_ETYPE	0x8899
/*
 * See struct sv_seq: whole-byte stores and loads for the lock-free readers.
 * This file has no <linux/compiler.h> (it builds in user space too), so this
 * is READ_ONCE()/WRITE_ONCE() spelt out.
 */
#define SV_ONCE(x)		(*(volatile __typeof__(x) *)&(x))
#define SV_SET_STATE(s, v)	(SV_ONCE((s)->state) = (v))
#define SV_SET_INCOMPLETE(s, v)	(SV_ONCE((s)->incomplete) = (v))
#define SV_SET_FORCED_LIVE(s, v) (SV_ONCE((s)->forced_live) = (v))

/* @io is always a plain pointer variable. */
#define sv_log(io, lvl, ...)	((io)->log((io)->ctx, (lvl), __VA_ARGS__))

static const char * const sv_trigger_names[SV_TRIG_NUM] = {
	[SV_TRIG_DEBUGFS] = "debugfs",
	[SV_TRIG_DEADLINE] = "deadline",
	[SV_TRIG_RETRY] = "retry",
	[SV_TRIG_REMOVE] = "remove",
	[SV_TRIG_SHUTDOWN] = "shutdown",
	[SV_TRIG_ROLLBACK] = "rollback",
	[SV_TRIG_FORCED_FOLLOWUP] = "forced_followup",
};

static const char * const sv_refusal_names[SV_REF_NUM] = {
	[SV_REF_DISABLED] = "disabled",
	[SV_REF_SHUTDOWN] = "shutdown",
	[SV_REF_DEADLINE] = "deadline",
	[SV_REF_ALREADY] = "already",
	[SV_REF_FAULT] = "fault",
	[SV_REF_NO_TAGGER] = "no_tagger",
	[SV_REF_OFFLOAD] = "offload",
	[SV_REF_TOPOLOGY] = "topology",
	[SV_REF_COLLISION] = "collision",
	[SV_REF_PRECHECK] = "precheck",
};

static const char * const sv_step_names[SV_STEP_NUM] = {
	[SV_STEP_NONE] = "none",
	[SV_STEP_SETUP] = "setup",
	[SV_STEP_A1] = "A1", [SV_STEP_A3] = "A3", [SV_STEP_A4] = "A4",
	[SV_STEP_A5] = "A5", [SV_STEP_A6] = "A6", [SV_STEP_A7] = "A7",
	[SV_STEP_A8] = "A8", [SV_STEP_A9] = "A9",
	[SV_STEP_X3] = "X3", [SV_STEP_X4] = "X4", [SV_STEP_X5] = "X5",
	[SV_STEP_X8] = "X8",
	[SV_STEP_F1] = "F1", [SV_STEP_F2] = "F2",
};

const char *sv_trigger_name(unsigned int trigger)
{
	return trigger < SV_TRIG_NUM ? sv_trigger_names[trigger] : "?";
}

const char *sv_refusal_name(unsigned int refusal)
{
	return refusal < SV_REF_NUM ? sv_refusal_names[refusal] : "?";
}

const char *sv_step_name(unsigned int step)
{
	return step < SV_STEP_NUM ? sv_step_names[step] : "?";
}

const char *sv_accept_name(unsigned int accept)
{
	switch (accept) {
	case RTL8_4S_ACCEPT_NONE:
		return "none";
	case RTL8_4S_ACCEPT_8899:
		return "8899";
	case RTL8_4S_ACCEPT_S:
		return "s";
	}
	return "?";
}

const char *sv_sw_state_name(unsigned int sw)
{
	switch (sw) {
	case RTL8_4S_SW_OFF:
		return "off";
	case RTL8_4S_SW_ARMING:
		return "arming";
	case RTL8_4S_SW_READY:
		return "ready";
	case RTL8_4S_SW_DISARMING:
		return "disarming";
	case RTL8_4S_SW_FAULT:
		return "fault";
	}
	return "?";
}

const char *sv_ppe_name(unsigned int ppe)
{
	switch (ppe) {
	case RTL8_4S_PPE_OFF:
		return "off";
	case RTL8_4S_PPE_SR_READY:
		return "sr_ready";
	case RTL8_4S_PPE_ACTIVE:
		return "active";
	case RTL8_4S_PPE_FAULT:
		return "fault";
	}
	return "?";
}

static bool sv_before(unsigned long a, unsigned long b)
{
	return (long)(a - b) < 0;
}

/* @v unshifted into the field @mask. */
static u32 sv_prep(u32 mask, u32 v)
{
	return (v << __builtin_ctz(mask)) & mask;
}

u32 sv_entry_word(u16 member, u16 untag)
{
	return sv_prep(RTL837X_VLAN_ENTRY_MEMBER, member) |
	       sv_prep(RTL837X_VLAN_ENTRY_UNTAG, untag) |
	       RTL837X_VLAN_ENTRY_IVL;
}

static u16 sv_port_svid(const struct sv_cfg *cfg, unsigned int port)
{
	return port < RTL8_4S_NUM_PORTS && (cfg->user_ports & BIT(port)) ?
	       rtl8_4s_svid_up(port) : 0;
}

u32 sv_port_svid_word(const struct sv_cfg *cfg, unsigned int k)
{
	return (u32)sv_port_svid(cfg, 2 * k + 1) << 12 | sv_port_svid(cfg, 2 * k);
}

unsigned int sv_entry_vids(const struct sv_cfg *cfg, u16 *vids)
{
	unsigned int p, n = 0;

	for (p = 0; p < RTL8_4S_NUM_PORTS; p++) {
		if (!(cfg->user_ports & BIT(p)))
			continue;
		vids[n++] = rtl8_4s_svid_up(p);
		vids[n++] = rtl8_4s_svid_dn(p);
	}
	return n;
}

/* The user port a driver-owned VID serves, or -1. */
static int sv_vid_port(const struct sv_cfg *cfg, u16 vid)
{
	int p;

	if (vid >= RTL8_4S_SVID_DN_BASE)
		p = vid - RTL8_4S_SVID_DN_BASE;
	else
		p = vid - RTL8_4S_SVID_UP_BASE;
	if (p < 0 || p >= RTL8_4S_NUM_PORTS || !(cfg->user_ports & BIT(p)))
		return -1;
	return p;
}

static u32 sv_vid_bit(u16 vid)
{
	return BIT(vid - RTL8_4S_VID_RSV_FIRST);
}

/*
 * The S fields: the A3-A8 targets in arm order. off is what setup, X3-X5
 * and quiesce write and what the prechecks require.
 */
struct sv_field {
	u32 reg;
	u32 mask;
	u32 off;
	u32 armed;
	u8 step;
	const char *name;
};

#define SVF_INSERT	0
#define SVF_TPID	1
#define SVF_PORT(k)	(2 + (k))
#define SVF_UNTAG	(SVF_PORT(RTL837X_SVLAN_PORT_SVID_WORDS))
#define SVF_CTRL	(SVF_UNTAG + 1)
#define SVF_UPLINK	(SVF_CTRL + 1)
#define SVF_NUM		(SVF_UPLINK + 1)

static void sv_sfield(const struct sv_cfg *cfg, unsigned int i,
		      struct sv_field *f)
{
	static const char * const port_names[RTL837X_SVLAN_PORT_SVID_WORDS] = {
		"port S-VID 0/1", "port S-VID 2/3", "port S-VID 4/5",
		"port S-VID 6/7", "port S-VID 8/9",
	};

	if (i >= SVF_PORT(0) && i < SVF_UNTAG) {
		*f = (struct sv_field){
			RTL837X_SVLAN_PORT_SVID(i - SVF_PORT(0)),
			RTL837X_SVLAN_PORT_SVID_FIELDS, 0,
			sv_port_svid_word(cfg, i - SVF_PORT(0)), SV_STEP_A5,
			port_names[i - SVF_PORT(0)] };
		return;
	}
	switch (i) {
	case SVF_INSERT:
		*f = (struct sv_field){
			RTL837X_CPU_TAG_CTRL, RTL837X_CPU_TAG_INSERT_MODE,
			sv_prep(RTL837X_CPU_TAG_INSERT_MODE,
				RTL837X_CPU_TAG_INSERT_ALL),
			sv_prep(RTL837X_CPU_TAG_INSERT_MODE,
				RTL837X_CPU_TAG_INSERT_TRAPPING),
			SV_STEP_A3, "insert mode" };
		break;
	case SVF_TPID:
		*f = (struct sv_field){
			RTL837X_SVLAN_TPID, RTL837X_SVLAN_TPID_MASK,
			RTL837X_SVLAN_TPID_8021AD, RTL837X_SVLAN_TPID_8021AD,
			SV_STEP_A4, "S-TPID" };
		break;
	case SVF_UNTAG:
		*f = (struct sv_field){
			RTL837X_SVLAN_UNTAG_SVID, RTL837X_SVLAN_UNTAG_SVID_MASK,
			0, RTL8_4S_SVID_UP_BASE, SV_STEP_A6, "untag S-VID" };
		break;
	case SVF_CTRL:
		/* SPRISEL 1 (S-PCP from the C-tag), UIFSEG 0, UNTAG 0 (drop). */
		*f = (struct sv_field){
			RTL837X_SVLAN_CTRL, RTL837X_SVLAN_CTRL_FIELDS, 0,
			sv_prep(RTL837X_SVLAN_CTRL_SPRISEL, 1), SV_STEP_A7,
			"S control" };
		break;
	default:
		*f = (struct sv_field){
			RTL837X_SVLAN_UPLINK, RTL837X_SVLAN_UPLINK_PORTS, 0,
			BIT(cfg->cpu), SV_STEP_A8, "uplink" };
		break;
	}
}

/* The precheck fields that S-R steering never writes. */
#define SVX_NUM		5

static void sv_xfield(const struct sv_cfg *cfg, unsigned int i,
		      struct sv_field *f)
{
	u32 mask, val;

	switch (i) {
	case 0:
		/* Measured word 0x102: the tag on, insert mode checked apart. */
		mask = RTL837X_CPU_TAG_ENABLE;
		*f = (struct sv_field){ RTL837X_CPU_TAG_CTRL, mask, mask, mask,
					SV_STEP_NONE, "CPU tag enable" };
		break;
	case 1:
		mask = RTL837X_EXTERNAL_CPU_PORT_MASK;
		val = sv_prep(mask, cfg->cpu);
		*f = (struct sv_field){ RTL837X_EXTERNAL_CPU_PORT, mask, val,
					val, SV_STEP_NONE, "CPU port" };
		break;
	case 2:
		mask = RTL837X_CPU_TAG_AWARE_PORTS;
		val = BIT(cfg->cpu);
		*f = (struct sv_field){ RTL837X_CPU_TAG_AWARE, mask, val, val,
					SV_STEP_NONE, "tag-aware ports" };
		break;
	case 3:
		mask = RTL837X_CPU_TAG_TPID_MASK;
		*f = (struct sv_field){ RTL837X_CPU_TAG_TPID, mask,
					SV_CPU_TAG_ETYPE, SV_CPU_TAG_ETYPE,
					SV_STEP_NONE, "CPU tag TPID" };
		break;
	default:
		mask = RTL837X_VLAN_EGRESS_TAG_MODE_MASK(cfg->cpu);
		val = sv_prep(mask, RTL837X_VLAN_EGRESS_MODE_REAL_KEEP);
		*f = (struct sv_field){ RTL837X_VLAN_EGRESS_TAG, mask, val, val,
					SV_STEP_NONE, "CPU egress mode" };
		break;
	}
}

/* What a verification covers. */
#define SVV_FIXED	BIT(0)
#define SVV_SBLOCK	BIT(1)
#define SVV_C2S		BIT(2)
#define SVV_ENTRIES	BIT(3)
#define SVV_TRAP_CTRL	BIT(4)
#define SVV_ALL		(SVV_FIXED | SVV_SBLOCK | SVV_C2S | SVV_ENTRIES | \
			 SVV_TRAP_CTRL)

/* Tokens of the show's MISMATCH list: a register, a VID or a C2S word. */
#define SV_TOK_VID	0x10000
#define SV_TOK_C2S	0x20000

struct sv_verify {
	const struct sv_io *io;
	const struct sv_cfg *cfg;
	const struct sv_out *out;	/* the svlan read, or NULL */
	const char *pfx;		/* log prefix of mismatches, or NULL */
	int lvl;
	int rd_err;			/* the first read error */
	unsigned int nbad;
	u32 bad[16];
};

static void sv_v_bad(struct sv_verify *v, u32 tok, int ret)
{
	if (ret && !v->rd_err)
		v->rd_err = ret;
	if (v->nbad < SV_ARRAY_SIZE(v->bad))
		v->bad[v->nbad] = tok;
	v->nbad++;
}

static void sv_v_field(struct sv_verify *v, const struct sv_field *f,
		       u32 want)
{
	const struct sv_io *io = v->io;
	u32 val = 0;
	int ret;
	bool ok;

	ret = io->rd(io->ctx, f->reg, &val);
	ok = !ret && (val & f->mask) == want;
	if (v->out)
		v->out->print(v->out->ctx,
			      "%-16s %#06x = %#010x field %#010x = %#x expect %#x %s\n",
			      f->name, f->reg, val, f->mask, val & f->mask,
			      want, ret ? "READ ERROR" : ok ? "ok" : "MISMATCH");
	if (ok)
		return;
	sv_v_bad(v, f->reg, ret);
	if (!v->pfx)
		return;
	if (ret)
		sv_log(io, v->lvl, "%s%s %#06x read failed: %d\n", v->pfx,
		       f->name, f->reg, ret);
	else
		sv_log(io, v->lvl,
		       "%s%s %#06x reads %#010x: field %#x is %#x, expected %#x\n",
		       v->pfx, f->name, f->reg, val, f->mask, val & f->mask,
		       want);
}

static void sv_v_entries(struct sv_verify *v, bool armed)
{
	const struct sv_io *io = v->io;
	u32 raw, mask, want;
	int ret, p;
	bool ok, own;
	u16 vid;

	for (vid = RTL8_4S_VID_RSV_FIRST; vid <= RTL8_4S_VID_RSV_LAST; vid++) {
		p = sv_vid_port(v->cfg, vid);
		own = p >= 0;
		if (armed && own) {
			mask = RTL837X_VLAN_ENTRY_FIELDS;
			want = sv_entry_word(BIT(p) | BIT(v->cfg->cpu), BIT(p));
		} else {
			mask = RTL837X_VLAN_ENTRY_USED;
			want = 0;
		}
		raw = 0;
		ret = io->entry_rd(io->ctx, vid, &raw);
		ok = !ret && (raw & mask) == want;
		if (v->out && (own || !ok))
			v->out->print(v->out->ctx,
				      "vlan %u = %#010x expect %s %s\n", vid,
				      raw, armed && own ? "armed" : "empty",
				      ret ? "READ ERROR" : ok ? "ok" : "MISMATCH");
		if (ok)
			continue;
		sv_v_bad(v, SV_TOK_VID | vid, ret);
		if (!v->pfx)
			continue;
		if (ret)
			sv_log(io, v->lvl, "%sVLAN %u entry read failed: %d\n",
			       v->pfx, vid, ret);
		else
			sv_log(io, v->lvl,
			       "%sVLAN %u entry %#010x, expected %s %#x under %#x\n",
			       v->pfx, vid, raw, armed && own ? "armed" : "empty",
			       want, mask);
	}
}

static void sv_v_c2s(struct sv_verify *v)
{
	const struct sv_io *io = v->io;
	unsigned int i, set = 0, errs = 0, first = 0;
	u32 val, first_val = 0;
	int ret, first_err = 0;

	for (i = 0; i < RTL837X_SVLAN_C2S_WORDS; i++) {
		val = 0;
		ret = io->rd(io->ctx, RTL837X_SVLAN_C2S(i), &val);
		if (!ret && !val)
			continue;
		if (!set && !errs) {
			first = i;
			first_val = val;
			first_err = ret;
		}
		if (ret)
			errs++;
		else
			set++;
		sv_v_bad(v, SV_TOK_C2S | i, ret);
	}
	if (v->out)
		v->out->print(v->out->ctx,
			      "c2s %u words: %u set, %u read errors (first %#06x = %#x)\n",
			      RTL837X_SVLAN_C2S_WORDS, set, errs,
			      set || errs ? RTL837X_SVLAN_C2S(first) : 0,
			      first_val);
	if ((set || errs) && v->pfx)
		sv_log(io, v->lvl,
		       "%sC2S: %u words set, %u read errors; first %#06x = %#x (%d)\n",
		       v->pfx, set, errs, RTL837X_SVLAN_C2S(first), first_val,
		       first_err);
}

/* Returns true when everything covered matches @armed. */
static bool sv_verify_run(struct sv_verify *v, bool armed, unsigned int what)
{
	const struct sv_io *io = v->io;
	struct sv_field f;
	unsigned int i;
	u32 val = 0;
	int ret;

	if (what & SVV_FIXED) {
		for (i = 0; i < SVX_NUM; i++) {
			sv_xfield(v->cfg, i, &f);
			sv_v_field(v, &f, f.off);
		}
	}
	if (what & SVV_SBLOCK) {
		for (i = 0; i < SVF_NUM; i++) {
			sv_sfield(v->cfg, i, &f);
			sv_v_field(v, &f, armed ? f.armed : f.off);
		}
	}
	if (what & SVV_C2S)
		sv_v_c2s(v);
	if (what & SVV_ENTRIES)
		sv_v_entries(v, armed);
	if (what & SVV_TRAP_CTRL) {
		/* Logged only. */
		ret = io->rd(io->ctx, RTL837X_SVLAN_TRAP_CTRL, &val);
		if (v->out)
			v->out->print(v->out->ctx,
				      "trap control     %#06x = %#010x (logged only%s)\n",
				      RTL837X_SVLAN_TRAP_CTRL, val,
				      ret ? ", READ ERROR" : "");
		else
			sv_log(io, SV_LOG_DBG, "svlan: %#06x = %#x (%d)\n",
			       RTL837X_SVLAN_TRAP_CTRL, val, ret);
	}
	return !v->nbad;
}

/* One field write and its masked read-back. */
static int sv_write_check(const struct sv_io *io, const struct sv_field *f,
			  u32 val)
{
	u32 now = 0;
	int ret;

	ret = io->wr(io->ctx, f->reg, f->mask, val);
	if (!ret)
		ret = io->rd(io->ctx, f->reg, &now);
	if (!ret && (now & f->mask) != val)
		ret = -EIO;
	if (ret)
		sv_log(io, SV_LOG_ERR,
		       "svlan: %s: %s %#06x field %#x <- %#x failed: %d (reads %#010x)\n",
		       sv_step_name(f->step), f->name, f->reg, f->mask, val, ret,
		       now);
	return ret;
}

/*
 * X3/X4 and the setup uplink: up to SV_ATTEMPTS writes, SV_ATTEMPT_GAP_MS
 * apart. An attempt whose read-back shows the value succeeds whatever the
 * write returned. Returns the last error, -EIO for a value that stayed.
 * The callers log (a retry every 2 s must not flood the log).
 */
static int sv_clear(const struct sv_io *io, u32 reg, u32 mask, u32 val)
{
	int i, ret = 0, wret;
	u32 now;

	for (i = 0; i < SV_ATTEMPTS; i++) {
		if (i)
			io->sleep_ms(io->ctx, SV_ATTEMPT_GAP_MS);
		wret = io->wr(io->ctx, reg, mask, val);
		now = 0;
		ret = io->rd(io->ctx, reg, &now);
		if (!ret && (now & mask) == val)
			return 0;
		if (!ret)
			ret = wret ?: -EIO;
	}
	return ret;
}

/*
 * X5: the S fields that are inert while the uplink is 0. Every write is
 * attempted, then the whole block is read back.
 */
static bool sv_off_block(const struct sv_io *io, const struct sv_cfg *cfg,
			 const char *pfx)
{
	static const u8 order[] = {
		SVF_CTRL, SVF_UNTAG, SVF_PORT(0), SVF_PORT(1), SVF_PORT(2),
		SVF_PORT(3), SVF_PORT(4), SVF_TPID,
	};
	struct sv_verify v = { .io = io, .cfg = cfg, .pfx = pfx,
			       .lvl = SV_LOG_WARN };
	struct sv_field f;
	unsigned int i;
	int ret;

	for (i = 0; i < SV_ARRAY_SIZE(order); i++) {
		sv_sfield(cfg, order[i], &f);
		ret = io->wr(io->ctx, f.reg, f.mask, f.off);
		if (ret && pfx)
			sv_log(io, SV_LOG_WARN, "%s%s %#06x write failed: %d\n",
			       pfx, f.name, f.reg, ret);
	}
	for (i = 0; i < SV_ARRAY_SIZE(order); i++) {
		sv_sfield(cfg, order[i], &f);
		sv_v_field(&v, &f, f.off);
	}
	return !v.nbad;
}

static void sv_note(struct sv_seq *s, unsigned int step, int err)
{
	s->last_step = step;
	s->last_err = err;
}

static void sv_set_state(struct sv_seq *s, const struct sv_io *io, u8 sw)
{
	SV_SET_STATE(s, sw);
	io->sw_set(io->ctx, sw);
}

static void sv_accept(struct sv_seq *s, const struct sv_io *io, u8 accept)
{
	s->accept = accept;
	io->set_accept(io->ctx, accept, s->s_ports, s->disarm_at);
}

static void sv_fault_names(u8 fault, const char **a, const char **b,
			   const char **c, const char **d)
{
	*a = fault & SV_FAULT_DISARM_INCOMPLETE ? " disarm_incomplete" : "";
	*b = fault & SV_FAULT_OFF_INERT ? " off_inert" : "";
	*c = fault & SV_FAULT_ENTRIES ? " entries" : "";
	*d = fault & SV_FAULT_FORCED_PPE ? " forced_ppe" : "";
}

/*
 * X8, and the cleanup of a failed A1: every entry in entries_written back
 * to canonical(0, 0), verified, then every driver-owned entry read back
 * empty. A bit stays set for an entry that may still be in hardware.
 */
static int sv_entries_remove(struct sv_seq *s, const struct sv_io *io,
			     const struct sv_cfg *cfg)
{
	u16 vids[SV_MAX_ENTRIES];
	unsigned int i, n;
	int ret = 0, err;
	u32 raw;
	u16 vid;

	for (vid = RTL8_4S_VID_RSV_FIRST; vid <= RTL8_4S_VID_RSV_LAST; vid++) {
		if (!(s->entries_written & sv_vid_bit(vid)))
			continue;
		err = io->entry_wr(io->ctx, vid, 0, 0);
		if (err) {
			sv_log(io, SV_LOG_ERR,
			       "svlan: entry removal failed: VID %u: %d\n", vid,
			       err);
			ret = ret ?: err;
			continue;
		}
		s->entries_written &= ~sv_vid_bit(vid);
	}
	n = sv_entry_vids(cfg, vids);
	for (i = 0; i < n; i++) {
		raw = 0;
		err = io->entry_rd(io->ctx, vids[i], &raw);
		if (!err && (raw & RTL837X_VLAN_ENTRY_USED))
			err = -EIO;
		if (!err)
			continue;
		sv_log(io, SV_LOG_ERR,
		       "svlan: entry removal failed: VID %u: %d (reads %#010x)\n",
		       vids[i], err, raw);
		s->entries_written |= sv_vid_bit(vids[i]);
		ret = ret ?: err;
	}
	return ret;
}

/*
 * X3/X4 failed: FAULT disarm_incomplete with the accept the caller chose,
 * and, unless the switch is going away, the automatic retry: every
 * SV_RETRY_PERIOD_S until SV_RETRY_WINDOW_S after the first failure, then
 * every SV_RETRY_SLOW_S, without a log line, until a disarm verifies. Any
 * disarm other than a retry (debugfs, the deadline, a rollback) opens a new
 * window.
 */
static int sv_incomplete(struct sv_seq *s, const struct sv_io *io,
			 const struct sv_cfg *cfg, enum sv_trigger trigger,
			 unsigned int step, int err)
{
	unsigned long now, period = SV_RETRY_PERIOD_S;

	s->fault |= SV_FAULT_DISARM_INCOMPLETE;
	SV_SET_INCOMPLETE(s, true);
	sv_note(s, step, err);
	sv_set_state(s, io, RTL8_4S_SW_FAULT);
	/* A failed retry is only counted (disarm_retry, last_err). */
	if (trigger != SV_TRIG_RETRY)
		sv_log(io, SV_LOG_ERR,
		       "svlan: disarm incomplete at %s (accept %s): %d\n",
		       sv_step_name(step), sv_accept_name(s->accept), err);
	/*
	 * ctl_stop has cancelled the work for good: never queue it again (it
	 * would run on freed priv), and leave no retry behind.
	 */
	if (trigger == SV_TRIG_REMOVE || trigger == SV_TRIG_SHUTDOWN) {
		s->retry_until = 0;
		s->retry_slow = false;
		return err;
	}

	now = io->jiffies(io->ctx);
	if (trigger != SV_TRIG_RETRY || !s->retry_until) {
		s->retry_until = now + (unsigned long)SV_RETRY_WINDOW_S * cfg->hz;
		if (!s->retry_until)
			s->retry_until = 1;
		s->retry_slow = false;
	} else if (!sv_before(now, s->retry_until)) {
		if (!s->retry_slow)
			sv_log(io, SV_LOG_ERR,
			       "svlan: disarm retry gave up after %d s; retrying every %d s\n",
			       SV_RETRY_WINDOW_S, SV_RETRY_SLOW_S);
		s->retry_slow = true;
		period = SV_RETRY_SLOW_S;
	}
	io->timer(io->ctx, true, period * cfg->hz);
	return err;
}

/*
 * The deadline work waits at most SV_DEADLINE_STEP_S at a time and re-queues
 * itself for the rest: the timer wheel fires a delay late by up to its
 * level's granularity (HZ 100: 5.12 s at 120-300 s, 40.96 s at 600-1800 s,
 * 43.7 min at 86400 s), but a delay of 30 s or less by 0.64 s at most.
 */
static unsigned long sv_deadline_step(const struct sv_cfg *cfg,
				      unsigned long left)
{
	unsigned long step = (unsigned long)SV_DEADLINE_STEP_S * cfg->hz;

	return left < step ? left : step;
}

/* X2-X10 (the RETRY trigger starts here). */
static int sv_disarm_run(struct sv_seq *s, const struct sv_io *io,
			 const struct sv_cfg *cfg, enum sv_trigger trigger)
{
	bool hot = trigger != SV_TRIG_REMOVE && trigger != SV_TRIG_SHUTDOWN;
	const char *fa, *fb, *fc, *fd;
	u8 pending = 0;
	int ret, err = 0;
	unsigned int p;
	u32 now = 0;
	u64 t0;

	/* X2 */
	sv_accept(s, io, RTL8_4S_ACCEPT_NONE);
	io->sync(io->ctx);
	t0 = io->ms(io->ctx);

	/* X3: the uplink, first and alone. */
	ret = sv_clear(io, RTL837X_SVLAN_UPLINK, RTL837X_SVLAN_UPLINK_PORTS, 0);
	if (ret) {
		/*
		 * Still reading BIT(cpu), with nothing written since A9
		 * verified the armed switch: deliver S again. A ROLLBACK never
		 * has armed_intact set (its A9 may have found a wrong field).
		 */
		if (!io->rd(io->ctx, RTL837X_SVLAN_UPLINK, &now)) {
			if ((now & RTL837X_SVLAN_UPLINK_PORTS) != BIT(cfg->cpu))
				s->armed_intact = false;
			else if (s->armed_intact && trigger != SV_TRIG_ROLLBACK)
				sv_accept(s, io, RTL8_4S_ACCEPT_S);
		}
		s->none_ms = (u32)(io->ms(io->ctx) - t0);
		return sv_incomplete(s, io, cfg, trigger, SV_STEP_X3, ret);
	}

	/*
	 * X4: from here on the switch is no longer as A9 left it, and S
	 * steering a forced path kept is off.
	 */
	s->armed_intact = false;
	SV_SET_FORCED_LIVE(s, false);
	ret = sv_clear(io, RTL837X_CPU_TAG_CTRL, RTL837X_CPU_TAG_INSERT_MODE,
		       sv_prep(RTL837X_CPU_TAG_INSERT_MODE,
			       RTL837X_CPU_TAG_INSERT_ALL));
	if (ret) {
		/* Uplink 0 with insert mode TRAPPING delivers raw data. */
		s->none_ms = (u32)(io->ms(io->ctx) - t0);
		return sv_incomplete(s, io, cfg, trigger, SV_STEP_X4, ret);
	}
	SV_SET_INCOMPLETE(s, false);

	/* X5: inert while the uplink is 0; a mismatch does not stop the run. */
	if (!sv_off_block(io, cfg, "svlan: X5: ")) {
		pending |= SV_FAULT_OFF_INERT;
		err = -EIO;
		sv_note(s, SV_STEP_X5, err);
	}

	/* X6 */
	io->sleep_ms(io->ctx, SV_DRAIN_MS);
	io->sync(io->ctx);

	/* X7 */
	s->s_ports = 0;
	s->disarm_at = 0;
	sv_accept(s, io, RTL8_4S_ACCEPT_8899);
	s->none_ms = (u32)(io->ms(io->ctx) - t0);
	io->timer(io->ctx, false, 0);
	s->retry_until = 0;
	s->retry_slow = false;
	s->yield_logged = false;

	/* X8, X9: REMOVE and SHUTDOWN leave them to the next setup's reset. */
	if (hot) {
		ret = sv_entries_remove(s, io, cfg);
		if (ret) {
			pending |= SV_FAULT_ENTRIES;
			err = err ?: ret;
			sv_note(s, SV_STEP_X8, ret);
		}
		for (p = 0; p < RTL8_4S_NUM_PORTS; p++)
			if (cfg->user_ports & BIT(p))
				io->fast_age(io->ctx, p);
	}

	/* X10 */
	s->fault |= pending;
	sv_set_state(s, io, s->fault ? RTL8_4S_SW_FAULT : RTL8_4S_SW_OFF);
	sv_log(io, SV_LOG_INFO, "svlan: disarmed (%s) none_ms %u\n",
	       sv_trigger_name(trigger), s->none_ms);
	if (s->fault) {
		sv_fault_names(s->fault, &fa, &fb, &fc, &fd);
		sv_log(io, SV_LOG_WARN, "svlan: FAULT latched:%s%s%s%s\n", fa,
		       fb, fc, fd);
	}
	return err;
}

/*
 * F1 and the isolate cpu restore: the CPU port's isolation word to @val, up
 * to SV_ATTEMPTS tries SV_ATTEMPT_GAP_MS apart; the first try reads the
 * word into *was first, when @was is set.
 */
static int sv_cpu_word(const struct sv_io *io, const struct sv_cfg *cfg,
		       u32 *was, u32 val)
{
	int i, ret = 0;

	for (i = 0; i < SV_ATTEMPTS; i++) {
		if (i)
			io->sleep_ms(io->ctx, SV_ATTEMPT_GAP_MS);
		ret = io->cpu_word(io->ctx, cfg->cpu, i ? NULL : was, val);
		if (!ret)
			return 0;
	}
	return ret;
}

/* F2: the PHYs of @mask to low power, read back, as sv_cpu_word() tries. */
static int sv_phy_down(const struct sv_io *io, u16 mask)
{
	int i, ret = 0;

	for (i = 0; i < SV_ATTEMPTS; i++) {
		if (i)
			io->sleep_ms(io->ctx, SV_ATTEMPT_GAP_MS);
		ret = io->phy_down(io->ctx, mask);
		if (!ret)
			return 0;
	}
	return ret;
}

/*
 * WP3-DESIGN §2.2: a disarm that met a PPE claim, on REMOVE, SHUTDOWN or
 * "disarm force". S steering is kept: a PPE that still emits S-R frames is
 * then either delivered correctly (steering armed) or blocked (the CPU port
 * isolated, the S-R PHYs down). Nothing here writes the S block, the C2S
 * table or a VLAN entry; accept, s_ports, disarm_at and armed_intact stay.
 * Returns 0 when either barrier verified, else -EIO.
 */
static int sv_forced(struct sv_seq *s, const struct sv_io *io,
		     const struct sv_cfg *cfg, enum sv_trigger trigger,
		     unsigned int flags, u8 ppe)
{
	u16 m = s->s_ports & cfg->phy_ports;
	const char *phy;
	int f1, f2 = 0;
	u64 t0;

	/*
	 * F0: sw FAULT, the ppe byte kept. Offers decline and no claim or
	 * move succeeds from here on; the PPE disarms on its own.
	 */
	t0 = io->us(io->ctx);
	s->forced_n++;
	s->forced_trig = trigger;
	s->forced_ppe = ppe;
	s->forced_iso = 0;
	s->forced_cpu_was = SV_WORD_UNREAD;
	sv_set_state(s, io, RTL8_4S_SW_FAULT);
	if (trigger == SV_TRIG_REMOVE)
		s->bind_latch = true;
	if (trigger != SV_TRIG_DEBUGFS)
		io->warn(io->ctx);
	/* A pending isolate cpu restore would open the CPU port again. */
	if (s->iso_bench_live) {
		s->iso_bench_live = false;
		io->iso_timer(io->ctx, false, 0);
	}

	/* F1: the CPU port reaches no port. */
	f1 = sv_cpu_word(io, cfg, &s->forced_cpu_was, 0);
	if (!f1)
		s->forced_iso |= SV_ISO_CPU_OK;
	else
		sv_note(s, SV_STEP_F1, f1);

	/* F2: the S-R PHYs down (vlan_lock released). */
	if ((flags & SV_DISARM_NOPHY) || !m) {
		s->forced_iso |= SV_ISO_PHY_SKIPPED;
	} else {
		f2 = sv_phy_down(io, m);
		if (!f2)
			s->forced_iso |= SV_ISO_PHY_OK;
		else
			sv_note(s, SV_STEP_F2, f2);
	}

	/* F3 */
	s->fault |= SV_FAULT_FORCED_PPE;
	SV_SET_FORCED_LIVE(s, true);
	s->iso_failed = !(s->forced_iso & (SV_ISO_CPU_OK | SV_ISO_PHY_OK));
	/* The follow-up; ctl_stop has cancelled the work for REMOVE/SHUTDOWN. */
	if (trigger == SV_TRIG_DEBUGFS)
		io->timer(io->ctx, true,
			  (unsigned long)SV_YIELD_PERIOD_S * cfg->hz);

	/* F4 */
	s->forced_us = (u32)(io->us(io->ctx) - t0);
	if (s->forced_iso & SV_ISO_PHY_SKIPPED)
		phy = "skipped";
	else
		phy = f2 ? "FAILED" : "down";
	sv_log(io, SV_LOG_ERR,
	       "svlan: forced (%s): ppe %s, CPU port %u isolation %s (was %#x), PHYs %#x %s; S steering kept\n",
	       sv_trigger_name(trigger), sv_ppe_name(ppe), cfg->cpu,
	       f1 ? "FAILED" : "ok", s->forced_cpu_was, m, phy);
	if (!s->iso_failed)
		return 0;
	sv_log(io, SV_LOG_ERR, "svlan: forced isolation FAILED (%s)\n",
	       sv_trigger_name(trigger));
	return -EIO;
}

int sv_seq_disarm(struct sv_seq *s, const struct sv_io *io,
		  const struct sv_cfg *cfg, enum sv_trigger trigger,
		  unsigned int flags)
{
	bool hard = trigger == SV_TRIG_REMOVE || trigger == SV_TRIG_SHUTDOWN ||
		    (trigger == SV_TRIG_DEBUGFS && (flags & SV_DISARM_FORCE));
	u8 ppe = RTL8_4S_PPE_OFF;

	/* The follow-up runs only from the deadline work (sv_seq_work()). */
	if (trigger >= SV_TRIG_NUM || trigger == SV_TRIG_FORCED_FOLLOWUP)
		return -EINVAL;
	if ((flags & SV_DISARM_FORCE) && trigger == SV_TRIG_DEBUGFS)
		s->force_requests++;
	s->disarm[trigger]++;

	/* X0 */
	if (s->state == RTL8_4S_SW_OFF && !s->entries_written)
		return 0;

	/*
	 * X1: steering is cleared only from ppe OFF (cmpxchg). Over a claim,
	 * REMOVE, SHUTDOWN and force isolate instead (WP3-DESIGN D1).
	 */
	if (trigger != SV_TRIG_RETRY && !io->sw_begin_disarm(io->ctx, &ppe)) {
		s->last_ppe = ppe;
		if (hard)
			return sv_forced(s, io, cfg, trigger, flags, ppe);
		/* The deadline work logs its yield once. */
		if (trigger != SV_TRIG_DEADLINE || !s->yield_logged)
			sv_log(io, SV_LOG_ERR,
			       "svlan: disarm refused: PPE S-R state %u\n", ppe);
		return -EBUSY;
	}
	if (trigger != SV_TRIG_RETRY)
		SV_SET_STATE(s, RTL8_4S_SW_DISARMING);
	return sv_disarm_run(s, io, cfg, trigger);
}

void sv_seq_work(struct sv_seq *s, const struct sv_io *io,
		 const struct sv_cfg *cfg)
{
	unsigned long now = io->jiffies(io->ctx);
	u8 ppe = RTL8_4S_PPE_OFF;
	int ret;

	/* Retry mode: FAULT disarm_incomplete, from X2. */
	if (s->state == RTL8_4S_SW_FAULT && s->incomplete && s->retry_until) {
		s->disarm[SV_TRIG_RETRY]++;
		sv_disarm_run(s, io, cfg, SV_TRIG_RETRY);
		return;
	}

	/*
	 * Follow-up mode (WP3-DESIGN §2.2): a debugfs forced path kept S
	 * steering. Once the PPE has released, the same begin_disarm as X1
	 * succeeds and X2-X10 clear steering; until then, look again every
	 * SV_YIELD_PERIOD_S without a log line. The FAULT stays latched.
	 */
	if (s->state == RTL8_4S_SW_FAULT && s->forced_live) {
		s->disarm[SV_TRIG_FORCED_FOLLOWUP]++;
		if (!io->sw_begin_disarm(io->ctx, &ppe)) {
			s->last_ppe = ppe;
			io->timer(io->ctx, true,
				  (unsigned long)SV_YIELD_PERIOD_S * cfg->hz);
			return;
		}
		s->followup_n++;
		SV_SET_STATE(s, RTL8_4S_SW_DISARMING);
		ret = sv_disarm_run(s, io, cfg, SV_TRIG_FORCED_FOLLOWUP);
		sv_log(io, SV_LOG_WARN, "svlan: forced follow-up disarm %d\n",
		       ret);
		return;
	}

	/* Deadline mode. A work left from an earlier arm re-queues itself. */
	if (s->state != RTL8_4S_SW_READY || !s->disarm_at)
		return;
	if (sv_before(now, s->disarm_at)) {
		io->timer(io->ctx, true, sv_deadline_step(cfg, s->disarm_at - now));
		return;
	}
	if (!s->yield_logged)
		sv_log(io, SV_LOG_INFO, "svlan: deadline reached, disarming\n");
	ret = sv_seq_disarm(s, io, cfg, SV_TRIG_DEADLINE, 0);
	if (ret != -EBUSY || s->state != RTL8_4S_SW_READY)
		return;
	if (!s->yield_logged) {
		sv_log(io, SV_LOG_ERR,
		       "svlan: deadline yields to PPE S-R state %u\n",
		       s->last_ppe);
		s->yield_logged = true;
	}
	io->timer(io->ctx, true, (unsigned long)SV_YIELD_PERIOD_S * cfg->hz);
}

static int sv_renew_refuse(struct sv_seq *s, const struct sv_io *io,
			   unsigned int cause, int err, const char *why)
{
	s->renew_refused[cause]++;
	sv_log(io, SV_LOG_INFO, "svlan: renew refused (%s)\n", why);
	return err;
}

/*
 * WP3-DESIGN §2.5: the lease. Memory only, no register: the new deadline
 * is published to the tagger (td->disarm_at, which the PPE caps its own
 * deadline by) and the deadline work moved. Renewing under a claim is the
 * point; it only ever moves the deadline later.
 */
int sv_seq_renew(struct sv_seq *s, const struct sv_io *io,
		 const struct sv_cfg *cfg, unsigned int secs)
{
	unsigned long now, at;

	if (secs < SV_DEADLINE_MIN_S || secs > SV_DEADLINE_MAX_S)
		return sv_renew_refuse(s, io, SV_RENEW_RANGE, -EINVAL, "range");
	if (s->state != RTL8_4S_SW_READY)
		return sv_renew_refuse(s, io, SV_RENEW_STATE, -ENOTCONN,
				       "state");
	now = io->jiffies(io->ctx);
	at = now + (unsigned long)secs * cfg->hz;
	if (sv_before(at, s->disarm_at))
		return sv_renew_refuse(s, io, SV_RENEW_SHORTER, -ERANGE,
				       "shorter");
	s->disarm_at = at ?: 1;
	/* READY: accept is S; s_ports unchanged, disarm_at released with it. */
	sv_accept(s, io, RTL8_4S_ACCEPT_S);
	s->yield_logged = false;
	io->timer(io->ctx, true, sv_deadline_step(cfg, s->disarm_at - now));
	s->renew_ok++;
	return 0;
}

/* The isolate cpu restore: the word read before the bench write, back. */
static void sv_iso_bench_restore(struct sv_seq *s, const struct sv_io *io,
				 const struct sv_cfg *cfg)
{
	int ret;

	s->iso_bench_live = false;
	ret = sv_cpu_word(io, cfg, NULL, s->iso_bench_was);
	if (ret)
		sv_log(io, SV_LOG_ERR,
		       "svlan: isolate cpu restore FAILED: CPU port %u word %#x: %d\n",
		       cfg->cpu, s->iso_bench_was, ret);
	else
		sv_log(io, SV_LOG_INFO, "svlan: isolate cpu restored (%#x)\n",
		       s->iso_bench_was);
}

/*
 * WP3-DESIGN §2.6, a bench verb (I4-S2a): the CPU port isolated for @secs
 * while the PPE is ACTIVE, nothing else written and no state changed, so
 * that a PPE frame met by the isolation alone can be looked for. A forced
 * path in the window cancels the restore (sv_forced()); ctl_stop cancels
 * the work, as REMOVE and SHUTDOWN isolate anyway.
 */
int sv_seq_iso_bench(struct sv_seq *s, const struct sv_io *io,
		     const struct sv_cfg *cfg, unsigned int secs, u8 ppe)
{
	u32 was = SV_WORD_UNREAD;
	int ret;

	if (secs < SV_ISO_BENCH_MIN_S || secs > SV_ISO_BENCH_MAX_S) {
		sv_log(io, SV_LOG_INFO,
		       "svlan: isolate cpu refused (range): %u..%u s\n",
		       SV_ISO_BENCH_MIN_S, SV_ISO_BENCH_MAX_S);
		return -EINVAL;
	}
	if (s->state != RTL8_4S_SW_READY || ppe != RTL8_4S_PPE_ACTIVE) {
		sv_log(io, SV_LOG_INFO,
		       "svlan: isolate cpu refused (state): sw %s, ppe %s\n",
		       sv_sw_state_name(s->state), sv_ppe_name(ppe));
		return -ENOTCONN;
	}
	if (s->iso_bench_live) {
		sv_log(io, SV_LOG_INFO, "svlan: isolate cpu refused (busy)\n");
		return -EBUSY;
	}
	s->iso_bench_n++;
	/* One try: a failure is the result, after the restore. */
	ret = io->cpu_word(io->ctx, cfg->cpu, &was, 0);
	/* A word not read is restored to what setup wrote. */
	s->iso_bench_was = was == SV_WORD_UNREAD ? cfg->user_mask : was;
	if (ret) {
		sv_log(io, SV_LOG_ERR, "svlan: isolate cpu %u: FAILED (%d)\n",
		       secs, ret);
		sv_iso_bench_restore(s, io, cfg);
		return -EIO;
	}
	s->iso_bench_live = true;
	io->iso_timer(io->ctx, true, (unsigned long)secs * cfg->hz);
	sv_log(io, SV_LOG_INFO, "svlan: isolate cpu %u: ok (was %#x)\n", secs,
	       was);
	return 0;
}

void sv_seq_iso_restore(struct sv_seq *s, const struct sv_io *io,
			const struct sv_cfg *cfg)
{
	if (s->iso_bench_live)
		sv_iso_bench_restore(s, io, cfg);
}

bool sv_seq_quiesce_keep_s(const struct sv_seq *s)
{
	return SV_ONCE(s->forced_live);
}

int sv_latch_check(struct sv_latch *l)
{
	if (!SV_ONCE(l->latched))
		return 0;
	l->refused++;
	return -EBUSY;
}

void sv_latch_note(struct sv_latch *l, const struct sv_seq *s)
{
	if (s->bind_latch)
		SV_ONCE(l->latched) = true;
}

static int sv_refuse(struct sv_seq *s, const struct sv_io *io,
		     unsigned int cause, int err, const char *why)
{
	s->refused[cause]++;
	sv_log(io, SV_LOG_ERR, "svlan: arm refused (%s): %s\n",
	       sv_refusal_name(cause), why);
	return err;
}

/* §4.3 refusals 1-6, in order; the register prechecks last. */
static int sv_arm_refusals(struct sv_seq *s, const struct sv_io *io,
			   const struct sv_cfg *cfg,
			   const struct sv_arm_req *req)
{
	struct sv_verify v = { .io = io, .cfg = cfg,
			       .pfx = "svlan: arm refused (precheck): ",
			       .lvl = SV_LOG_ERR };
	const struct sv_port_info *pi;
	const char *why;
	bool bad = false;
	unsigned int p;

	if (!req->enabled)
		return sv_refuse(s, io, SV_REF_DISABLED, -EOPNOTSUPP,
				 "S mode is off (svlan=0 at bind)");
	if (!req->ctl_ready || !req->running)
		return sv_refuse(s, io, SV_REF_SHUTDOWN, -ESHUTDOWN,
				 "the switch is going away");
	if (req->deadline_s < SV_DEADLINE_MIN_S ||
	    req->deadline_s > SV_DEADLINE_MAX_S)
		return sv_refuse(s, io, SV_REF_DEADLINE, -EINVAL,
				 "the deadline must be 30..86400 s");
	switch (s->state) {
	case RTL8_4S_SW_OFF:
		break;
	case RTL8_4S_SW_FAULT:
		return sv_refuse(s, io, SV_REF_FAULT, -EIO,
				 "FAULT latched until the next bind");
	default:
		return sv_refuse(s, io, SV_REF_ALREADY, -EALREADY,
				 "already armed or arming");
	}
	if (!req->td_present)
		return sv_refuse(s, io, SV_REF_NO_TAGGER, -ENODEV,
				 "rtl8_4s is not bound");
	if (req->offload_disabled)
		return sv_refuse(s, io, SV_REF_OFFLOAD, -EBUSY,
				 req->offload_disabled);

	for (p = 0; p < RTL8_4S_NUM_PORTS; p++) {
		if (!(cfg->user_ports & BIT(p)))
			continue;
		pi = &req->port[p];
		if (!pi->bridged)
			why = "is not in the offloaded bridge";
		else if (!pi->aware)
			why = "is not VLAN-aware";
		else if (pi->upper_8021ad)
			why = "has an 802.1ad upper";
		else if (pi->upper_rsv_vid)
			why = "has an 8021q upper in 3900-3919";
		else
			continue;
		sv_log(io, SV_LOG_ERR, "svlan: arm refused (topology): port %u %s\n",
		       p, why);
		bad = true;
	}
	if (bad) {
		s->refused[SV_REF_TOPOLOGY]++;
		return -EBUSY;
	}

	if (req->shadow_vid) {
		sv_log(io, SV_LOG_ERR,
		       "svlan: arm refused (collision): VLAN %u has bridge members\n",
		       req->shadow_vid);
		bad = true;
	}
	if (req->pvid) {
		sv_log(io, SV_LOG_ERR,
		       "svlan: arm refused (collision): port %u has PVID %u\n",
		       req->pvid_port, req->pvid);
		bad = true;
	}
	if (bad) {
		s->refused[SV_REF_COLLISION]++;
		return -EBUSY;
	}

	/* Read only; accept is still 8899. */
	if (!sv_verify_run(&v, false, SVV_ALL)) {
		s->refused[SV_REF_PRECHECK]++;
		return v.rd_err ?: -EBUSY;
	}
	return 0;
}

/* A1: each entry is its own verified transaction. */
static int sv_arm_entries(struct sv_seq *s, const struct sv_io *io,
			  const struct sv_cfg *cfg)
{
	u16 vids[SV_MAX_ENTRIES];
	unsigned int i, n;
	int ret, p;

	n = sv_entry_vids(cfg, vids);
	for (i = 0; i < n; i++) {
		p = sv_vid_port(cfg, vids[i]);
		s->entries_written |= sv_vid_bit(vids[i]);
		ret = io->entry_wr(io->ctx, vids[i], BIT(p) | BIT(cfg->cpu),
				   BIT(p));
		if (ret) {
			sv_log(io, SV_LOG_ERR, "svlan: A1: VLAN %u not written: %d\n",
			       vids[i], ret);
			return ret;
		}
	}
	return 0;
}

int sv_seq_arm(struct sv_seq *s, const struct sv_io *io,
	       const struct sv_cfg *cfg, const struct sv_arm_req *req)
{
	struct sv_verify v = { .io = io, .cfg = cfg, .pfx = "svlan: A9: ",
			       .lvl = SV_LOG_ERR };
	const char *fa, *fb, *fc, *fd;
	unsigned int i, step = SV_STEP_NONE;
	unsigned long now, at;
	struct sv_field f;
	int ret, err;
	u64 t0;

	ret = sv_arm_refusals(s, io, cfg, req);
	if (ret)
		return ret;

	/* A0: claims need READY, so they fail from here on. */
	sv_set_state(s, io, RTL8_4S_SW_ARMING);
	s->entries_written = 0;
	s->armed_intact = false;
	s->yield_logged = false;

	/*
	 * A1, accept still 8899. Without the uplink the entries steer nothing,
	 * but they are C-VLANs too: port p can now reach the CPU with its own
	 * 39xx C-VID under the 8 byte tag, which the tagger drops (INNER_RSV),
	 * as it does while a FAULT leaves them.
	 */
	ret = sv_arm_entries(s, io, cfg);
	if (ret) {
		s->arm_fail++;
		sv_note(s, SV_STEP_A1, ret);
		err = sv_entries_remove(s, io, cfg);
		if (!err) {
			sv_set_state(s, io, RTL8_4S_SW_OFF);
			sv_log(io, SV_LOG_ERR,
			       "svlan: arm failed at A1: %d; rolled back\n", ret);
		} else {
			s->fault |= SV_FAULT_ENTRIES;
			sv_note(s, SV_STEP_X8, err);
			sv_set_state(s, io, RTL8_4S_SW_FAULT);
			sv_log(io, SV_LOG_ERR,
			       "svlan: arm failed at A1: %d; ROLLBACK FAILED (entries)\n",
			       ret);
		}
		return ret;
	}

	/* A2 */
	sv_accept(s, io, RTL8_4S_ACCEPT_NONE);
	io->sync(io->ctx);
	t0 = io->ms(io->ctx);

	/* A3-A8, the uplink last. */
	for (i = 0; i < SVF_NUM; i++) {
		sv_sfield(cfg, i, &f);
		ret = sv_write_check(io, &f, f.armed);
		if (ret) {
			step = f.step;
			goto rollback;
		}
	}

	/* A9 */
	if (!sv_verify_run(&v, true, SVV_FIXED | SVV_SBLOCK | SVV_C2S |
			   SVV_ENTRIES)) {
		ret = v.rd_err ?: -EIO;
		step = SV_STEP_A9;
		goto rollback;
	}

	/* A10: time-based only (K1). */
	io->sleep_ms(io->ctx, SV_DRAIN_MS);
	io->sync(io->ctx);

	/* A11 */
	now = io->jiffies(io->ctx);
	at = now + (unsigned long)req->deadline_s * cfg->hz;
	s->s_ports = cfg->user_ports;
	s->disarm_at = at ?: 1;
	sv_accept(s, io, RTL8_4S_ACCEPT_S);
	s->none_ms = (u32)(io->ms(io->ctx) - t0);
	sv_set_state(s, io, RTL8_4S_SW_READY);
	s->armed_intact = true;
	io->timer(io->ctx, true,
		  sv_deadline_step(cfg, (unsigned long)req->deadline_s * cfg->hz));
	s->arm_ok++;
	sv_log(io, SV_LOG_INFO,
	       "svlan: armed s_ports %#x deadline %us none_ms %u drain_ms %u\n",
	       s->s_ports, req->deadline_s, s->none_ms, SV_DRAIN_MS);
	return 0;

rollback:
	s->arm_fail++;
	sv_note(s, step, ret);
	err = sv_seq_disarm(s, io, cfg, SV_TRIG_ROLLBACK, 0);
	if (s->state == RTL8_4S_SW_OFF) {
		sv_log(io, SV_LOG_ERR, "svlan: arm failed at %s: %d; rolled back\n",
		       sv_step_name(step), ret);
	} else if (err == -EBUSY && s->state == RTL8_4S_SW_ARMING) {
		sv_log(io, SV_LOG_ERR,
		       "svlan: arm failed at %s: %d; ROLLBACK FAILED (refused)\n",
		       sv_step_name(step), ret);
	} else {
		sv_fault_names(s->fault, &fa, &fb, &fc, &fd);
		sv_log(io, SV_LOG_ERR,
		       "svlan: arm failed at %s: %d; ROLLBACK FAILED (fault%s%s%s%s)\n",
		       sv_step_name(step), ret, fa, fb, fc, fd);
	}
	return ret;
}

int sv_seq_off(const struct sv_io *io, const struct sv_cfg *cfg,
	       const char *what, bool *rest_ok)
{
	u32 w[SVF_NUM];
	struct sv_field f;
	unsigned int i;
	int up, ret, rd_err = 0;
	bool ok = true;

	up = sv_clear(io, RTL837X_SVLAN_UPLINK, RTL837X_SVLAN_UPLINK_PORTS, 0);
	if (sv_clear(io, RTL837X_CPU_TAG_CTRL, RTL837X_CPU_TAG_INSERT_MODE,
		     sv_prep(RTL837X_CPU_TAG_INSERT_MODE,
			     RTL837X_CPU_TAG_INSERT_ALL)))
		ok = false;
	if (!sv_off_block(io, cfg, NULL))
		ok = false;
	for (i = 0; i < SVF_NUM; i++) {
		sv_sfield(cfg, i, &f);
		w[i] = 0;
		ret = io->rd(io->ctx, f.reg, &w[i]);
		rd_err = rd_err ?: ret;
	}
	sv_log(io, up || !ok ? SV_LOG_WARN : SV_LOG_INFO,
	       "svlan: %s 57c0=%#x 57c4=%#x 57c8=%#x 57cc..57dc=%#x/%#x/%#x/%#x/%#x 6044=%#x 6720=%#x %s (uplink %d, read %d)\n",
	       what, w[SVF_UPLINK], w[SVF_CTRL], w[SVF_UNTAG], w[SVF_PORT(0)],
	       w[SVF_PORT(1)], w[SVF_PORT(2)], w[SVF_PORT(3)], w[SVF_PORT(4)],
	       w[SVF_TPID], w[SVF_INSERT], up || !ok ? "MISMATCH" : "match",
	       up, rd_err);
	*rest_ok = ok;
	return up;
}

void sv_seq_setup_fault(struct sv_seq *s)
{
	s->fault |= SV_FAULT_OFF_INERT;
	sv_note(s, SV_STEP_SETUP, -EIO);
	SV_SET_STATE(s, RTL8_4S_SW_FAULT);
}

void sv_seq_dump(const struct sv_io *io, const char *what)
{
	static const u32 regs[] = {
		RTL837X_SVLAN_UPLINK, RTL837X_SVLAN_CTRL,
		RTL837X_SVLAN_UNTAG_SVID, RTL837X_SVLAN_PORT_SVID(0),
		RTL837X_SVLAN_PORT_SVID(1), RTL837X_SVLAN_PORT_SVID(2),
		RTL837X_SVLAN_PORT_SVID(3), RTL837X_SVLAN_PORT_SVID(4),
		RTL837X_SVLAN_TPID, RTL837X_CPU_TAG_CTRL,
		RTL837X_SVLAN_TRAP_CTRL,
	};
	u32 w[SV_ARRAY_SIZE(regs)];
	unsigned int i;
	int ret, err = 0;

	for (i = 0; i < SV_ARRAY_SIZE(regs); i++) {
		w[i] = 0;
		ret = io->rd(io->ctx, regs[i], &w[i]);
		err = err ?: ret;
	}
	sv_log(io, SV_LOG_INFO,
	       "svlan: %s 57c0=%#x 57c4..57dc=%#x/%#x/%#x/%#x/%#x/%#x/%#x 6044=%#x 6720=%#x 4ec8=%#x (read %d)\n",
	       what, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9],
	       w[10], err);
}

bool sv_seq_report(const struct sv_io *io, const struct sv_cfg *cfg)
{
	struct sv_verify off = { .io = io, .cfg = cfg,
				 .pfx = "P0 readback S-VLAN: ",
				 .lvl = SV_LOG_WARN };
	struct sv_verify ent = off;
	bool a, b;

	a = sv_verify_run(&off, false, SVV_SBLOCK);
	b = sv_verify_run(&ent, false, SVV_ENTRIES);
	sv_log(io, a && b ? SV_LOG_INFO : SV_LOG_WARN,
	       "P0 readback S-VLAN block off %s, entries %u-%u empty %s\n",
	       a ? "PASS" : "MISMATCH", RTL8_4S_VID_RSV_FIRST,
	       RTL8_4S_VID_RSV_LAST, b ? "PASS" : "MISMATCH");
	return a && b;
}

bool sv_seq_may_be_live(const struct sv_seq *s)
{
	u8 state = SV_ONCE(s->state);

	if (state == RTL8_4S_SW_OFF)
		return false;
	if (state != RTL8_4S_SW_FAULT)
		return true;
	return SV_ONCE(s->incomplete) || SV_ONCE(s->forced_live);
}

bool sv_seq_expect_armed(const struct sv_seq *s)
{
	return s->state == RTL8_4S_SW_READY ||
	       (s->state == RTL8_4S_SW_FAULT && s->armed_intact &&
		(s->incomplete || s->forced_live));
}

int sv_seq_show(const struct sv_seq *s, const struct sv_io *io,
		const struct sv_cfg *cfg, const struct sv_out *out)
{
	struct sv_verify v = { .io = io, .cfg = cfg, .out = out };
	bool armed = sv_seq_expect_armed(s);
	struct sv_field f;
	unsigned int i, n;
	u32 t;

	/* Only setup restores the CPU port after F1 (WP3-DESIGN D4). */
	t = s->fault & SV_FAULT_FORCED_PPE ? 0 : cfg->user_mask;
	f = (struct sv_field){ RTL837X_PORT_ISOLATION_REG(cfg->cpu),
			       RTL837X_PORT_ISOLATION_MASK, t, t, SV_STEP_NONE,
			       "CPU isolation" };
	sv_verify_run(&v, armed, SVV_ALL);
	sv_v_field(&v, &f, t);
	if (!v.nbad) {
		out->print(out->ctx, "expect %s: match\n", armed ? "armed" : "off");
		return 0;
	}
	out->print(out->ctx, "expect %s: MISMATCH", armed ? "armed" : "off");
	n = v.nbad < SV_ARRAY_SIZE(v.bad) ? v.nbad : SV_ARRAY_SIZE(v.bad);
	for (i = 0; i < n; i++) {
		t = v.bad[i];
		if (t & SV_TOK_C2S)
			out->print(out->ctx, " c2s%u", t & 0xff);
		else if (t & SV_TOK_VID)
			out->print(out->ctx, " vlan%u", t & 0xfff);
		else
			out->print(out->ctx, " %#06x", t);
	}
	if (v.nbad > n)
		out->print(out->ctx, " (+%u)", v.nbad - n);
	out->print(out->ctx, "\n");
	return v.rd_err ?: -EIO;
}
