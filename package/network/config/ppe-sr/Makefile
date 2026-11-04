include $(TOPDIR)/rules.mk

PKG_NAME:=ppe-sr
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=François Guerraz <kubrick@fgv6.net>

include $(INCLUDE_DIR)/package.mk

define Package/ppe-sr
  SECTION:=net
  CATEGORY:=Network
  TITLE:=S-R steering controller for the PPE and the rtl8372n switch
  DEPENDS:=@TARGET_qualcommbe_ipq53xx +kmod-dsa-rtl8372n +kmod-qcom-ppe
  PKGARCH:=all
endef

define Package/ppe-sr/description
Arms, renews, disarms and stops S-R hardware offload on the GL.iNet
GL-BE9300: the rtl8372n switch's S-VLAN steering first, then the
Qualcomm PPE's S-R mode, and the reverse to disarm. Provides the ppe-sr
command, an init script whose daemon arms at boot (opt-in), renews the
daily lease, re-arms after a benign disarm and logs health lines, and
the managed "options rtl8372n_dsa svlan=1" line in /etc/modules.conf.
Nothing is enabled by default.
endef

define Build/Compile
endef

define Package/ppe-sr/conffiles
/etc/config/ppe_sr
endef

define Package/ppe-sr/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) ./files/ppe-sr.sh $(1)/usr/sbin/ppe-sr
	$(INSTALL_BIN) ./files/ppe-sr.init $(1)/etc/init.d/ppe-sr
	$(INSTALL_CONF) ./files/ppe-sr.config $(1)/etc/config/ppe_sr
endef

$(eval $(call BuildPackage,ppe-sr))
