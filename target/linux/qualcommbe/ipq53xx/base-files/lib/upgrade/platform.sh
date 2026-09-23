REQUIRE_IMAGE_METADATA=1

platform_do_upgrade() {
	case "$(board_name)" in
	gl.inet,gl-be9300)
		CI_KERNPART="0:HLOS"
		CI_ROOTPART="rootfs"
		emmc_do_upgrade "$1"
		;;
	*)
		echo "Sysupgrade is not supported on your board yet."
		return 1
		;;
	esac
}

platform_check_image() {
	[ "$#" -gt 1 ] && return 1

	case "$(board_name)" in
	gl.inet,gl-be9300)
		return 0
		;;
	*)
		echo "Sysupgrade is not supported on your board yet."
		return 1
		;;
	esac
}

platform_copy_config() {
	case "$(board_name)" in
	gl.inet,gl-be9300)
		emmc_copy_config
		;;
	esac
}
