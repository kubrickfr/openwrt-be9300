#!/bin/sh
# ppe-sr: the S-R steering controller (WP3-DESIGN §5). BusyBox ash.
#
# One controller for the two kernel halves of S-R offload: the rtl8372n
# switch's S-VLAN steering (debugfs rtl8372n/svlan, svlan_counters) and the
# PPE's S-R mode (debugfs ppe/offload_sr*). The modules never call each other;
# this script orders them: arm the switch, then the PPE; disarm the PPE, then
# the switch. The CLI, the init script, its daemon and the gate kit all call it.
#
# Every kernel write goes through ctl_write(): one write in a background
# subshell with a PID file, waited for a bounded time. A write that does not
# return in time may still land (TIMEOUT); it is never followed by a second
# write to the same file while it is pending. Every read is of a memory-only
# file (svlan_counters, offload_sr_status, offload_sr_scope), also bounded.
# Never read here: debugfs svlan (MDIO), offload_sr_hw, offload_flows*, any
# glob over /sys/kernel/debug, ethtool, bridge, ip neigh, interface
# statistics, ubus network.device.
#
# Verbs: status [--raw] | check | scope apply | enable [--lease s] [--no-offers]
#        | disable [--persist] | renew | rewire | stop [--no-rewire] | reconcile
#        | health | daemon | modconf
# Log lines (syslog tag ppe-sr) are classified by the stop regex of
# WP3-DESIGN §4.7: "enable failed", "unwind failed", "rewire failed", "FAULT",
# "health FAIL" and "stop" are stop lines; "enabled", "disabled", "rearm",
# "hold", "rewire ok" and "health ok" are routine.

DBG=/sys/kernel/debug
SVF=$DBG/rtl8372n/svlan
SVC=$DBG/rtl8372n/svlan_counters
PSR=$DBG/ppe/offload_sr
PSC=$DBG/ppe/offload_sr_scope
PSS=$DBG/ppe/offload_sr_status
PSG=$DBG/ppe/offload_sr_guard
DSAPAR=/sys/module/rtl8372n_dsa/parameters
PPEPAR=/sys/module/qcom_ppe/parameters
NET=/sys/class/net
PSTORE=/sys/fs/pstore
ARCHIVE=/root/ppe-sr-pstore
MODCONF=/etc/modules.conf
MANAGED='options rtl8372n_dsa svlan=1 # ppe-sr'
RUN=/var/run/ppe-sr
# The daemon's wake-up period (s); the desk harness shortens it.
TICK=${PPE_SR_TICK:-30}
BENIGN=' conduit_down bridge_gone user_gone '
QUIET=0
LOCKED=0

# --- output -----------------------------------------------------------------

# log LEVEL MSG: syslog "ppe-sr: MSG" (daemon facility); stderr too, unless
# the daemon (procd sends its output to syslog already).
log() {
	logger -t ppe-sr -p "daemon.$1" "$2"
	[ "$QUIET" = 1 ] || echo "ppe-sr: $2" >&2
}

die() {
	echo "ppe-sr: $*" >&2
	exit 1
}

uptime_s() {
	local u
	read -r u _ < /proc/uptime
	echo "${u%%.*}"
}

stamp() {
	date +%Y-%m-%dT%H:%M:%S
}

isnum() {
	case $1 in ''|*[!0-9]*) return 1 ;; esac
	return 0
}

# --- kernel file access -----------------------------------------------------

# err_name TEXT: the errno name for a shell write error (glibc or musl text).
err_name() {
	local t=$1

	case $t in
	*"write error: "*) t=${t##*write error: } ;;
	*"can't create "*|*"can't open "*) echo EMISSING; return ;;
	*) echo "E?"; return ;;
	esac
	t=${t%%
*}
	case $t in
	"Operation not permitted") echo EPERM ;;
	"No such file or directory") echo ENOENT ;;
	"Interrupted system call") echo EINTR ;;
	"Input/output error"|"I/O error") echo EIO ;;
	"Cannot allocate memory"|"Out of memory") echo ENOMEM ;;
	"Permission denied") echo EACCES ;;
	"Bad address") echo EFAULT ;;
	"Device or resource busy"|"Resource busy") echo EBUSY ;;
	"No such device") echo ENODEV ;;
	"Invalid argument") echo EINVAL ;;
	"No space left on device") echo ENOSPC ;;
	"Numerical result out of range"|"Result not representable") echo ERANGE ;;
	"Operation not supported"|"Not supported") echo EOPNOTSUPP ;;
	"Transport endpoint is not connected"|"Socket not connected") echo ENOTCONN ;;
	"Cannot send after transport endpoint shutdown"|"Cannot send after socket shutdown") echo ESHUTDOWN ;;
	"Operation already in progress") echo EALREADY ;;
	"Stale file handle") echo ESTALE ;;
	*) echo "E?($t)" ;;
	esac
}

# writer_alive PIDFILE: a writer of ours is still pending (PID reuse aside:
# the subshell carries this script's command line).
writer_alive() {
	local pid

	[ -s "$1" ] || return 1
	read -r pid < "$1"
	isnum "$pid" || return 1
	kill -0 "$pid" 2>/dev/null || return 1
	grep -q ppe-sr "/proc/$pid/cmdline" 2>/dev/null
}

# writer_wait PIDFILE: wait, however long, for a pending writer to exit.
writer_wait() {
	local n=0

	while writer_alive "$1"; do
		[ $n = 0 ] && log notice "hold: waiting for a pending kernel write ($(cat "$1"))"
		n=$((n + 1))
		sleep 1
	done
	rm -f "$1"
}

# ctl_write FILE TEXT TIMEOUT_S [PIDFILE]: one write of TEXT (one write(2)),
# from a background subshell, waited at most TIMEOUT_S (whole seconds: the
# subshell reports on a FIFO, read with read -t). Sets W_RET to 0, an errno
# name, EMISSING (the file is not there) or TIMEOUT (the write may still land:
# W_PID is pending, PIDFILE kept); returns 0 only for 0. A writer still
# pending on PIDFILE is waited for first: never two at once.
ctl_write() {
	local f=$1 t=$2 to=$3 pf=${4:-$RUN/writer.pid} st= end now fifo=$RUN/fifo.$$

	W_RET= W_PID= W_ERR=$pf.err
	writer_wait "$pf"
	rm -f "$W_ERR" "$fifo"
	mkfifo "$fifo" || { W_RET="E?(mkfifo)"; return 1; }
	exec 8<>"$fifo"
	rm -f "$fifo"
	(
		echo "$t" 2>"$W_ERR" >"$f"
		r=$?
		echo "w $r" >&8
		exit $r
	) &
	W_PID=$!
	echo "$W_PID" > "$pf"
	end=$(($(uptime_s) + to))
	# read -t returns early on a signal (the daemon's SIGHUP): read again.
	while ! read -t "$to" st <&8; do
		now=$(uptime_s)
		if [ "$now" -ge "$end" ]; then
			W_RET=TIMEOUT
			return 1
		fi
		to=$((end - now))
	done
	exec 8>&-
	wait "$W_PID"
	rm -f "$pf"
	if [ "${st#w }" = 0 ]; then
		W_RET=0
		return 0
	fi
	W_RET=$(err_name "$(cat "$W_ERR" 2>/dev/null)")
	return 1
}

