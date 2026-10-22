#!/bin/sh
# Packet steering for the Qualcomm IPQ53xx PPE ports (qcom_ppe / EDMA).
#
# Run the generic steering logic first, so Wi-Fi and everything else is
# configured as usual, then steer the PPE ports.
#
# The PPE hashes received flows over the EDMA Rx rings and the driver pins
# each ring's interrupt to its own CPU, so different flows already arrive
# on different cores. The generic logic cannot see that: with no threaded
# NAPI on these devices it takes the single-device path and writes one
# single-CPU mask across every rx-* queue of both lan and wan, which would
# funnel the whole datapath back onto one core through the RPS backlog.
# That holds for the WAN. On lan, the switch's rtl8_4 tag hides L3 from the
# PPE, so nearly all LAN ingress arrives on one ring and RPS does the
# spreading (package/kernel/rtl8372n/TESTING.md, section 7).
#
# A single flow lands on one ring. Receiving and forwarding it on that
# ring's CPU caps one TCP stream at about 1.45 Gbit/s on a PPPoE WAN, and
# with RPS over all CPUs that happens one time in four. With four CPUs and
# both lan and wan on the PPE, split the cores instead: the Rx rings on
# CPU0-1 (net.edma.rps_bitmap_cores), WAN traffic steered to CPU2-3, so the
# core forwarding a WAN flow never also receives it, and LAN traffic to
# CPU0-1, so the LAN-side ACKs of a download stay off the forwarding cores.
# Measured on a GL-BE9300 (2026-10-21, one PPPoE download, fixed source
# ports): 7 of 8 runs at 2.00-2.16 Gbit/s (the eighth was server-limited),
# against 1.43-2.13 with RPS over all CPUs; multi-stream, upload and Wi-Fi
# were not measured separately.
#
# The driver does not record the Rx ring in the skb, so rx-0's mask applies
# to all traffic of a port; every queue gets the same mask anyway. Other
# devices are left as the generic logic configured them.

steering_flows="$(uci -q get network.@globals[0].steering_flows)"
[ "${steering_flows:-0}" -gt 0 ] && opts="-l $steering_flows"

/usr/libexec/network/packet-steering.uc $opts "$1"

cpu_count() {
	set -- /sys/bus/cpu/devices/cpu*
	echo $#
}

ppe_port() {
	[ -d "/sys/class/net/$1/device" ] || return 1
	case "$(basename "$(readlink -f "/sys/class/net/$1/device")")" in
	*.ethernet) return 0 ;;
	esac
	return 1
}

all="$(printf %x $(( (1 << $(cpu_count)) - 1 )))"
rings="$all" mask="$all" lan_mask="$all" wan_mask="$all"

# packet_steering=0 turns RPS off everywhere, the PPE ports included, and
# spreads the Rx rings over every core again.
if [ "$1" = 0 ]; then
	mask=0 lan_mask=0 wan_mask=0
elif [ "$all" = f ] && ppe_port lan && ppe_port wan; then
	rings=3 lan_mask=3 wan_mask=c
fi

# Written only on a change: the handler reprograms the PPE hash map even
# when the value is the same, and this runs on every interface event.
bitmap=/proc/sys/net/edma/rps_bitmap_cores
[ -w "$bitmap" ] && [ "$(cat "$bitmap")" != "$((0x$rings))" ] &&
	echo "$((0x$rings))" > "$bitmap"

# The split only holds if the rings really moved; otherwise steer as before.
[ "$(cat "$bitmap" 2>/dev/null)" = "$((0x$rings))" ] || {
	lan_mask="$mask" wan_mask="$mask"
}

for dev in /sys/class/net/*; do
	ppe_port "${dev##*/}" || continue
	case "${dev##*/}" in
	lan) m="$lan_mask" ;;
	wan) m="$wan_mask" ;;
	*) m="$mask" ;;
	esac
	for q in "$dev"/queues/rx-*; do
		[ -f "$q/rps_cpus" ] && echo "$m" > "$q/rps_cpus"
	done
done

exit 0
