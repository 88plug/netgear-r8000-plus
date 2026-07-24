// SPDX-License-Identifier: GPL-2.0
/*
 * fa_stats_probe.c - Read-only probe of FA's hit/miss/error counters,
 * used as the before/after oracle for the live=1 real-forwarding test
 * (see BRINGUP_RESULT.md). Pure reads, same safety class as fa_probe.c.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/errno.h>

#define FA_PHYS_BASE	0x18027c00UL
#define FA_MAP_SIZE	0x100UL

#define FA_REG_CONTROL	0x00
#define FA_REG_STATUS	0x30
#define FA_REG_STAT_HIT		0x3c
#define FA_REG_STAT_MISS	0x40
#define FA_REG_ERROR		0x64
#define FA_REG_ECC_ERROR	0x7c

static int __init fa_stats_probe_init(void)
{
	void __iomem *b = ioremap(FA_PHYS_BASE, FA_MAP_SIZE);

	if (!b) {
		pr_err("fa_stats_probe: ioremap failed\n");
		return -ENOMEM;
	}

	pr_info("fa_stats_probe: control=0x%08x status=0x%08x hit=%u miss=%u error=0x%08x ecc_error=0x%08x\n",
		readl(b + FA_REG_CONTROL), readl(b + FA_REG_STATUS),
		readl(b + FA_REG_STAT_HIT), readl(b + FA_REG_STAT_MISS),
		readl(b + FA_REG_ERROR), readl(b + FA_REG_ECC_ERROR));

	iounmap(b);
	return 0;
}

static void __exit fa_stats_probe_exit(void)
{
	pr_info("fa_stats_probe: unloaded\n");
}

module_init(fa_stats_probe_init);
module_exit(fa_stats_probe_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hwoffload-research");
MODULE_DESCRIPTION("Read-only FA hit/miss/error stats probe");
