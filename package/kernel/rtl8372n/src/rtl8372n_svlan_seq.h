/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-R steering, switch side (WP1-DESIGN §4): the arm, disarm, retry,
 * deadline and off sequences, and the expected words, in a form a user-space
 * harness compiles. Nothing here calls the kernel: register and table I/O,
 * the tagger state word, time, sleeping, the deadline work and logging all
 * go through struct sv_io, and the caller holds the svlan lock (struct
 * rtl8372n_svlan in rtl837x.h) around every call that takes a struct sv_seq.
 *
 * <linux/dsa/rtl8_4s.h> is included whole or, when the including .c file
 * defines RTL8_4S_PURE first (rtl8372n_svlan_seq.c, the harness), its pure
 * part only. This header never defines RTL8_4S_PURE itself.
 */
#ifndef __RTL8372N_SVLAN_SEQ_H__
#define __RTL8372N_SVLAN_SEQ_H__

#include <linux/bits.h>
#include <linux/types.h>
#include <linux/dsa/rtl8_4s.h>

/* The kernel's compiler_attributes.h defines it; user space may not. */
#ifndef __printf
#define __printf(a, b)	__attribute__((format(printf, a, b)))
#endif

/* A10/X6: the time-based drain (synchronize_net() drains no ring). */
#define SV_DRAIN_MS		50
/* X3/X4 and the setup uplink: attempts, and the gap between them. */
#define SV_ATTEMPTS		3
#define SV_ATTEMPT_GAP_MS	10
/*
 * The automatic disarm retry after FAULT disarm_incomplete: every
 * SV_RETRY_PERIOD_S for SV_RETRY_WINDOW_S, then every SV_RETRY_SLOW_S until
 * a disarm verifies, so that no arm outlives its deadline for good.
 */
#define SV_RETRY_PERIOD_S	2
#define SV_RETRY_WINDOW_S	300
#define SV_RETRY_SLOW_S		60
/* A deadline that a PPE claim holds off is looked at again this often. */
#define SV_YIELD_PERIOD_S	5
/* The deadline work's longest wait: see sv_deadline_step(). */
#define SV_DEADLINE_STEP_S	30
/* D14: every arm has a deadline; 0 is refused. */
#define SV_DEADLINE_MIN_S	30
#define SV_DEADLINE_MAX_S	86400
/* The reserved range; entries_written has one bit per VID (VID - 3900). */
#define SV_NUM_VIDS		(RTL8_4S_VID_RSV_LAST - RTL8_4S_VID_RSV_FIRST + 1)
/* Driver-owned entries: one upload and one download VID per user port. */
#define SV_MAX_ENTRIES		(2 * RTL8_4S_NUM_PORTS)
/* The isolate cpu bench verb (WP3-DESIGN §2.6): 1..10 s. */
#define SV_ISO_BENCH_MIN_S	1
#define SV_ISO_BENCH_MAX_S	10
/* An isolation word that was not read (or whose read failed). */
#define SV_WORD_UNREAD		0xffffffffU

/*
 * sv_seq_disarm() flags (WP3-DESIGN §2.2). REMOVE and SHUTDOWN always take
 * the forced path over a PPE claim; the debugfs disarm only with
 * SV_DISARM_FORCE. SV_DISARM_NOPHY (the bench verb "disarm force nophy")
 * skips F2, the PHY power-down.
 */
#define SV_DISARM_FORCE		BIT(0)
#define SV_DISARM_NOPHY		BIT(1)

enum sv_trigger {
	SV_TRIG_DEBUGFS,
	SV_TRIG_DEADLINE,
	SV_TRIG_RETRY,
	SV_TRIG_REMOVE,
	SV_TRIG_SHUTDOWN,
	SV_TRIG_ROLLBACK,
	/* The deadline work, once the PPE is off after a debugfs forced path */
	SV_TRIG_FORCED_FOLLOWUP,
	SV_TRIG_NUM,
};

/* The arm refusals of §4.3, in the order they are checked. */
enum sv_refusal {
	SV_REF_DISABLED,	/* svlan=0: -EOPNOTSUPP */
	SV_REF_SHUTDOWN,	/* control window closed, or shutting down */
	SV_REF_DEADLINE,	/* outside 30..86400 s: -EINVAL */
	SV_REF_ALREADY,		/* READY or ARMING: -EALREADY */
	SV_REF_FAULT,		/* FAULT: -EIO */
	SV_REF_NO_TAGGER,	/* rtl8_4s not bound: -ENODEV */
	SV_REF_OFFLOAD,		/* bridge offload off: -EBUSY */
	SV_REF_TOPOLOGY,	/* a user port not ready for S: -EBUSY */
	SV_REF_COLLISION,	/* the bridge uses a reserved VID: -EBUSY */
	SV_REF_PRECHECK,	/* a register is not as recorded: -EBUSY */
	SV_REF_NUM,
};

