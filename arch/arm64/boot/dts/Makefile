# SPDX-License-Identifier: GPL-2.0

dtb-$(CONFIG_MACH_EXYNOS7885_A7Y18VE_JPN_RKT) := \
	exynos/dtbo/exynos7885.dtb \

dtbo-$(CONFIG_MACH_EXYNOS7885_A7Y18VE_JPN_RKT) := \
	exynos/dtbo/exynos7885-a7y18_jpn_rkt_00.dtbo \

targets += dtbs
DTB_LIST  := $(dtb-y) $(dtbo-y)
always    := $(DTB_LIST)

dtbs: $(addprefix $(obj)/, $(DTB_LIST))

clean-files := *.dtb*
