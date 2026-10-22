include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=rtl8372n
PKG_VERSION:=0.0.2
PKG_RELEASE:=10
PKG_MAINTAINER:=air jinkela (air_jinkela@163.com)
PKG_CONFIG_DEPENDS:=CONFIG_RTL837X_DSA_DEBUG CONFIG_RTL837X_DSA_OFFLOAD

include $(INCLUDE_DIR)/package.mk

define KernelPackage/dsa-rtl8372n
  SUBMENU:=Network Devices
  TITLE:=Realtek RTL8372N switch DSA support
  # The switch uses its native RTL8_4 DSA tag.
  KCONFIG:=CONFIG_NET_DSA_TAG_RTL8_4
  FILES:= \
	$(PKG_BUILD_DIR)/rtl8372n_dsa.ko \
	$(LINUX_DIR)/net/dsa/tag_rtl8_4.ko
  AUTOLOAD:=$(call AutoLoad,17,rtl8372n_dsa)
  # Needs the qualcommbe EDMA checksum fallback (target patch 0431).
  DEPENDS:=@TARGET_qualcommbe +kmod-dsa +kmod-phy-realtek +kmod-regmap-core +kmod-mdio-devres
  # kmod-dsa-rtl8365mb packages tag_rtl8_4.ko too.
  CONFLICTS:=kmod-dsa-rtl8365mb
endef

define KernelPackage/dsa-rtl8372n/description
  DSA driver for the Realtek RTL8372N switch (4x 2.5G + 10G uplink), as
  on the GL.iNet GL-BE9300, with bridge and VLAN offload.
endef

define KernelPackage/dsa-rtl8372n/config
  if PACKAGE_kmod-dsa-rtl8372n

	config RTL837X_DSA_DEBUG
		bool "Print the driver's debug messages (bench builds)"
		default n
		help
		  Build rtl8372n_dsa with -DDEBUG, so that its dev_dbg()
		  lines reach the kernel log without CONFIG_DYNAMIC_DEBUG:
		  bridge joins and leaves, VLAN filtering and VLAN changes,
		  STP states, FDB entries and flushes, the L2 table clear.
		  The hardware bench checks rely on them. Every bridge, VLAN
		  or FDB change logs a line; leave this off in production.

	config RTL837X_DSA_OFFLOAD
		bool "Offload bridging to the switch by default"
		default y
		help
		  Sets the default of the rtl8372n_dsa "offload" module
		  parameter. On, one bridge and its VLANs are switched in
		  hardware; off, every bridge on the LAN ports runs in
		  software and each port reaches only the CPU. Either
		  default is overridden at boot by "options rtl8372n_dsa
		  offload=0" (or =1) in /etc/modules.conf. Turn it off for
		  phase 1 of the bench plan (TESTING.md).

  endif
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) \
		M="$(PKG_BUILD_DIR)" \
		EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
		CONFIG_RTL837X_DSA=m \
		CONFIG_RTL837X_DSA_DEBUG=$(if $(CONFIG_RTL837X_DSA_DEBUG),y) \
		CONFIG_RTL837X_DSA_OFFLOAD=$(if $(CONFIG_RTL837X_DSA_OFFLOAD),y) \
		modules
endef

$(eval $(call KernelPackage,dsa-rtl8372n))