/* FAULT reasons (§4.8), latched until the next bind. */
#define SV_FAULT_DISARM_INCOMPLETE	BIT(0)
#define SV_FAULT_OFF_INERT		BIT(1)
#define SV_FAULT_ENTRIES		BIT(2)
/* A disarm met a PPE claim and took the forced path (WP3-DESIGN §2.2). */
#define SV_FAULT_FORCED_PPE		BIT(3)

/* What the forced path verified (struct sv_seq forced_iso). */
#define SV_ISO_CPU_OK		BIT(0)	/* F1: the CPU port's word reads 0 */
#define SV_ISO_PHY_OK		BIT(1)	/* F2: every S-R PHY reads low power */
#define SV_ISO_PHY_SKIPPED	BIT(2)	/* F2 not run: nophy, or no PHY port */

/* The renew refusals of WP3-DESIGN §2.5. */
enum sv_renew_refusal {
	SV_RENEW_RANGE,		/* outside 30..86400 s: -EINVAL */
	SV_RENEW_STATE,		/* not READY: -ENOTCONN */
	SV_RENEW_SHORTER,	/* before the current deadline: -ERANGE */
	SV_RENEW_NUM,
};

enum sv_step {
	SV_STEP_NONE,
	SV_STEP_SETUP,
	SV_STEP_A1, SV_STEP_A3, SV_STEP_A4, SV_STEP_A5, SV_STEP_A6,
	SV_STEP_A7, SV_STEP_A8, SV_STEP_A9,
	SV_STEP_X3, SV_STEP_X4, SV_STEP_X5, SV_STEP_X8,
	SV_STEP_F1, SV_STEP_F2,
	SV_STEP_NUM,
};

enum sv_log_level {
	SV_LOG_ERR,
	SV_LOG_WARN,
	SV_LOG_INFO,
	SV_LOG_DBG,
};

/*
 * Every callback gets @ctx. Register values are positioned (as for
 * regmap_update_bits()); rd() returns the whole word. entry_wr() writes the
 * canonical entry and verifies it (rtl837x_vlan_entry_write()); entry_rd()
 * returns the raw data word. The tagger-side callbacks are no-ops (and
 * sw_begin_disarm() succeeds) while no rtl8_4s tagger_data is reachable.
 * set_accept() stores s_ports and disarm_at, then accept with release
 * semantics. timer() modifies (true) or cancels, without waiting, the
 * deadline work; @delay is in jiffies. iso_timer() does the same for the
 * isolate cpu restore work.
 *
 * cpu_word() (F1 and isolate cpu) takes vlan_lock: it reads the CPU port's
 * isolation word into *was (SV_WORD_UNREAD when that read fails; no read
 * when @was is NULL), writes @val, reads the word back, and returns 0 when
 * bits 9:0 read @val, else the read-back's error, the write's, or -EIO.
 * phy_down() (F2) writes PMA low power to the internal PHYs of @mask at once
 * and reads each back; it returns 0 when every one reads low power, else
 * the first read error, the write's, or -EIO. One try each: the sequences
 * retry. Neither touches the S block or a VLAN entry.
 */
struct sv_io {
	void *ctx;
	int (*rd)(void *ctx, u32 reg, u32 *val);
	int (*wr)(void *ctx, u32 reg, u32 mask, u32 val);
	int (*entry_wr)(void *ctx, u16 vid, u16 member, u16 untag);
	int (*entry_rd)(void *ctx, u16 vid, u32 *raw);
	void (*fast_age)(void *ctx, int port);
	void (*sw_set)(void *ctx, u8 sw);
	bool (*sw_begin_disarm)(void *ctx, u8 *ppe);
	void (*set_accept)(void *ctx, u8 accept, u16 s_ports,
			   unsigned long disarm_at);
	void (*sync)(void *ctx);
	void (*sleep_ms)(void *ctx, unsigned int ms);
	unsigned long (*jiffies)(void *ctx);
	u64 (*ms)(void *ctx);
	u64 (*us)(void *ctx);
	void (*timer)(void *ctx, bool mod, unsigned long delay);
	void (*iso_timer)(void *ctx, bool mod, unsigned long delay);
	int (*cpu_word)(void *ctx, unsigned int cpu, u32 *was, u32 val);
	int (*phy_down)(void *ctx, u16 mask);
	/* WARN_ON_ONCE: a REMOVE or SHUTDOWN forced path (over a PPE claim). */
	void (*warn)(void *ctx);
	void (*log)(void *ctx, int level, const char *fmt, ...) __printf(3, 4);
};

