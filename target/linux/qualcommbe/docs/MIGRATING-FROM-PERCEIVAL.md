# Migrating from perceival/openwrt-flint3 images

Moved from the README.

This tree keeps upstream OpenWrt's DHCP setup: dnsmasq serves DHCPv4 and DNS,
and odhcpd-ipv6only serves RA and DHCPv6. perceival's router images run full
odhcpd as the DHCPv4 server (`dhcp.odhcpd.maindhcp=1`), reportedly with no
dnsmasq section. This section applies to any kept configuration with
`maindhcp=1`. odhcpd-ipv6only has no DHCPv4 and silently ignores
`dhcpv4='server'`, and dnsmasq generates no DHCP ranges while `maindhcp=1`.
A kept configuration therefore leaves the LAN **without DHCPv4**, and without
DNS if there is no dnsmasq section. Before flashing, make sure you can reach
the router with a static address.

**Option A — upstream defaults**, run after the first boot. If unbound
serves your DNS, read the unbound paragraph below before running it.

```sh
uci set dhcp.odhcpd.maindhcp='0'
uci -q show dhcp | grep -q '=dnsmasq$' || {
	uci add dhcp dnsmasq >/dev/null
	for o in domainneeded=1 boguspriv=1 localise_queries=1 rebind_protection=1 \
		rebind_localhost=1 local=/lan/ domain=lan expandhosts=1 cachesize=1000 \
		authoritative=1 readethers=1 leasefile=/tmp/dhcp.leases \
		resolvfile=/tmp/resolv.conf.d/resolv.conf.auto nonwildcard=1 \
		localservice=1 ednspacket_max=1232; do
		uci set dhcp.@dnsmasq[0].$o
	done
}
uci set dhcp.lan.dhcpv4='server'
uci commit dhcp && /etc/init.d/odhcpd restart && /etc/init.d/dnsmasq restart
```

Also set `dhcpv4='server'` on every other `config dhcp` section that should
serve DHCPv4: dnsmasq treats a missing option as disabled (the defaults above
are those of `package/network/services/dnsmasq/files/dhcp.conf`). Check that
`/var/etc/dnsmasq.conf.*` now has a `dhcp-range=set:lan,…` line.

If unbound serves DNS, use the "parallel dnsmasq" setup from unbound's
README (`feeds/packages/net/unbound/files/README.md`): unbound keeps port 53
and asks dnsmasq, moved to port 1053, for the names of DHCP clients. Before
the `uci commit dhcp` line above:

```sh
uci set dhcp.@dnsmasq[0].port='1053'
uci set dhcp.@dnsmasq[0].noresolv='1'
uci add_list dhcp.lan.dhcp_option='option:dns-server,0.0.0.0'
uci set unbound.@unbound[0].dhcp_link='dnsmasq'
uci commit unbound
```

and afterwards `/etc/init.d/unbound restart`. Add the same `dhcp_option` to
every other section that serves DHCPv4.

**Option B — keep odhcpd as the DHCPv4 server:** build with
`CONFIG_PACKAGE_odhcpd=y`, which replaces odhcpd-ipv6only in the image
(upstream `1f7eb34c53`; checked with `make defconfig` on 2026-10-05). Then
`maindhcp=1` configurations work unchanged, but DNS still needs a server:
unbound, or a dnsmasq section (which then serves DNS only). Packages
installed with apk do not survive a sysupgrade, so build it in.
