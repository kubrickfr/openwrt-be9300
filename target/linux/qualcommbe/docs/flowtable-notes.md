# Software flowtable and switch ports

Notes moved from the README. They apply to the software flowtable: with
software offloading, and with hardware offloading for every flow the PPE
declines. The PPE side is in `ppe-flow-offload/`.

- With hardware offloading, fw4 builds the flowtable on `lan1`–`lan4`, the
  Wi-Fi interfaces and `wan` instead of `br-lan`. Pending patch 701 lets it
  mix devices that offload with devices that do not. Flows to wired clients
  that the PPE declines are sent straight to a switch port in software.
- With software offloading the flowtable lists `br-lan`, and since
  OpenWrt's backport of "netfilter: flowtable: bail out if forward path
  cannot be discovered" (706-03) a flow through `br-lan` is not offloaded at
  all: flows to LAN and Wi-Fi clients take the normal forwarding path. fw4
  briefly listed the ports in software mode too, but reverted that
  (firewall4 issue #114); OpenWrt is working on a kernel fix
  (openwrt/openwrt#26305) and a new fw4 change (firewall4 PR #116).
- Do not combine flow offloading with SQM on a PPPoE or VLAN WAN: the
  flowtable catches downloads on the device below (`wan`), so they skip a
  download (ingress) shaper on `pppoe-wan` or on the VLAN device. Uploads
  are still shaped (except those the PPE forwards, below), and so are both
  directions with SQM on a plain `wan`. With `flow_offloading_hw=1` fw4
  also lists the device below a VLAN WAN, which is the case of firewall4
  issue #114. The downloads the PPE forwards skip every shaper, on any
  WAN, and so do the uploads it forwards in the experimental S-R mode.
- Pending patch 716 marks the frames the flowtable sends straight to a
  switch port for TX forwarding offload, as the bridge does for the frames
  it forwards, so that the switch delivers them from its own FDB; for
  software flowtable traffic (nftables, and iptables `FLOWOFFLOAD`) it
  replaced patch 766, which did the same inside the tagger for every
  unmarked unicast frame. Pending patches 767-01/02 keep the tagger from
  dereferencing a missing or freed bridge if a marked frame reaches a port
  that just left its bridge. All three serve the previous `tag_8021q`
  driver, whose bridge FDB had every wired client on the first forwarding
  port. With the `rtl8_4` driver the bridge sees each frame's real ingress
  port and the driver offers no TX forwarding offload: 716 marks nothing,
  767-02 (the `tag_vsc73xx_8021q` half) never runs, and 767-01 still runs in
  the DSA core, though no tagger reads the bridge. A flow sent straight to a
  port leaves by the port the bridge FDB named when the flow was set up.
- A flow sent straight to a port used to stay pinned to it: in testing with
  766, a download to a client on `lan1` stalled for as long as `lan4` was
  down, because the bridge had learned the client on `lan4`. Pending patch
  717 tears flows down when their port loses carrier or joins or leaves a
  bridge, so the next packet takes the bridge and the flow is set up again.
  OpenWrt's own pending patch 708 (tear down direct xmit flows when the fdb
  entry moves) also tears a flow down at the next flowtable gc pass once the
  bridge learns its destination on another port (nftables flowtables only:
  the iptables `FLOWOFFLOAD` target does not record the bridge path).
  717's carrier-loss case ran on hardware on 2026-10-27 (then numbered 709,
  before this tree carried OpenWrt's 708), with PPE offload on: a client
  re-cabled from `lan2` to `lan3` mid-download had its flows set up again
  through `lan3` (`ppe-flow-offload/TESTING.md` section 0). The mock-ISP
  bench repeated it and also moved a client between two ports that both
  stayed up (OpenWrt's 708 with the PPE's FDB follower): both flows went
  back into hardware on the new port (section 0b). 717's bridge join and
  leave cases with flows in hardware are not tested. 716 and 767-01/02
  have nothing to do with the `rtl8_4` driver, as said above.
- Known limit of sending flows straight to the ports: a station that moves
  to another switch port, or between a switch port and the router's own
  Wi-Fi, without changing address and while its old port keeps carrier,
  gets nothing on its existing flows until the bridge learns it on the new
  port and OpenWrt's 708 tears them down at the next flowtable gc pass (with
  iptables `FLOWOFFLOAD`, until they go idle). With the `rtl8_4` driver the
  bridge learns each wired client's real port, so moves between switch
  ports are followed (tested on the bench, `ppe-flow-offload/TESTING.md`
  section 0b). A move to or from Wi-Fi is not tested.
- Bypassing the bridge was worth a few percent on a single stream: with
  `lan1`–`lan4` in the software flowtable, one PPPoE download went from ~1.84
  to ~1.92–2.08 Gbit/s (2026-10-09, RPS on the WAN). That was measured with
  `flow_offloading_hw=0`, the ports added by hand, 766 and the previous
  switch driver, before 716 replaced 766. The ~1.84 baseline had `br-lan` in
  the flowtable, which this tree no longer offloads: with
  `flow_offloading_hw=0` the difference is now offload or none.
- The flowtable keys on a frame's VLAN ID, not its whole tag, since the fix
  OpenWrt backports as 706-04 (carried here as 651-01 before that). Before
  it, LAN uploads that the managed switch tagged with a priority missed the
  software flowtable and took the slow path.

## Earlier measurements

Kept for history; they do not describe the current tree.

- With software flowtable offload, `br-lan` in the flowtable
  (`flow_offloading_hw=0`, a path this tree no longer offloads) and the
  core split in `packet-steering.sh`: one TCP download at 2.00-2.16 Gbit/s
  (7 of 8 runs, 2026-10-21), the forwarding core about 90% busy.
- With the previous, SDK-based switch driver (`tag_8021q`): between two
  units over a 2.5G trunk, about 1.8-1.9 Gbit/s; 2.04 Gbit/s down with 4
  TCP streams (PPE clock 200 MHz, 2026-10-04).
- PPE offload before the flowtable VLAN-ID fix (then the backport 651-01,
  now OpenWrt's 706-04; 2026-10-23): the downloads' ACKs took the slow
  path, 43% of one core for one stream at 2.0 Gbit/s and 56% for Ookla's
  multi-stream download.