# ctl_finish: after a TIMEOUT, wait until that writer (our child) is gone and
# set W_RET from the status it put on the FIFO. A trapped signal (the
# daemon's SIGHUP) ends wait early, 128 + its number, with the writer still
# pending: wait again while it exists (a zombie included). Once the shell
# has reaped it, kill -0 fails; 127 is a PID that is no longer our child.
ctl_finish() {
	local st= n=0

	while kill -0 "$W_PID" 2>/dev/null; do
		wait "$W_PID"
		[ $? = 127 ] && break
	done
	# Already written by the writer, unless it was killed first.
	while [ $n -lt 3 ] && ! read -t 1 st <&8; do n=$((n + 1)); done
	exec 8>&-
	rm -f "$RUN/writer.pid"
	if [ "${st#w }" = 0 ]; then W_RET=0; else W_RET=$(err_name "$(cat "$W_ERR" 2>/dev/null)"); fi
}

# ctl_read FILE OUT [TIMEOUT_S]: copy a memory-only file to OUT; the reader is
# killed after TIMEOUT_S (its lock waits are killable, and an open debugfs
# file pins its module).
ctl_read() {
	local f=$1 o=$2 to=${3:-5}

	rm -f "$o" "$o.tmp"
	[ -e "$f" ] || return 1
	timeout -s KILL "$to" cat "$f" > "$o.tmp" 2>/dev/null 9>&- || { rm -f "$o.tmp"; return 1; }
	mv "$o.tmp" "$o"
}

# --- state --------------------------------------------------------------------

# sv_load [TIMEOUT]: svlan_counters into S_* (S_OK=1 when read).
sv_load() {
	local k v

	S_OK=0 S_ABI=0 S_BOUND=0 S_STATE=- S_SW=- S_FAULT=- S_ACCEPT=- S_PORTS=- S_PPE=-
	S_TTL=0 S_TOPO=0 S_STEER=0 S_FORCED_N=0 S_ISO_CPU=- S_ISO_PHY=- S_ISOFAIL=0 S_LASTERR=-
	ctl_read "$SVC" "$RUN/svc.txt" "${1:-5}" || return 1
	while read -r k v; do
		case $k in
		abi) S_ABI=$v ;;
		bound) S_BOUND=$v ;;
		state) S_STATE=$v ;;
		sw_state) S_SW=$v ;;
		fault) S_FAULT=$v ;;
		accept) S_ACCEPT=$v ;;
		s_ports) S_PORTS=$v ;;
		ppe_state) S_PPE=$v ;;
		disarm_in_s) S_TTL=$v ;;
		topo_warn) S_TOPO=$v ;;
		forced_n) S_FORCED_N=$v ;;
		forced_iso) v=${v#cpu }; S_ISO_CPU=${v%% *}; S_ISO_PHY=${v##* } ;;
		steer_live) S_STEER=$v ;;
		iso_failed) S_ISOFAIL=$v ;;
		last_err) S_LASTERR=$v ;;
		esac
	done < "$RUN/svc.txt"
	S_OK=1
}

# ppe_load [TIMEOUT]: offload_sr_status into M_* (M_OK=1 when read).
ppe_load() {
	local k v

	M_OK=0 M_STATE=- M_TD=- M_INH=- M_GEN=- M_REF=- M_OFFLOAD=- M_TTL=none M_SWTTL=none
	M_SCOPE=- M_UU=- M_IDS=0 M_ACC=- M_LAST=- M_FAULT=- M_INERT=-
	ctl_read "$PSS" "$RUN/pss.txt" "${1:-5}" || return 1
	while read -r k v; do
		case $k in
		state) M_STATE=$v ;;
		td) M_TD=$v ;;
		inhibit) M_INH=$v ;;
		inhibit_gen) M_GEN=$v ;;
		sw_module_ref) M_REF=$v ;;
		offload) M_OFFLOAD=$v ;;
		deadline_in_s) M_TTL=$v ;;
		switch_disarm_in_s) M_SWTTL=$v ;;
		scope) M_SCOPE=$v ;;
		u_uploads) M_UU=$v ;;
		id) M_IDS=$((M_IDS + 1)) ;;
		accepted) M_ACC=$v ;;
		last) M_LAST=$v ;;
		fault) M_FAULT=$v ;;
		inert_fail) M_INERT=$v ;;
		esac
	done < "$RUN/pss.txt"
	M_OK=1
}

# combined: C from S_* and M_* (WP3-DESIGN §3.1).
combined() {
	if [ "$S_OK" != 1 ] || [ "$S_BOUND" != 1 ] || [ "$S_ABI" != 1 ] || [ "$M_OK" != 1 ]; then
		C=UNAVAILABLE
	elif [ "$S_STATE" = fault ] || [ "$M_STATE" = FAULT ]; then
		C=FAULT
	else
		case $S_STATE/$M_STATE in
		off/off) C=OFF ;;
		switch_ready/off) C=SWITCH_READY ;;
		sr_ready/arming|sr_ready/ready|sr_ready/disarming) C=SR_READY ;;
		active/active) C=ACTIVE ;;
		disarming/off) C=DISARMING ;;
		*) C=TRANSITION ;;
		esac
	fi
}

# state_read [TIMEOUT]: both files and C; a TRANSITION is re-read up to 5 x 1 s.
state_read() {
	local n=0

	while :; do
		sv_load "$1"
		ppe_load "$1"
		combined
		[ "$C" != TRANSITION ] && return 0
		[ $n -ge 5 ] && return 0
		n=$((n + 1))
		sleep 1
	done
}

# fault_which: what is in FAULT, from S_* and M_*.
fault_which() {
	local w=

	[ "$S_FAULT" != none ] && [ "$S_FAULT" != - ] && w="switch $S_FAULT"
	if [ "$M_STATE" = FAULT ] || { [ "$M_FAULT" != - ] && [ "${M_FAULT%% *}" != none ]; }; then
		w="${w:+$w; }ppe $M_FAULT"
	fi
	[ "$S_PPE" = fault ] && [ -z "$w" ] && w="ppe byte fault"
	echo "${w:-unknown}"
}

wired_isolated() {
	case " $S_FAULT " in *" forced_ppe "*) ;; *) return 1 ;; esac
	[ "$S_ISO_CPU" = ok ] || [ "$S_ISO_PHY" = ok ]
}

# last_trigger: the trigger of the PPE's last disarm, or the last kind.
last_trigger() {
	local r=${M_LAST#* }

	case $M_LAST in
	disarm\ *) echo "${r%% *}" ;;
	*) echo "${M_LAST%% *}" ;;
	esac
}

# --- runtime state, UCI, hold ---------------------------------------------------

uci_put() {	# KEY [VALUE]: one UCI write (an empty VALUE deletes)
	local d=$RUN/uci.$$

	mkdir -p "$d"
	if [ -n "$2" ]; then
		uci -q -t "$d" set "ppe_sr.main.$1=$2"
	else
		uci -q -t "$d" delete "ppe_sr.main.$1"
	fi
	uci -q -t "$d" commit ppe_sr
	rm -rf "$d"
}

cfg_load() {
	local o

	. /lib/functions.sh
	# uci_load resets lists, not options: in the long-running daemon, an
	# option deleted since the last load would keep its old value.
	for o in enabled auto_arm offers lease renew holdoff max_rearm ppe_params \
		stop_force stop_rewire hold armed_build ack_multi_cvid; do
		unset "CONFIG_main_$o"
	done
	config_load ppe_sr
	config_get_bool C_ENABLED main enabled 0
	config_get_bool C_AUTO main auto_arm 0
	config_get_bool C_OFFERS main offers 1
	config_get C_LEASE main lease 86400
	config_get C_RENEW main renew 3600
	config_get C_HOLDOFF main holdoff 120
	config_get C_MAXREARM main max_rearm 3
	config_get_bool C_PPEPARAMS main ppe_params 1
	config_get C_STOPFORCE main stop_force full
	config_get_bool C_STOPREWIRE main stop_rewire 1
	config_get C_HOLD main hold ''
	config_get C_BUILD main armed_build ''
	config_get_bool C_MULTI main ack_multi_cvid 0
	config_get C_ACK main ack_port ''
}

