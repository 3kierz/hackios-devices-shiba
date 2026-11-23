// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung's UFS CAL for zuma (the Pixel 8's PHY and UniPro set-up tables and the code that writes them), built into
 * HakiOS's U-Boot as Google's kernel builds it: zuma-cal/ is google-modules/soc/gs drivers/ufs/zuma/ (branch
 * android-gs-shusky-6.1-android16-beta) unchanged, GPL-2.0-only, Copyright 2022 Samsung Electronics. Its kernel branch
 * (neither __UFS_CAL_LK__ nor __UFS_CAL_FW__) needs readl/writel, the kernel's integer types and pr_info, all of which
 * U-Boot has; hakios_zuma_ufs.c calls it exactly where Google's ufs-exynos.c does.
 */

#include <log.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <asm/io.h>

#include "zuma-cal/ufs-cal-if.c"
