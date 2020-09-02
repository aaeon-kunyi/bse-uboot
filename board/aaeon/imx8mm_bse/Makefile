#
# Copyright 2018 NXP
# Copyright 2020 AAEON
#
# SPDX-License-Identifier:      GPL-2.0+
#

obj-y += imx8mm_bse.o

ifdef CONFIG_SPL_BUILD
# for SPL Build
obj-y += spl.o
obj-$(CONFIG_IMX8M_LPDDR4) += lpddr4_timing.o
else
#
obj-y += ../common/mmc.o
endif