# cfg_bad: the first invalid option, or nothing.
cfg_bad() {
	isnum "$C_LEASE" && [ "$C_LEASE" -ge 90 ] && [ "$C_LEASE" -le 86400 ] || { echo "lease '$C_LEASE'"; return; }
	isnum "$C_RENEW" && { [ "$C_RENEW" = 0 ] || { [ "$C_RENEW" -ge 60 ] && [ "$C_RENEW" -le 86400 ]; }; } ||
		{ echo "renew '$C_RENEW'"; return; }
	isnum "$C_HOLDOFF" && [ "$C_HOLDOFF" -le 3600 ] || { echo "holdoff '$C_HOLDOFF'"; return; }
	isnum "$C_MAXREARM" && [ "$C_MAXREARM" -le 100 ] || { echo "max_rearm '$C_MAXREARM'"; return; }
	case $C_STOPFORCE in full|nophy) ;; *) echo "stop_force '$C_STOPFORCE'" ;; esac
}

hold_get() {
	local h=

	[ -s "$RUN/hold" ] && h=$(cat "$RUN/hold")
	[ -n "$h" ] || h=$(uci -q get ppe_sr.main.hold)
	echo "$h"
}

# hold_set REASON [persist]: tmpfs; with persist also UCI (one write, D17).
hold_set() {
	echo "$1" > "$RUN/hold"
	rm -f "$RUN/hold_noted"
	[ "$2" = persist ] && uci_put hold "$1"
}

hold_clear() {
	rm -f "$RUN/hold" "$RUN/hold_noted"
	[ -n "$(uci -q get ppe_sr.main.hold)" ] && uci_put hold
}

# hold_note REASON: a tmpfs hold from reconcile, logged once ("hold_noted"
# marks a tmpfs hold reconcile wrote and logged itself).
hold_note() {
	[ -s "$RUN/hold" ] && [ "$(cat "$RUN/hold_noted" 2>/dev/null)" = "$1" ] && return 0
	hold_set "$1 $(stamp)"
	echo "$1" > "$RUN/hold_noted"
	log notice "hold $1"
}

# pre_note: log why reconcile does not arm, once (a hold it wrote is quiet).
pre_note() {
	case $P_KIND in
	hold) hold_note "$P_WHY" ;;
	held) [ -s "$RUN/hold_noted" ] || defer_note "$P_WHY" ;;
	*) defer_note "$P_WHY" ;;
	esac
}

# defer_note REASON: why an automatic arm waits, logged once per reason.
defer_note() {
	local old=

	[ -s "$RUN/deferred" ] && old=$(cat "$RUN/deferred")
	[ "$old" = "$1" ] && return 0
	echo "$1" > "$RUN/deferred"
	log info "rearm deferred ($1)"
}

build_id() {
	local r m s out=

	r=$(uname -r)
	for m in qcom-ppe rtl8372n_dsa; do
		s=-
		[ -f "/lib/modules/$r/$m.ko" ] && s=$(md5sum "/lib/modules/$r/$m.ko" | cut -d' ' -f1)
		out="${out:+$out }$s"
	done
	echo "$out"
}

# --- pstore brake (D18: dmesg-ramoops-* only) ---------------------------------

pstore_dmesg() {
	local f

	for f in "$PSTORE"/dmesg-ramoops-*; do
		[ -e "$f" ] && echo "$f"
	done
}

# pstore_archive: copy each dmesg record once to $ARCHIVE/<date>/.
pstore_archive() {
	local f s d=

	for f in $(pstore_dmesg); do
		s=$(md5sum "$f" | cut -d' ' -f1)
		grep -q "^$s " "$ARCHIVE/index" 2>/dev/null && continue
		[ -n "$d" ] || { d=$ARCHIVE/$(date +%Y%m%dT%H%M%S); mkdir -p "$d"; }
		cp "$f" "$d/" && echo "$s $d/${f##*/}" >> "$ARCHIVE/index"
	done
}

# --- the scope ------------------------------------------------------------------

ipv4_ok() {
	local IFS=. x

	case $1 in ''|*[!0-9.]*|.*|*.|*..*) return 1 ;; esac
	set -- $1
	[ $# = 4 ] || return 1
	for x; do
		case $x in 0?*) return 1 ;; esac
		[ ${#x} -le 3 ] && [ "$x" -le 255 ] || return 1
	done
}

vid_ok() {
	case $1 in ''|*[!0-9]*|0*) return 1 ;; esac
	[ ${#1} -le 4 ] && [ "$1" -le 4094 ] && { [ "$1" -lt 3900 ] || [ "$1" -gt 3919 ]; }
}

sc_class() {
	local n p pn

	[ -n "$SC_ERR" ] && return
	set -f
	set -- $1
	set +f
	if [ $# != 3 ]; then SC_ERR="class '$*': want <lanN> <U|T> <vid>"; return; fi
	case $1 in lan[1-8]) n=${1#lan} ;; *) SC_ERR="class '$*': port $1 is not lan1-lan8"; return ;; esac
	case $2 in U|T) ;; *) SC_ERR="class '$*': form $2 is not U or T"; return ;; esac
	vid_ok "$3" || { SC_ERR="class '$*': VID $3 not 1-4094 outside 3900-3919"; return; }
	case " $C_ACK " in *" $1 "*) ;; *) SC_ERR="class '$*': $1 is not an ack_port"; return ;; esac
	[ -d "$NET/$1" ] || { SC_ERR="class '$*': no DSA user port $1"; return; }
	p=$((8 - n))
	pn=$(cat "$NET/$1/phys_port_name" 2>/dev/null)
	[ -z "$pn" ] || [ "$pn" = "p$p" ] || { SC_ERR="class '$*': $1 is $pn, not p$p"; return; }
	case "$SC_LINES" in *"class $p $2 $3
"*) SC_ERR="class '$*' given twice"; return ;; esac
	SC_LINES="${SC_LINES}class $p $2 $3
"
	SC_NCLS=$((SC_NCLS + 1))
	case " $SC_VIDS " in *" $3 "*) ;; *) SC_VIDS="${SC_VIDS:+$SC_VIDS }$3" ;; esac
	case " $SC_PORTS " in *" $1 "*) ;; *) SC_PORTS="${SC_PORTS:+$SC_PORTS }$1" ;; esac
	[ "$2" = U ] && SC_U=1
}

sc_client() {
	[ -n "$SC_ERR" ] && return
	if [ "$1" = any ]; then
		[ -z "$SC_CLIENTS" ] || { SC_ERR="client any with other clients"; return; }
		SC_CLIENTS=any
		return
	fi
	[ "$SC_CLIENTS" = any ] && { SC_ERR="client $1 with client any"; return; }
	ipv4_ok "$1" || { SC_ERR="client '$1' is not an IPv4 address"; return; }
	case " $SC_CLIENTS " in *" $1 "*) SC_ERR="client $1 given twice"; return ;; esac
	SC_CLIENTS="${SC_CLIENTS:+$SC_CLIENTS }$1"
	SC_NCLI=$((SC_NCLI + 1))
	[ $SC_NCLI -le 64 ] || SC_ERR="more than 64 clients"
}

# scope_build: validate the UCI scope (WP3-DESIGN §5.2); SC_ERR, or SC_TEXT
# (the lines to write, commit last), SC_PORTS, SC_VIDS.
scope_build() {
	local c

	SC_ERR= SC_LINES= SC_NCLS=0 SC_VIDS= SC_PORTS= SC_U=0 SC_CLIENTS= SC_NCLI=0 SC_TEXT=
	config_list_foreach main class sc_class
	config_list_foreach main client sc_client
	[ -n "$SC_ERR" ] && return 1
	[ $SC_NCLS -gt 0 ] || { SC_ERR="no class"; return 1; }
	[ $SC_NCLS -le 330 ] || { SC_ERR="more than 330 classes"; return 1; }
	[ -n "$SC_CLIENTS" ] || { SC_ERR="no client"; return 1; }
	case $SC_VIDS in *" "*) [ "$C_MULTI" = 1 ] || { SC_ERR="VIDs $SC_VIDS: more than one needs ack_multi_cvid 1"; return 1; } ;; esac
	SC_TEXT=$SC_LINES
	for c in $SC_CLIENTS; do SC_TEXT="${SC_TEXT}client $c
