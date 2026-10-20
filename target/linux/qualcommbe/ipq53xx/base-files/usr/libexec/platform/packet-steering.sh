#!/bin/sh
# Packet steering for the Qualcomm IPQ53xx PPE ports (qcom_ppe / EDMA).
#
# Run the generic steering logic first, so Wi-Fi and everything else is
# configured as usual, then steer the PPE ports over all CPUs.
#
# The PPE hashes received flows over the four EDMA Rx rings and the driver
# pins each ring's interrupt to its own CPU, so different flows already
# arrive on different cores. The generic logic cannot see that: with no
# threaded NAPI on these devices it takes the single-device path and writes
# one single-CPU mask across every rx-* queue of both lan and wan, which
# would funnel the whole datapath back onto one core through the RPS
# backlog. That holds for the WAN. On lan, the switch's rtl8_4 tag hides L3
# from the PPE, so LAN ingress probably all arrives on one ring and RPS does
# all the spreading (package/kernel/rtl837x/TESTING.md, section 7).
#
# A single flow still lands on one ring. Receiving and forwarding it on
# that ring's CPU caps one TCP stream at about 1.5 Gbit/s on a PPPoE WAN.
# With RPS over all CPUs, the ring's CPU only receives the packets and the
# flow hash picks the CPU that forwards them, which is another core three
# times out of four. The driver does not record the Rx ring in the skb, so
# rx-0's mask applies to all traffic of a port; every queue gets the same
# mask anyway. Other devices are left as the generic logic configured them.

steering_flows="$(uci -q get network.@globals[0].steering_flows)"
[ "${steering_flows:-0}" -gt 0 ] && opts="-l $steering_flows"

/usr/libexec/network/packet-steering.uc $opts "$1"

cpu_count() {
	set -- /sys/bus/cpu/devices/cpu*
	echo $#
}

# packet_steering=0 turns RPS off everywhere, the PPE ports included.
mask=0
[ "$1" = 0 ] || mask="$(printf %x $(( (1 << $(cpu_count)) - 1 )))"

for dev in /sys/class/net/*; do
	[ -d "$dev/device" ] || continue
	case "$(basename "$(readlink -f "$dev/device")")" in
	*.ethernet)
		for q in "$dev"/queues/rx-*; do
			[ -f "$q/rps_cpus" ] && echo "$mask" > "$q/rps_cpus"
		done
		;;
	esac
done

exit 0