/* The svlan debugfs read prints through this. */
struct sv_out {
	void *ctx;
	void (*print)(void *ctx, const char *fmt, ...) __printf(2, 3);
};

struct sv_cfg {
	u8 cpu;			/* the CPU port, the S uplink */
	u16 user_ports;		/* the future s_ports */
	u16 phy_ports;		/* user ports with an internal PHY (F2) */
	u16 user_mask;		/* the CPU port's isolation word after setup */
	unsigned int hz;	/* jiffies per second */
};

/* What the glue found for one user port (RTNL; aware under vlan_lock). */
struct sv_port_info {
	bool bridged;		/* in the offloaded bridge */
	bool aware;		/* vlan_mode == AWARE */
	bool upper_8021ad;	/* has an 802.1ad upper */
	u16 upper_rsv_vid;	/* VID of an 8021q upper in 3900-3919, or 0 */
};

/* Everything the refusals of §4.3 need besides the registers. */
struct sv_arm_req {
	bool enabled;
	bool ctl_ready;
	bool running;		/* system_state == SYSTEM_RUNNING */
	bool td_present;	/* rtl8_4s_ds_td() != NULL */
	const char *offload_disabled;
	unsigned int deadline_s;
	struct sv_port_info port[RTL8_4S_NUM_PORTS];
	u16 shadow_vid;		/* a reserved VID with bridge members, or 0 */
	u16 pvid;		/* a reserved bridge PVID, or 0 ... */
	u8 pvid_port;		/* ... of this port */
};

/*
 * The switch state of §4.2 and what svlan_counters shows. state mirrors the
 * sw byte of td->st; it, incomplete and forced_live are written with
 * volatile stores so that the lock-free reads of prechangeupper, the
 * topology notes and quiesce (sv_seq_may_be_live(),
 * sv_seq_quiesce_keep_s()) see whole values.
 */
struct sv_seq {
	u8 state;		/* enum rtl8_4s_sw_state */
	u8 fault;		/* SV_FAULT_*, latched */
	u8 accept;		/* the last value given to set_accept() */
	bool incomplete;	/* X3 or X4 failed and has not verified since */
	/* The hardware is as A9 verified it: X4 and later never attempted. */
	bool armed_intact;
	bool yield_logged;
	bool retry_slow;	/* past the retry window: SV_RETRY_SLOW_S */
	/*
	 * A forced path kept S steering: X3 has not verified since. The
	 * deadline work then runs the follow-up disarm once the PPE is off.
	 */
	bool forced_live;
	/* A REMOVE took the forced path: the glue latches the next bind. */
	bool bind_latch;
	bool iso_failed;	/* the last forced path verified no barrier */
	bool iso_bench_live;	/* isolate cpu: a restore is due */
	u8 last_ppe;		/* ppe byte of the last refused disarm */
	u8 forced_trig;		/* enum sv_trigger of the last forced path */
	u8 forced_ppe;		/* its ppe byte */
	u8 forced_iso;		/* its SV_ISO_* */
	u16 s_ports;
	u32 entries_written;	/* BIT(vid - 3900): may be in hardware */
	unsigned long disarm_at;	/* jiffies; 0 unless armed */
	/* jiffies, the end of the fast retries; 0 unless retrying */
	unsigned long retry_until;
	u32 none_ms;		/* the last accept-NONE window */
	int last_err;
	u8 last_step;		/* enum sv_step */
	u32 arm_ok;
	u32 arm_fail;
	u32 refused[SV_REF_NUM];
	u32 disarm[SV_TRIG_NUM];
	u32 force_requests;
	u32 forced_n;		/* forced paths taken */
	u32 forced_cpu_was;	/* F1: the CPU port's word before it */
	u32 forced_us;		/* F0-F4 of the last forced path */
	u32 followup_n;		/* follow-up disarms run */
	u32 renew_ok;
	u32 renew_refused[SV_RENEW_NUM];
	u32 iso_bench_n;	/* isolate cpu writes */
	u32 iso_bench_was;	/* what the isolate cpu restore writes */
};

/*
 * D16 (WP3-DESIGN): the bind latch, module-static in the glue so that it
 * outlives the device. A REMOVE that took the forced path sets it; every
 * later probe is then refused (-EBUSY) until the module is reloaded.
 */
struct sv_latch {
	bool latched;
	u32 refused;		/* probes refused */
};