"; done
	SC_TEXT="${SC_TEXT}commit"
}

# scope_expect: the class and client lines the read-back must hold. Both sides
# are compared as sets (each sorted plainly, lines are unique): the router's
# BusyBox sort has no -t/-k (FEATURE_SORT_BIG is off) and ignores them, so
# the kernel's order (port, U before T, VID) cannot be rebuilt here.
scope_expect() {
	local c

	printf '%s' "$SC_LINES" | sort
	for c in $SC_CLIENTS; do echo "client $c"; done | sort
}

# scope_apply: write the validated scope, commit, read it back (SC_RET).
scope_apply() {
	SC_RET=0
	if [ "$SC_U" = 1 ] && [ "$(cat "$PPEPAR/sr_u_uploads" 2>/dev/null)" = N ]; then
		log warning "scope: U classes without qcom_ppe.sr_u_uploads get no upload identity"
	fi
	ctl_write "$PSC" "$SC_TEXT" 10 || { SC_RET=$W_RET; return 1; }
	ctl_read "$PSC" "$RUN/scope.txt" 5 || { SC_RET="read-back failed"; return 1; }
	{ grep '^class ' "$RUN/scope.txt" | sort; grep '^client ' "$RUN/scope.txt" | sort; } > "$RUN/scope.got"
	scope_expect > "$RUN/scope.want"
	cmp -s "$RUN/scope.got" "$RUN/scope.want" || { SC_RET="read-back mismatch"; return 1; }
}

# --- preconditions (WP3-DESIGN §5.5) ---------------------------------------------

# pre_fail KIND REASON: KIND hold (until an enable or reboot) or wait.
pre_fail() {
	P_KIND=$1 P_WHY=$2
	[ "$P_PRINT" = 1 ] && echo "FAIL $2 ($1)"
	[ "$P_PRINT" = 1 ]
}

pre_ok() {
	[ "$P_PRINT" = 1 ] && echo "ok   $1"
	return 0
}

iface_up() {
	ubus call "network.interface.$1" status 2>/dev/null | grep -q '"up": true'
}

# preconds MODE: MODE explicit (an operator's enable) or auto (boot arm,
# re-arm). Needs state_read and cfg_load. Returns 1 at the first failure
# (P_KIND, P_WHY); with P_PRINT=1, prints every check and goes on.
preconds() {
	local m=$1 b v p fail=0 bad

	P_KIND= P_WHY=
	if [ "$m" = auto ]; then
		{ [ "$C_ENABLED" = 1 ] && [ "$C_AUTO" = 1 ]; } && pre_ok "enabled 1 auto_arm 1" ||
			{ pre_fail wait "auto_arm off" || return 1; fail=1; }
	fi
	bad=$(cfg_bad)
	[ -z "$bad" ] && pre_ok config || { pre_fail hold "config $bad" || return 1; fail=1; }
	{ [ "$S_BOUND" = 1 ] && [ "$S_ABI" = 1 ] && [ "$M_OK" = 1 ]; } && pre_ok "bound 1 abi 1" ||
		{ pre_fail wait "not bound (bound $S_BOUND abi $S_ABI ppe $M_OK)" || return 1; fail=1; }
	{ [ "$C" != FAULT ] && [ "$S_FAULT" = none ]; } && pre_ok "no FAULT" ||
		{ pre_fail hold "FAULT $(fault_which)" || return 1; fail=1; }
	[ "$M_INERT" = 0 ] && pre_ok "inert_fail 0" || { pre_fail hold "inert_fail $M_INERT" || return 1; fail=1; }
	v=$(cat "$PPEPAR/flow_offload" 2>/dev/null)
	[ "$v" = Y ] && pre_ok "flow_offload Y" || { pre_fail wait "flow_offload ${v:-?}" || return 1; fail=1; }
	[ "$M_OFFLOAD" = "enabled 1 degraded 0 tripped 0" ] && pre_ok "offload $M_OFFLOAD" ||
		{ pre_fail wait "offload $M_OFFLOAD" || return 1; fail=1; }
	if scope_build; then
		pre_ok "scope: $SC_NCLS class(es), clients $SC_CLIENTS"
	else
		pre_fail hold "scope $SC_ERR" || return 1
		fail=1
	fi
	iface_up lan && pre_ok "interface lan up" || { pre_fail wait "interface lan down" || return 1; fail=1; }
	for v in $SC_VIDS; do
		[ "$(cat "$NET/br-lan.$v/operstate" 2>/dev/null)" = up ] && pre_ok "br-lan.$v up" ||
			{ pre_fail wait "br-lan.$v not up" || return 1; fail=1; }
	done
	[ "$(cat "$NET/br-lan/bridge/vlan_filtering" 2>/dev/null)" = 1 ] && pre_ok "br-lan vlan_filtering 1" ||
		{ pre_fail wait "br-lan vlan_filtering off" || return 1; fail=1; }
	for p in $SC_PORTS; do
		v=$(readlink "$NET/$p/master" 2>/dev/null)
		[ "${v##*/}" = br-lan ] && pre_ok "$p in br-lan" || { pre_fail wait "$p not in br-lan" || return 1; fail=1; }
	done
	bad=
	for v in rtl8372n_sv_probe ppe_gate_probe; do
		grep -q "^$v " /proc/modules && bad="${bad:+$bad }$v"
	done
	[ -z "$bad" ] && pre_ok "no probe module" || { pre_fail hold "probe $bad loaded" || return 1; fail=1; }
	[ -e /tmp/.failsafe ] && { pre_fail wait failsafe || return 1; fail=1; }
	[ "$m" = explicit ] && return $fail

	[ "$(uptime_s)" -ge "$C_HOLDOFF" ] && pre_ok "uptime >= holdoff $C_HOLDOFF" ||
		{ pre_fail wait "uptime < holdoff $C_HOLDOFF" || return 1; fail=1; }
	if [ -n "$(pstore_dmesg)" ]; then
		pre_fail hold pstore || return 1
		fail=1
	else
		pre_ok "no dmesg-ramoops"
	fi
	v=$(hold_get)
	[ -z "$v" ] && pre_ok "no hold" || { pre_fail held "held: $v" || return 1; fail=1; }
	# D17: only a build an explicit enable recorded arms by itself, so the
	# first automatic arm of any build (none recorded yet included) follows
	# one "ppe-sr enable".
	b=$(build_id)
	v=build
	[ -n "$C_BUILD" ] || v="build unrecorded (enable once)"
	[ "$b" = "$C_BUILD" ] && pre_ok "armed_build = running" || { pre_fail hold "$v" || return 1; fail=1; }
	[ "$M_INH" = 0 ] && pre_ok "inhibit 0" || { pre_fail wait inhibit || return 1; fail=1; }
	budget_ok && pre_ok "budget" || { pre_fail wait budget || return 1; fail=1; }
	return $fail
}

# Re-arm budget: max_rearm per rolling hour; the first arm of a boot is free.
budget_ok() {
	local t now n=0

	[ -e "$RUN/armed_once" ] || return 0
	now=$(uptime_s)
	[ -s "$RUN/rearm" ] && while read -r t; do
		[ $((now - t)) -lt 3600 ] && n=$((n + 1))
	done < "$RUN/rearm"
	[ $n -lt "$C_MAXREARM" ]
}

