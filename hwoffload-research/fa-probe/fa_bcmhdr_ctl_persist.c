// SPDX-License-Identifier: GPL-2.0
/*
 * fa_bcmhdr_ctl_persist.c - Writes FA's own bcm_hdr_ctl register
 * (FA_BASE_OFFSET+0x08) with the exact value Broadcom's fa_up() uses
 * when HW_HASH() is true (etc_fa.c:980-984):
 *   CTF_BRCM_HDR_PARSE_IGN_EN | CTF_BRCM_HDR_HW_EN |
 *   CTF_BRCM_HDR_SW_RX_EN | CTF_BRCM_HDR_SW_TX_EN  = 0xF
 *
 * Confirmed via fa_bcmhdr_probe.c (FINDINGS.md #64): this register reads
 * 0x00000000 today - completely untouched, unlike the switch-side
 * OOB-pause/BRCM_HDR-tag registers (both already satisfied - #61/#63).
 *
 * Same direct-ioremap access as fa_probe.c/fa_accel.c (this is FA's own
 * MMIO block, not the SRAB-accessed switch registers). Read-before-write,
 * held until rmmod, reverted to the exact pre-write value on unload -
 * same pattern as every other _persist module in this project.
 *
 * This write is NOT persisted to flash/config in any way - it is a live
 * MMIO register write via insmod, gone on the next reboot regardless of
 * how this test goes.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/errno.h>

#define FA_PHYS_BASE		0x18027c00UL
#define FA_MAP_SIZE		0x100UL
#define FA_REG_BCM_HDR_CTL	0x08

#define CTF_BRCM_HDR_HW_EN		(1U << 0)
#define CTF_BRCM_HDR_SW_RX_EN		(1U << 1)
#define CTF_BRCM_HDR_SW_TX_EN		(1U << 2)
#define CTF_BRCM_HDR_PARSE_IGN_EN	(1U << 3)
#define FA_BCM_HDR_VAL	(CTF_BRCM_HDR_HW_EN | CTF_BRCM_HDR_SW_RX_EN | \
			 CTF_BRCM_HDR_SW_TX_EN | CTF_BRCM_HDR_PARSE_IGN_EN)

static void __iomem *fa_base;
static u32 bcm_hdr_ctl_before;

static int __init fa_bcmhdr_ctl_persist_init(void)
{
	fa_base = ioremap(FA_PHYS_BASE, FA_MAP_SIZE);
	if (!fa_base) {
		pr_err("fa_bcmhdr_ctl_persist: ioremap failed\n");
		return -ENOMEM;
	}

	bcm_hdr_ctl_before = readl(fa_base + FA_REG_BCM_HDR_CTL);
	pr_info("fa_bcmhdr_ctl_persist: bcm_hdr_ctl before = 0x%08x\n",
		bcm_hdr_ctl_before);

	writel(FA_BCM_HDR_VAL, fa_base + FA_REG_BCM_HDR_CTL);

	pr_info("fa_bcmhdr_ctl_persist: bcm_hdr_ctl set to 0x%08x (readback=0x%08x) "
		"- staying active until this module is rmmod'd\n",
		FA_BCM_HDR_VAL, readl(fa_base + FA_REG_BCM_HDR_CTL));

	return 0;
}

static void __exit fa_bcmhdr_ctl_persist_exit(void)
{
	if (fa_base) {
		writel(bcm_hdr_ctl_before, fa_base + FA_REG_BCM_HDR_CTL);
		pr_info("fa_bcmhdr_ctl_persist: reverted bcm_hdr_ctl to 0x%08x\n",
			bcm_hdr_ctl_before);
		iounmap(fa_base);
		fa_base = NULL;
	}
	pr_info("fa_bcmhdr_ctl_persist: unloaded\n");
}

module_init(fa_bcmhdr_ctl_persist_init);
module_exit(fa_bcmhdr_ctl_persist_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hwoffload-research");
MODULE_DESCRIPTION("bcm_hdr_ctl set-and-hold (revert on rmmod) - the last untested piece of fa_up()");
