#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=ath12k-firmware-legacy
PKG_VERSION:=1.6.1270
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

RSTRIP:=:
STRIP:=:

define Package/ath12k-firmware-ipq5332-legacy
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=IPQ5332 ath12k firmware (WLAN.WBE.1.6-01270, split MDT)
  CONFLICTS:=ath12k-firmware-ipq5332
endef

define Package/ath12k-firmware-ipq5332-legacy/description
  Split .mdt/.bNN IPQ5332 WCSS firmware (WLAN.WBE.1.6-01270) for the
  on-SoC 2.4 GHz radio. No IPQ5332 firmware is published in
  linux-firmware yet; this build needs the legacy firmware support of
  the ath12k driver. See PROVENANCE for its origin.
endef

define Build/Compile
endef

define Package/ath12k-firmware-ipq5332-legacy/install
	$(INSTALL_DIR) $(1)/lib/firmware/ath12k/IPQ5332/hw1.0
	$(INSTALL_DATA) \
		$(PKG_BUILD_DIR)/IPQ5332/hw1.0/*.mdt \
		$(PKG_BUILD_DIR)/IPQ5332/hw1.0/*.b[0-9][0-9] \
		$(1)/lib/firmware/ath12k/IPQ5332/hw1.0/
endef

$(eval $(call BuildPackage,ath12k-firmware-ipq5332-legacy))