budget_take() {
	[ -e "$RUN/armed_once" ] && uptime_s >> "$RUN/rearm"
}

# --- locking --------------------------------------------------------------------

# lock_take MAX_WAIT_S: the controller's flock (fd 9, inherited by its kernel
# writers, so a pending write keeps it). Then recover an interrupted enable.
lock_take() {
	exec 9>>"$RUN/lock"
	if [ "$1" = 0 ]; then
		flock -n 9 || { exec 9>&-; return 1; }
	else
		# flock locks fd 9's open file, which this shell keeps.
		timeout -s KILL "$1" flock 9 || { exec 9>&-; return 1; }
	fi
	LOCKED=1
	recover_enabling
}

lock_drop() {
	[ "$LOCKED" = 1 ] && exec 9>&-
	LOCKED=0
}

# An enable cut short (procd SIGTERM, a crash, ^C) left its marker: unwind,
# and put back a hold its verify step had cleared (en_hold_restore).
recover_enabling() {
	local st h

	[ -e "$RUN/enabling" ] || return 0
	read -r st _ < "$RUN/enabling"
	writer_wait "$RUN/writer.pid"
	unwind
	if [ "$U_OK" = 1 ]; then
		log err "enable failed at ${st:-?}: interrupted; unwound"
	else
		log err "enable failed at ${st:-?}: interrupted; unwind failed ($U_TXT)"
	fi
	if [ -s "$RUN/enabling.hold" ]; then
		h=$(cat "$RUN/enabling.hold")
		[ -n "$(uci -q get ppe_sr.main.hold)" ] || uci_put hold "$h"
	fi
	rm -f "$RUN/enabling" "$RUN/enabling.hold"
}

# --- enable, unwind, disable ------------------------------------------------------

# unwind: PPE disarm (sets inhibit), then switch disarm. U_OK, U_TXT.
unwind() {
	local p s

	if [ -e "$PSR" ]; then
		ctl_write "$PSR" disarm 10
		[ "$W_RET" = TIMEOUT ] && ctl_finish
		p=$W_RET
	else
		p=EMISSING
	fi
	ctl_write "$SVF" disarm 10
	[ "$W_RET" = TIMEOUT ] && ctl_finish
	s=$W_RET
	U_TXT="ppe disarm $p, switch disarm $s"
	U_OK=1
	case $p in 0|EMISSING) ;; *) U_OK=0 ;; esac
	case $s in 0|EMISSING|EOPNOTSUPP) ;; *) U_OK=0 ;; esac
}

en_step() {
	EN_STEP=$1
	echo "$1 $EN_MODE" > "$RUN/enabling"
}

# en_hold_clear: an explicit enable's acknowledgement of the hold (D17),
# made at its verify step only, just before the verify read: a stop whose
# guard lands after that read writes its hold after this clear. The hold is
# kept in enabling.hold for recover_enabling() until the enable ends.
en_hold_clear() {
	EN_HOLD_TMP=$(cat "$RUN/hold" 2>/dev/null)
	EN_HOLD_UCI=$(uci -q get ppe_sr.main.hold)
	[ -n "$EN_HOLD_UCI" ] && printf '%s\n' "$EN_HOLD_UCI" > "$RUN/enabling.hold"
	hold_clear
	EN_CLEARED=1
}

# en_hold_restore: a failed explicit enable puts back the hold its verify
# step cleared, unless a newer one (a stop's) is there: a failed enable
# never lifts the brake of D17.
en_hold_restore() {
	[ "$EN_CLEARED" = 1 ] || return 0
	EN_CLEARED=0
	[ -n "$EN_HOLD_TMP" ] && [ ! -s "$RUN/hold" ] && echo "$EN_HOLD_TMP" > "$RUN/hold"
	[ -n "$EN_HOLD_UCI" ] && [ -z "$(uci -q get ppe_sr.main.hold)" ] && uci_put hold "$EN_HOLD_UCI"
	rm -f "$RUN/enabling.hold"
}

# en_fail ERR: unwind, put back a hold the verify step cleared, and log the
# one stop line.
en_fail() {
	if [ "$W_RET" = TIMEOUT ] && [ "$1" = TIMEOUT ]; then
		log err "enable failed at $EN_STEP: TIMEOUT (pending)"
		ctl_finish
		state_read
		unwind
		if [ "$U_OK" = 1 ]; then
			log notice "disabled (unwound after TIMEOUT at $EN_STEP, found $C)"
		else
			log err "unwind failed after TIMEOUT at $EN_STEP: $U_TXT"
		fi
	else
		unwind
		if [ "$U_OK" = 1 ]; then
			log err "enable failed at $EN_STEP: $1; unwound"
		else
			log err "enable failed at $EN_STEP: $1; unwind failed ($U_TXT)"
		fi
	fi
	en_hold_restore
	rm -f "$RUN/enabling"
	return 1
}

# en_stopped: a stop overlapped this enable (inhibit_gen moved, or uninhibit
# -ESTALE); it wins.
en_stopped() {
	hold_set "stop during enable $(stamp)" persist
	log notice "hold stop during enable ($1)"
	rm -f "$RUN/enabling"
	return 1
}

# enable_run MODE LEASE OFFERS [ppe|cycle]: the coordinated enable
# (WP3-DESIGN §3.2). MODE explicit (an operator: uninhibits, and once the arm
# is verified clears the hold, acknowledges pstore records and records the
# build; a failed one leaves all three) or auto (boot arm and re-arms: none
# of that).
# "ppe" arms the PPE alone over an armed switch, "cycle" disarms the armed
# switch first (reconcile, §5.5). Needs cfg_load and the lock; returns 0 when
# armed.
enable_run() {
	local L=$2 offers=$3 only=$4 g g2 want

	EN_MODE=$1
	EN_CLEARED=0
	en_step start
	ppe_load || { W_RET=; en_fail "offload_sr_status unreadable"; return 1; }
	g=$M_GEN
	if [ "$EN_MODE" = auto ] && [ "$M_INH" != 0 ]; then
		# A stop landed since the preconditions were read: it wins.
		rm -f "$RUN/enabling"
		defer_note inhibit
		return 1
	fi
	if [ "$EN_MODE" = explicit ]; then
		en_step uninhibit
		ctl_write "$PSR" "uninhibit $g" 10
		case $W_RET in
		0) ;;
		ESTALE) en_stopped "uninhibit $g ESTALE"; return 1 ;;
		*) en_fail "$W_RET"; return 1 ;;
		esac
		en_step check
		state_read
		P_PRINT=0
		preconds explicit || { W_RET=; en_fail "$P_WHY"; return 1; }
	else
		scope_build || { W_RET=; en_fail "scope $SC_ERR"; return 1; }
	fi
	en_step scope
	scope_apply || { en_fail "$SC_RET"; return 1; }
	ppe_load || { W_RET=; en_fail "offload_sr_status unreadable"; return 1; }
	g2=$M_GEN
	[ "$g2" = "$g" ] || { en_stopped "inhibit_gen $g -> $g2"; return 1; }
	if [ "$only" = cycle ]; then
		en_step switch-disarm
		ctl_write "$SVF" disarm 10 || { en_fail "$W_RET"; return 1; }
	fi
	if [ "$only" != ppe ]; then
		en_step switch-arm
		ctl_write "$SVF" "arm $L" 10 || { en_fail "$W_RET"; return 1; }
	fi
	en_step ppe-arm
	if [ "$offers" = 1 ]; then
		ctl_write "$PSR" "arm $L offers" 10 || { en_fail "$W_RET"; return 1; }
		want=ACTIVE
	else
		ctl_write "$PSR" "arm $L" 10 || { en_fail "$W_RET"; return 1; }
		want=SR_READY
	fi
	en_step verify
	[ "$EN_MODE" = explicit ] && en_hold_clear
	state_read
	if [ "$M_OK" = 1 ] && [ "$M_GEN" != "$g" ]; then
		W_RET=
		EN_CLEARED=0
		rm -f "$RUN/enabling.hold"
		hold_set "stop during enable $(stamp)" persist
		en_fail "stopped (inhibit_gen $g -> $M_GEN)"
		return 1
	fi
	[ "$C" = "$want" ] || { W_RET=; en_fail "state $C, not $want"; return 1; }
	EN_CLEARED=0
	echo "$S_TOPO" > "$RUN/armed_topo_warn"
	uptime_s > "$RUN/renewed"
	touch "$RUN/armed_once"
	rm -f "$RUN/deferred" "$RUN/fault_logged" "$RUN/renew_note"
	if [ "$EN_MODE" = explicit ]; then
		if [ -n "$(pstore_dmesg)" ]; then
			pstore_archive
			rm -f $(pstore_dmesg)
			log notice "hold cleared: dmesg-ramoops records archived to $ARCHIVE and removed"
		fi
		g=$(build_id)
		[ "$g" = "$C_BUILD" ] || uci_put armed_build "$g"
	fi
	rm -f "$RUN/enabling" "$RUN/enabling.hold"
	log notice "enabled $C ($EN_MODE${only:+ $only}) lease $L sw_ttl $S_TTL ppe_ttl $M_TTL scope $M_SCOPE"
}

