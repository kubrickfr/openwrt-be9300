# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025 StarField Xu <air_jinkela@163.com>

ccflags-y += -Wall
# The kernel turns -Wmaybe-uninitialized off for GCC below W=2, which lets a
# variable set on one path only and read on every path build silently. Turn
# it back on for this module; with CONFIG_WERROR a false positive fails the
# build, so give the variable an explicit value rather than dropping this.
ccflags-$(CONFIG_CC_IS_GCC) += -Wmaybe-uninitialized
# The package option RTL837X_DSA_DEBUG passes CONFIG_RTL837X_DSA_DEBUG=y for
# bench builds: with DEBUG, dev_dbg() prints at KERN_DEBUG even though the
# target kernel has no CONFIG_DYNAMIC_DEBUG (with it, the lines start on).
# EXTRA_CFLAGS cannot carry -DDEBUG: kbuild no longer reads it.
ccflags-$(CONFIG_RTL837X_DSA_DEBUG) += -DDEBUG
# The package option RTL837X_DSA_OFFLOAD passes CONFIG_RTL837X_DSA_OFFLOAD=y
# to make bridge offload the default of the offload module parameter.
ccflags-$(CONFIG_RTL837X_DSA_OFFLOAD) += -DRTL8372N_OFFLOAD_DEFAULT=1

obj-$(CONFIG_RTL837X_DSA) += rtl8372n_dsa.o
rtl8372n_dsa-objs := rtl8372n.o rtl837x_mdio.o rtl837x_common.o \
		     rtl837x_table.o rtl8372n_l2.o rtl8372n_vlan.o
