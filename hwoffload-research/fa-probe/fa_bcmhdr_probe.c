// SPDX-License-Identifier: GPL-2.0
/*
 * fa_bcmhdr_probe.c - Read-only probe of FA's bcm_hdr_ctl register
 * (FA_BASE_OFFSET+0x08, per fa_core.h's faregs_t layout) - controls
 * whether FA's own hardware "Broadcom header" hit/miss-tagging mechanism
 * is active. Never read or written by any prior module in this project
 * (grep confirmed - fa_probe.c/fa_bringup.c only ever touch control/
 * status/mem_acc_ctl). Pure read, same safety class as fa_probe.c
 * (already proven safe, repeatedly, against this exact MMIO window).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/errno.h>

#define FA_PHYS_BASE	0x18027c00UL
#define FA_MAP_SIZE	0x100UL

#define FA_REG_CONTROL		0x00
#define FA_REG_MEM_ACC_CTL	0x04
#define FA_REG_BCM_HDR_CTL	0x08
#define FA_REG_L2_SKIP_CTL	0x0c
#define FA_REG_STATUS		0x30

static int __init fa_bcmhdr_probe_init(void)
{
	void __iomem *b = ioremap(FA_PHYS_BASE, FA_MAP_SIZE);

	if (!b) {
		pr_err("fa_bcmhdr_probe: ioremap failed\n");
		return -ENOMEM;
	}

	pr_info("fa_bcmhdr_probe: control=0x%08x mem_acc_ctl=0x%08x "
		"bcm_hdr_ctl=0x%08x l2_skip_ctl=0x%08x status=0x%08x\n",
		readl(b + FA_REG_CONTROL), readl(b + FA_REG_MEM_ACC_CTL),
		readl(b + FA_REG_BCM_HDR_CTL), readl(b + FA_REG_L2_SKIP_CTL),
		readl(b + FA_REG_STATUS));

	iounmap(b);
	return 0;
}

static void __exit fa_bcmhdr_probe_exit(void)
{
	pr_info("fa_bcmhdr_probe: unloaded\n");
}

module_init(fa_bcmhdr_probe_init);
module_exit(fa_bcmhdr_probe_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hwoffload-research");
MODULE_DESCRIPTION("Read-only probe of FA's bcm_hdr_ctl register");