# disarm_both: the normal disable order (§3.2). DIS_OK, DIS_TXT.
disarm_both() {
	unwind
	DIS_OK=$U_OK DIS_TXT=$U_TXT
}

# --- verbs ------------------------------------------------------------------------

cmd_status() {
	state_read
	if [ "$1" = --raw ]; then
		echo "== svlan_counters"
		cat "$RUN/svc.txt" 2>/dev/null || echo "(unreadable)"
		echo "== offload_sr_status"
		cat "$RUN/pss.txt" 2>/dev/null || echo "(unreadable)"
		return 0
	fi
	echo "combined $C"
	echo "switch bound $S_BOUND abi $S_ABI state $S_STATE sw_state $S_SW fault $S_FAULT accept $S_ACCEPT s_ports $S_PORTS ppe_state $S_PPE disarm_in_s $S_TTL steer_live $S_STEER topo_warn $S_TOPO forced_n $S_FORCED_N forced_iso cpu $S_ISO_CPU phy $S_ISO_PHY"
	echo "ppe state $M_STATE td $M_TD inhibit $M_INH inhibit_gen $M_GEN sw_module_ref $M_REF offload $M_OFFLOAD"
	echo "ppe deadline_in_s $M_TTL switch_disarm_in_s $M_SWTTL scope $M_SCOPE ids $M_IDS accepted $M_ACC"
	echo "ppe last $M_LAST"
	echo "ppe fault $M_FAULT inert_fail $M_INERT"
	[ "$C" = FAULT ] && echo "fault $(fault_which)"
	wired_isolated && echo "wired_isolated (sw forced_ppe: cpu $S_ISO_CPU phy $S_ISO_PHY): ppe-sr rewire once the PPE is off"
	echo "hold $(hold_get)"
	echo "config enabled $C_ENABLED auto_arm $C_AUTO lease $C_LEASE renew $C_RENEW svlan $(cat "$DSAPAR/svlan" 2>/dev/null || echo -)"
	[ -e "$RUN/enabling" ] && echo "enabling $(cat "$RUN/enabling")"
	return 0
}

cmd_check() {
	state_read
	echo "combined $C"
	P_PRINT=1
	preconds auto
}

cmd_scope() {
	[ "$1" = apply ] || die "usage: ppe-sr scope apply"
	lock_take 30 || die "busy"
	scope_build || die "scope refused: $SC_ERR"
	if scope_apply; then
		echo "scope applied: $SC_NCLS class(es), clients $SC_CLIENTS"
	else
		die "scope apply failed: $SC_RET"
	fi
}

cmd_enable() {
	local L=$C_LEASE o=$C_OFFERS

	while [ $# -gt 0 ]; do
		case $1 in
		--lease) L=$2; shift ;;
		--no-offers) o=0 ;;
		*) die "usage: ppe-sr enable [--lease s] [--no-offers]" ;;
		esac
		shift
	done
	isnum "$L" && [ "$L" -ge 90 ] && [ "$L" -le 86400 ] || die "lease $L: 90..86400"
	lock_take 30 || die "busy"
	state_read
	case $C in
	ACTIVE|SR_READY) echo "already $C"; return 0 ;;
	OFF) ;;
	SWITCH_READY)
		ctl_write "$SVF" disarm 10 || die "switch disarm before enable: $W_RET"
		;;
	*) die "state $C: not enabling (ppe-sr status)" ;;
	esac
	enable_run explicit "$L" "$o" || exit 1
	echo "enabled $C"
}

cmd_disable() {
	lock_take 30 || die "busy"
	disarm_both
	if [ "$DIS_OK" = 1 ]; then
		log notice "disabled ($DIS_TXT)"
	else
		log err "unwind failed (disable): $DIS_TXT"
	fi
	if [ "$1" = --persist ]; then
		uci_put enabled 0
		C_ENABLED=0
		modconf_sync
		log notice "disabled persistently: enabled 0, managed svlan line removed; reboot for rtl8_4"
	fi
	[ "$DIS_OK" = 1 ]
}

# renew_note failed|deferred WHY: "renew failed (WHY)" (warning) or "renew
# deferred (WHY)" (info), logged once until the cause changes or a renewal
# succeeds (the daemon tries again every tick). R_TXT too.
renew_note() {
	R_TXT="$1 ($2)"
	[ "$(cat "$RUN/renew_note" 2>/dev/null)" = "$R_TXT" ] && return 0
	echo "$R_TXT" > "$RUN/renew_note"
	if [ "$1" = failed ]; then
		log warning "renew failed ($2)"
	else
		log info "renew deferred ($2)"
	fi
}

# renew_run: the lease (§3.3): switch renew first, then the PPE deadline,
# which the kernel caps 30 s before the switch's. A switch lease that already
# runs longer than "lease" (lowered since the arm, or enable --lease) is not
# renewed until it is within it: the switch refuses a shorter deadline
# (-ERANGE), so the PPE's is left alone too. Returns 0 (renewed), 1 (failed),
# 2 (not armed), 3 (deferred); R_TXT says which.
renew_run() {
	local bad

	state_read
	case $C in ACTIVE|SR_READY) ;; *) R_TXT="state $C"; return 2 ;; esac
	bad=$(cfg_bad)
	if [ -n "$bad" ]; then
		renew_note failed "config $bad"
		return 1
	fi
	if isnum "$S_TTL" && [ "$S_TTL" -gt "$C_LEASE" ]; then
		renew_note deferred "the switch lease ends after lease $C_LEASE s"
		return 3
	fi
	ctl_write "$SVF" "renew $C_LEASE" 10
	case $W_RET in
	0) ;;
	ERANGE)
		renew_note deferred "the switch lease ends after lease $C_LEASE s"
		return 3
		;;
	*)
		renew_note failed "switch $W_RET"
		return 1
		;;
	esac
	ctl_write "$PSR" "deadline $C_LEASE" 10 || { renew_note failed "ppe $W_RET"; return 1; }
	uptime_s > "$RUN/renewed"
	rm -f "$RUN/renew_note"
	R_TXT=ok
}

cmd_renew() {
	lock_take 30 || die "busy"
	renew_run
	echo "renew: $R_TXT"
	case $R_TXT in ok|deferred*) return 0 ;; esac
	return 1
}