const char *sv_trigger_name(unsigned int trigger);
const char *sv_refusal_name(unsigned int refusal);
const char *sv_step_name(unsigned int step);
const char *sv_accept_name(unsigned int accept);
const char *sv_sw_state_name(unsigned int sw);
const char *sv_ppe_name(unsigned int ppe);

/* The canonical VLAN entry word (rtl837x_vlan_entry_canonical()). */
u32 sv_entry_word(u16 member, u16 untag);
/* Port S-VID word @k (ports 2k, 2k + 1) armed: 3900 + p for a user port. */
u32 sv_port_svid_word(const struct sv_cfg *cfg, unsigned int k);
/* The driver-owned VIDs in A1 order (3900 + p, 3910 + p per user port). */
unsigned int sv_entry_vids(const struct sv_cfg *cfg, u16 *vids);

/* §4.3: refusals, register prechecks, A0-A11 and the A1 cleanup. */
int sv_seq_arm(struct sv_seq *s, const struct sv_io *io,
	       const struct sv_cfg *cfg, const struct sv_arm_req *req);
/*
 * §4.4: X0-X10. Over a PPE claim, REMOVE, SHUTDOWN and the debugfs disarm
 * with SV_DISARM_FORCE take the forced path F0-F4 (WP3-DESIGN §2.2)
 * instead, and every other trigger is refused (-EBUSY).
 */
int sv_seq_disarm(struct sv_seq *s, const struct sv_io *io,
		  const struct sv_cfg *cfg, enum sv_trigger trigger,
		  unsigned int flags);
/*
 * The deadline work: the disarm retry, the follow-up disarm after a forced
 * path, or the deadline itself.
 */
void sv_seq_work(struct sv_seq *s, const struct sv_io *io,
		 const struct sv_cfg *cfg);
/* WP3-DESIGN §2.5: a new deadline @secs from now, never earlier. */
int sv_seq_renew(struct sv_seq *s, const struct sv_io *io,
		 const struct sv_cfg *cfg, unsigned int secs);
/*
 * The isolate cpu bench verb (WP3-DESIGN §2.6): with sw READY and @ppe
 * ACTIVE, the CPU port's isolation word to 0 for @secs; the restore work
 * (sv_seq_iso_restore()) writes the word read first back.
 */
int sv_seq_iso_bench(struct sv_seq *s, const struct sv_io *io,
		     const struct sv_cfg *cfg, unsigned int secs, u8 ppe);
void sv_seq_iso_restore(struct sv_seq *s, const struct sv_io *io,
			const struct sv_cfg *cfg);
/* Lock-free: quiesce leaves the S block as it is (forced steering live). */
bool sv_seq_quiesce_keep_s(const struct sv_seq *s);
/* D16: -EBUSY (counted) once latched; publish a REMOVE's forced path. */
int sv_latch_check(struct sv_latch *l);
void sv_latch_note(struct sv_latch *l, const struct sv_seq *s);
/*
 * Setup and quiesce: write the off values, uplink first, and log one line
 * prefixed "svlan: <what>". Returns the uplink's result apart (0, or the
 * error of its last attempt; -EIO when it reads back set); *rest_ok tells
 * whether every other off field reads back off.
 */
int sv_seq_off(const struct sv_io *io, const struct sv_cfg *cfg,
	       const char *what, bool *rest_ok);
/* Setup found an off field it could not clear: FAULT off_inert. */
void sv_seq_setup_fault(struct sv_seq *s);
/* Log the S block before or after the chip reset ("svlan: <what> ..."). */
void sv_seq_dump(const struct sv_io *io, const char *what);
/* The setup readback line: S block off, entries 3900-3919 empty. */
bool sv_seq_report(const struct sv_io *io, const struct sv_cfg *cfg);
/*
 * Lock-free: whether S-R steering may be live in the switch (its uplink may
 * be set): ARMING, READY, DISARMING, or a FAULT whose X3 or X4 has not
 * verified since, or that a forced path left armed. A FAULT latched with
 * the uplink verified clear (off_inert at setup or X5, entries at X8, a
 * follow-up disarm) is not, and no arm passes before the next bind.
 */
bool sv_seq_may_be_live(const struct sv_seq *s);
/* Whether the svlan read expects the armed words. */
bool sv_seq_expect_armed(const struct sv_seq *s);
/*
 * The svlan debugfs read: the S block (armed or off, sv_seq_expect_armed())
 * and the CPU port's isolation word (0 under FAULT forced_ppe, else
 * user_mask); returns 0 when everything matches.
 */
int sv_seq_show(const struct sv_seq *s, const struct sv_io *io,
		const struct sv_cfg *cfg, const struct sv_out *out);

#endif /* __RTL8372N_SVLAN_SEQ_H__ */
