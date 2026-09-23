define Build/fit-inline-rootfs
	rm -f $@.dtb $@.kernel
	cp $@ $@.kernel
	cp $(word 2,$(1)) $@.dtb
	cp $@.kernel $@
	$(call Build/fit-its,$(word 1,$(1)) $@.dtb with-rootfs)
	$(call Build/fit-image,$(word 1,$(1)) $@.dtb with-rootfs)
	kernel_size="$$(stat -c%s $@.kernel)"; \
	rootfs_offset="$$(grep -oba hsqs $@ | \
		awk -F: -v limit="$$kernel_size" '$$1 >= limit {print $$1; exit}')"; \
	[ -n "$$rootfs_offset" ] || { echo "Failed to locate SquashFS in $@"; exit 1; }; \
	pad="$$(( (4096 - ($$rootfs_offset % 4096)) % 4096 ))"; \
	cp $(word 2,$(1)) $@.dtb; \
	dd if=/dev/zero bs=1 count="$$pad" >> $@.dtb 2>/dev/null; \
	cp $@.kernel $@; \
	$(call Build/fit-its,$(word 1,$(1)) $@.dtb with-rootfs)
	$(call Build/fit-image,$(word 1,$(1)) $@.dtb with-rootfs)
	kernel_size="$$(stat -c%s $@.kernel)"; \
	rootfs_offset="$$(grep -oba hsqs $@ | \
		awk -F: -v limit="$$kernel_size" '$$1 >= limit {print $$1; exit}')"; \
	[ "$$(( $$rootfs_offset % 4096 ))" -eq 0 ] || { echo "SquashFS is misaligned in $@"; exit 1; }; \
	rm -f $@.dtb $@.kernel
endef

define Device/glinet_gl-be9300
	$(call Device/FitImage)
	$(call Device/EmmcImage)
	DEVICE_VENDOR := GL.iNet
	DEVICE_MODEL := GL-BE9300
	DEVICE_ALT0_VENDOR := GL.iNet
	DEVICE_ALT0_MODEL := Flint 3
	# Stock U-Boot has no AP-MI01.6 entry in its board->config table, so
	# bootipq falls back to asking for "config-1". Any other name - a
	# board-specific config@mi01.6, or OpenWrt's own default config@1 -
	# fails with "Config not available" and the unit will not boot from
	# eMMC.
	DEVICE_DTS_CONFIG := config-1
	SOC := ipq5332
	SUPPORTED_DEVICES += gl.inet,gl-be9300
	DEVICE_PACKAGES := kmod-ath12k ath12k-firmware-ipq5332-legacy \
		ath12k-firmware-qcn9274 ipq-wifi-glinet_gl-be9300 \
		kmod-hwmon-pwmfan kmod-qrtr-smd kmod-rtl837x-dsa \
		kmod-phy-realtek ethtool e2fsprogs f2fsck mkf2fs \
		kmod-usb-core kmod-usb2 kmod-usb3 kmod-usb-dwc3 \
		kmod-usb-dwc3-qcom kmod-usb-xhci-hcd kmod-scsi-core \
		kmod-usb-storage kmod-usb-storage-uas kmod-fs-ext4 \
		block-mount blockd usbutils
endef
TARGET_DEVICES += glinet_gl-be9300