# rewire_run: R4, reload rtl8372n_dsa (only with the PPE off and steering
# cleared; the kernel refuses rmmod while the PPE holds S-R anyway). Done
# only when the switch is back with svlan=1, bound 1 abi 1, and the combined
# state is OFF (§5.3).
rewire_run() {
	local pid n=0 why= up=0 v

	state_read 2
	if [ "$M_STATE" != off ]; then
		why="PPE state $M_STATE"
	elif [ "$M_REF" != 0 ]; then
		why="sw_module_ref $M_REF"
	elif [ "$S_OK" = 1 ] && [ "$S_STEER" != 0 ]; then
		why="steer_live $S_STEER"
	fi
	[ -z "$why" ] || { log err "rewire failed: $why"; return 1; }
	if [ -d /sys/module/rtl8372n_dsa ]; then
		rmmod rtl8372n_dsa > "$RUN/rmmod.out" 2>&1 ||
			{ log err "rewire failed: rmmod refused ($(head -1 "$RUN/rmmod.out"))"; return 1; }
	fi
	modprobe rtl8372n_dsa > "$RUN/modprobe.out" 2>&1 9>&- &
	pid=$!
	while [ $n -lt 30 ]; do
		sleep 1
		n=$((n + 1))
		if ! kill -0 "$pid" 2>/dev/null && [ -e "$SVC" ]; then
			sv_load 2
			if [ "$(cat "$DSAPAR/svlan" 2>/dev/null)" != Y ] || [ "$S_BOUND" = 1 ]; then
				up=1
				break
			fi
		fi
	done
	[ $up = 1 ] || { log err "rewire failed: not bound 30 s after modprobe"; return 1; }
	wait "$pid" 2>/dev/null
	v=$(cat "$DSAPAR/svlan" 2>/dev/null)
	if [ "$v" != Y ]; then
		log err "rewire failed: svlan ${v:-?} after the reload (no managed svlan line: enabled 0?)"
		return 1
	fi
	# A status read may time out (killable locks, 2 s): read again, 5 x 1 s.
	n=0
	while :; do
		state_read 2
		[ "$S_BOUND" = 1 ] && [ "$S_ABI" = 1 ] && [ "$C" = OFF ] && break
		if [ $n -ge 5 ]; then
			log err "rewire failed: state $C (bound $S_BOUND abi $S_ABI) after the reload"
			return 1
		fi
		n=$((n + 1))
		sleep 1
	done
	log notice "rewire ok ($C)"
}

cmd_rewire() {
	lock_take 10 || { log err "rewire failed: busy"; exit 1; }
	rewire_run
}

# stop (no flock; WP3-DESIGN §5.3, D10): hold, PPE guard (3 s); 0 -> switch
# disarm; else switch disarm force, then the guard once more; persist the
# hold; rewire once the PPE is off and steering cleared, after a forced rung.
#
# STOP_WAIT_S: how long the forced rung waits for the PPE to be off and the
# follow-up to clear steering. A PPE with no flow in hardware notices the
# switch's FAULT only at its deadline work's next tick (PPE_SR_DEADLINE_STEP_S,
# 30 s), and the follow-up looks every SV_YIELD_PERIOD_S (5 s): 35 s, plus
# 10 s for the PPE's disarm (DEVIATION from §5.3's 30 s). The second guard
# makes it seconds whenever the PPE lock is free by then.
STOP_WAIT_S=45
cmd_stop() {
	local rw=1 g s rung forced=0 t0 t=0 pf=$RUN/stop why ok=1

	[ "$1" = --no-rewire ] && rw=0
	[ "$C_STOPREWIRE" = 1 ] || rw=0
	why="stop $(stamp)"
	hold_set "$why"
	ctl_write "$PSG" 1 3 "$pf-guard.pid"
	g=$W_RET
	if [ "$g" = 0 ]; then
		rung=disarm
		ctl_write "$SVF" disarm 10 "$pf-svlan.pid"
		s=$W_RET
		if [ "$s" = EBUSY ]; then
			# A claim came back after the guard (an enable started after
			# it): guard once more; then the forced rung.
			ctl_write "$PSG" 1 3 "$pf-guard.pid"
			g="$g,$W_RET"
			ctl_write "$SVF" disarm 10 "$pf-svlan.pid"
			s=$W_RET
		fi
		case $s in 0|EMISSING|EOPNOTSUPP) ;; *) forced=1 ;; esac
	else
		forced=1
	fi
	if [ $forced = 1 ]; then
		if [ "$C_STOPFORCE" = nophy ]; then
			rung=force-nophy
			ctl_write "$SVF" "disarm force nophy" 10 "$pf-svlan.pid"
			s=$W_RET
			sv_load 2
			if [ "$S_ISO_CPU" = FAILED ]; then
				rung=force-nophy+force
				ctl_write "$SVF" "disarm force" 10 "$pf-svlan.pid"
				s="$s,$W_RET"
			fi
		else
			rung=force
			ctl_write "$SVF" "disarm force" 10 "$pf-svlan.pid"
			s=$W_RET
		fi
		# The switch is FAULT now: the guard again (bounded, 3 s) turns the
		# PPE off at once if its lock is free, rather than at its next
		# tick. Not over a PPE FAULT (-EIO), nor while the first guard's
		# write is pending (it will land).
		case ${g##*,} in
		EIO) ;;
		*)
			if ! writer_alive "$pf-guard.pid"; then
				ctl_write "$PSG" 1 3 "$pf-guard.pid"
				g="$g,$W_RET"
			fi
			;;
		esac
	fi
	case ${s##*,} in 0|EMISSING|EOPNOTSUPP) ;; *) ok=0 ;; esac
	hold_set "$why" persist
	log err "stop: rung $rung (guard $g, switch $s)"
	echo "rung $rung guard $g switch $s"
	if [ $forced = 1 ] && [ $rw = 1 ]; then
		t0=$(uptime_s)
		while :; do
			ppe_load 2
			sv_load 2
			[ "$M_STATE" = off ] && [ "$S_STEER" = 0 ] && break
			t=$(($(uptime_s) - t0))
			# A PPE FAULT never ends by itself (R5).
			[ "$M_STATE" = FAULT ] || [ $t -ge $STOP_WAIT_S ] && break
			sleep 1
		done
		if [ "$M_STATE" != off ] || [ "$S_STEER" != 0 ]; then
			log err "rewire failed: PPE $M_STATE, steer_live $S_STEER after $t s"
			echo "rewire failed"
			return 1
		fi
		if lock_take 10; then
			rewire_run && echo "rewire ok" || { echo "rewire failed"; return 1; }
		else
			log err "rewire failed: busy"
			echo "rewire failed"
			return 1
		fi
	fi
	[ $ok = 1 ]
}

# fault_note: log a FAULT once and persist the hold (D17), touching nothing else.
fault_note() {
	local w

	w=$(fault_which)
	[ -s "$RUN/fault_logged" ] && [ "$(cat "$RUN/fault_logged")" = "$w" ] && return 0
	echo "$w" > "$RUN/fault_logged"
	log err "FAULT $w"
	[ -n "$(uci -q get ppe_sr.main.hold)" ] || hold_set "fault $w $(stamp)" persist
}

# reconcile_run: WP3-DESIGN §5.5; needs cfg_load and the lock.
reconcile_run() {
	local t L=$C_LEASE

	state_read
	case $C in
	FAULT) fault_note; return 0 ;;
	ACTIVE|SR_READY|DISARMING|TRANSITION|UNAVAILABLE) return 0 ;;
	esac
	if [ "$C" = OFF ]; then
		t=$(last_trigger)
		# A PPE disarm this boot whose SWITCH_READY window passed between
		# two ticks counts as that row would have (DEVIATION DEV-1 of
		# sr/wp3/IMPL-NOTES.md: §5.5's OFF row re-arms on any trigger).
		if [ -e "$RUN/armed_once" ] && [ "${M_LAST%% *}" = disarm ]; then
			case "$BENIGN control guard " in
			*" $t "*) ;;
			*) hold_note "$t"; return 0 ;;
			esac
		fi
		P_PRINT=0
		if ! preconds auto; then
			[ "$P_WHY" = pstore ] && pstore_archive
			pre_note
			return 0
		fi
		rm -f "$RUN/deferred"
		budget_take
		[ -e "$RUN/armed_once" ] && log notice "rearm from OFF (last $t)"
		enable_run auto "$L" "$C_OFFERS"
		return 0
	fi
	# SWITCH_READY: re-arm only after a benign PPE trigger (§5.5).
	t=$(last_trigger)
	case "$BENIGN" in
	*" $t "*) ;;
	*) ctl_write "$SVF" disarm 10; hold_note "$t"; return 0 ;;
	esac
	if [ "$M_INH" != 0 ]; then
		ctl_write "$SVF" disarm 10
		hold_note inhibit
		return 0
	fi
	if ! budget_ok; then
		ctl_write "$SVF" disarm 10
		hold_note budget
		return 0
	fi
	P_PRINT=0
	if ! preconds auto; then
		[ "$P_WHY" = pstore ] && pstore_archive
		case $P_KIND in hold|held) ctl_write "$SVF" disarm 10 ;; esac
		pre_note
		return 0
	fi
	budget_take
	if [ "$S_TOPO" -gt "$(cat "$RUN/armed_topo_warn" 2>/dev/null || echo 0)" ]; then
		log notice "rearm after $t: topo_warn rose, full cycle"
		enable_run auto "$L" "$C_OFFERS" cycle
	elif [ "$S_TTL" -ge 90 ]; then
		log notice "rearm after $t: PPE only, switch ttl $S_TTL"
		enable_run auto "$L" "$C_OFFERS" ppe
	else
		log notice "rearm after $t: full cycle, switch ttl $S_TTL"
		enable_run auto "$L" "$C_OFFERS" cycle
	fi
	return 0
}

cmd_reconcile() {
	lock_take 30 || die "busy"
	reconcile_run
}

# health_line: one line, memory reads only.
health_line() {
	local acc up dn h

	state_read
	acc=${M_ACC#up }
	up=${acc%% *}
	dn=${acc##* }
	h=$(hold_get)
	if wired_isolated; then
		log err "health FAIL wired_isolated (cpu $S_ISO_CPU phy $S_ISO_PHY, PPE $M_STATE)"
	elif [ "$C" = FAULT ]; then
		log err "health FAIL fault $(fault_which)"
	elif [ "$C" = TRANSITION ]; then
		log err "health FAIL transition ($S_STATE/$M_STATE)"
	elif [ "$C" = UNAVAILABLE ] && [ "$C_ENABLED" = 1 ]; then
		log err "health FAIL unavailable (bound $S_BOUND abi $S_ABI ppe $M_OK)"
	else
		log info "health ok $C sw_ttl $S_TTL ppe_ttl $M_TTL ids $M_IDS acc_up $up acc_dn $dn hold ${h:--}"
	fi
}

# ppe_params_set: with ppe_params 1, flow_offload and pppoe_offload to 1 if N.
ppe_params_set() {
	local p

	[ "$C_PPEPARAMS" = 1 ] || return 0
	for p in flow_offload pppoe_offload; do
		[ "$(cat "$PPEPAR/$p" 2>/dev/null)" = N ] || continue
		ctl_write "$PPEPAR/$p" 1 10 && log notice "qcom_ppe.$p set to 1 (ppe_params)" ||
			log warning "qcom_ppe.$p: $W_RET"
	done
}

# daemon (procd): every TICK s, or at once on SIGHUP: reconcile, renew,
# health every 300 s. SIGTERM never disarms.
cmd_daemon() {
	local hup=0 sp= now last_h=-300 params=0 r

	QUIET=1
	echo $$ > "$RUN/daemon.pid"
	trap 'hup=1; [ -n "$sp" ] && kill "$sp" 2>/dev/null' HUP
	trap '[ -n "$sp" ] && kill "$sp" 2>/dev/null; rm -f "$RUN/daemon.pid"; exit 0' TERM INT
	while :; do
		hup=0
		cfg_load
		now=$(uptime_s)
		if [ "$now" -ge "$C_HOLDOFF" ] && iface_up lan && lock_take 0; then
			[ $params = 1 ] || { ppe_params_set; params=1; }
			reconcile_run
			r=$(cat "$RUN/renewed" 2>/dev/null || echo 0)
			# A bad "renew" goes to renew_run, which says so (once).
			if [ "$C_RENEW" != 0 ] && { ! isnum "$C_RENEW" || [ $((now - r)) -ge "$C_RENEW" ]; }; then
				renew_run
			fi
			lock_drop
		fi
		if [ $((now - last_h)) -ge 300 ]; then
			health_line
			last_h=$now
		fi
		[ $hup = 1 ] && continue
		sleep "$TICK" &
		sp=$!
		wait "$sp"
		kill "$sp" 2>/dev/null
		sp=
	done
}

# modconf_sync: the managed svlan line in /etc/modules.conf from "enabled",
# no other line touched (§5.4 (1)); then (2), the running parameter.
modconf_sync() {
	local f=$MODCONF t has=0 v want=N

	[ -f "$f" ] && grep -qxF "$MANAGED" "$f" && has=1
	if [ "$C_ENABLED" = 1 ] && [ $has = 0 ]; then
		t=$f.ppe-sr.$$
		{
			[ -f "$f" ] && cat "$f"
			[ -s "$f" ] && [ -n "$(tail -c1 "$f")" ] && echo
			echo "$MANAGED"
		} > "$t" && mv "$t" "$f"
	elif [ "$C_ENABLED" != 1 ] && [ $has = 1 ]; then
		t=$f.ppe-sr.$$
		grep -vxF "$MANAGED" "$f" > "$t"
		mv "$t" "$f"
	fi
	[ "$C_ENABLED" = 1 ] && want=Y
	v=$(cat "$DSAPAR/svlan" 2>/dev/null)
	[ -n "$v" ] && [ "$v" != "$want" ] && log notice "hold (reboot required for svlan)"
	return 0
}

usage() {
	cat <<-EOF
	usage: ppe-sr <verb>
	  status [--raw]                  combined state and both status files (memory only)
	  check                           the automatic-arm preconditions, dry
	  scope apply                     validate, write, commit and read back the UCI scope
	  enable [--lease s] [--no-offers]  clear the hold, uninhibit, arm switch then PPE
	  disable [--persist]             PPE disarm, then switch disarm
	  renew                           switch renew, then PPE deadline (the lease)
	  rewire                          reload rtl8372n_dsa (PPE off, steering cleared)
	  stop [--no-rewire]              guard, switch disarm or force; hold; rewire
	  reconcile                       the automatic arm and re-arm (daemon)
	  health                          one health line to syslog
	  daemon                          procd's instance
	  modconf                         sync the managed svlan line in $MODCONF
	EOF
}

mkdir -p "$RUN"
verb=$1
[ $# -gt 0 ] && shift
cfg_load
case $verb in
status) cmd_status "$@" ;;
check) cmd_check ;;
scope) cmd_scope "$@" ;;
enable) cmd_enable "$@" ;;
disable) cmd_disable "$@" ;;
renew) cmd_renew ;;
rewire) cmd_rewire ;;
stop) cmd_stop "$@" ;;
reconcile) cmd_reconcile ;;
health) health_line ;;
daemon) cmd_daemon ;;
modconf) modconf_sync ;;
*) usage; exit 1 ;;
esac
