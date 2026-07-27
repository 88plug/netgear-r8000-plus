// SPDX-License-Identifier: GPL-2.0
/*
 * fa_switch_brcmhdr_persist.c - The OTHER half of robo_fa_enable() that was
 * never actually tried before FINDINGS.md #62/#63: PAGE_MMR(0x02) /
 * REG_BRCM_HDR(0x03), which enables Broadcom-header tagging on the switch's
 * IMP (CPU-facing) port. Per bcmrobo.c (graveyard-vendor/notes.md §4),
 * robo_fa_enable(robo, on, bhdr) is exactly TWO register writes:
 *   1. PAGE_FC(0x0a)/REG_FC_OOBPAUSE(0xe0) bit 8 - already done by
 *      fa_switch_oobpause_test.c/fa_switch_oobpause_persist.c.
 *   2. PAGE_MMR(0x02)/REG_BRCM_HDR(0x03) - THIS file. Never attempted
 *      before.
 *
 * Without BRCM-HDR tagging enabled, the switch has no mechanism to signal
 * "check this packet against FA" to the GMAC/FA side at all - #62's
 * hit=0 result (with only OOB-pause done, no BRCM-HDR) is fully consistent
 * with this being the missing piece, not proof the whole approach is dead.
 *
 * Same SRAB access protocol as fa_switch_oobpause_persist.c (verbatim -
 * proven safe, already exercised repeatedly this project). Read-before-
 * write, OR in bit 0 rather than writing a literal replacement value (safe
 * either way: if the register is currently 0, OR-ing bit 0 in is
 * identical to "= 1"; if other bits are already meaningfully set, this
 * preserves them rather than clobbering). Reverts to the exact pre-write
 * value on rmmod, same pattern as the OOB-pause persist module.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/delay.h>

#define SRAB_PHYS_BASE	0x18007000UL
#define SRAB_MAP_SIZE	0x1000UL

#define B53_SRAB_CMDSTAT	0x2c
#define B53_SRAB_WD_L		0x34
#define B53_SRAB_RD_L		0x3c
#define B53_SRAB_CTRLS		0x40

#define B53_SRAB_CMDSTAT_WRITE		(1U << 1)
#define B53_SRAB_CMDSTAT_GORDYN		(1U << 0)
#define B53_SRAB_CMDSTAT_PAGE_SHIFT	24
#define B53_SRAB_CMDSTAT_REG_SHIFT	16

#define B53_SRAB_CTRLS_RCAREQ		(1U << 3)
#define B53_SRAB_CTRLS_RCAGNT		(1U << 4)

#define PAGE_MMR		0x02U
#define REG_BRCM_HDR		0x03U
#define BRCM_HDR_ENABLE_BIT	(1U << 0)

#define SRAB_GRANT_POLL_STEPS	20
#define SRAB_OP_POLL_STEPS	5

static void __iomem *srab_base;
static u16 brcmhdr_before;

static int srab_request_grant(void)
{
	u32 ctrls;
	int i;

	ctrls = readl(srab_base + B53_SRAB_CTRLS);
	ctrls |= B53_SRAB_CTRLS_RCAREQ;
	writel(ctrls, srab_base + B53_SRAB_CTRLS);

	for (i = 0; i < SRAB_GRANT_POLL_STEPS; i++) {
		ctrls = readl(srab_base + B53_SRAB_CTRLS);
		if (ctrls & B53_SRAB_CTRLS_RCAGNT)
			return 0;
		usleep_range(10, 100);
	}
	return -EIO;
}

static void srab_release_grant(void)
{
	u32 ctrls = readl(srab_base + B53_SRAB_CTRLS);

	ctrls &= ~B53_SRAB_CTRLS_RCAREQ;
	writel(ctrls, srab_base + B53_SRAB_CTRLS);
}

static int srab_op(u8 page, u8 reg, u32 op)
{
	u32 cmdstat;
	int i;

	cmdstat = ((u32)page << B53_SRAB_CMDSTAT_PAGE_SHIFT) |
		  ((u32)reg << B53_SRAB_CMDSTAT_REG_SHIFT) |
		  B53_SRAB_CMDSTAT_GORDYN | op;
	writel(cmdstat, srab_base + B53_SRAB_CMDSTAT);

	for (i = 0; i < SRAB_OP_POLL_STEPS; i++) {
		cmdstat = readl(srab_base + B53_SRAB_CMDSTAT);
		if (!(cmdstat & B53_SRAB_CMDSTAT_GORDYN))
			return 0;
		usleep_range(10, 100);
	}
	return -EIO;
}

static int srab_read16(u8 page, u8 reg, u16 *val)
{
	int ret = srab_request_grant();

	if (ret)
		return ret;
	ret = srab_op(page, reg, 0);
	if (!ret)
		*val = readl(srab_base + B53_SRAB_RD_L) & 0xffff;
	srab_release_grant();
	return ret;
}

static int srab_write16(u8 page, u8 reg, u16 val)
{
	int ret = srab_request_grant();

	if (ret)
		return ret;
	writel(val, srab_base + B53_SRAB_WD_L);
	ret = srab_op(page, reg, B53_SRAB_CMDSTAT_WRITE);
	srab_release_grant();
	return ret;
}

static int __init fa_switch_brcmhdr_persist_init(void)
{
	u16 val;
	int ret;

	srab_base = ioremap(SRAB_PHYS_BASE, SRAB_MAP_SIZE);
	if (!srab_base)
		return -ENOMEM;

	ret = srab_read16(PAGE_MMR, REG_BRCM_HDR, &val);
	if (ret) {
		pr_err("fa_switch_brcmhdr_persist: initial read failed (%d)\n", ret);
		iounmap(srab_base);
		srab_base = NULL;
		return ret;
	}
	brcmhdr_before = val;
	pr_info("fa_switch_brcmhdr_persist: BRCM_HDR before = 0x%04x\n", val);

	ret = srab_write16(PAGE_MMR, REG_BRCM_HDR, val | BRCM_HDR_ENABLE_BIT);
	if (ret) {
		pr_err("fa_switch_brcmhdr_persist: write failed (%d)\n", ret);
		iounmap(srab_base);
		srab_base = NULL;
		return ret;
	}

	pr_info("fa_switch_brcmhdr_persist: BRCM_HDR set to 0x%04x - staying "
		"active until this module is rmmod'd\n", val | BRCM_HDR_ENABLE_BIT);

	return 0;
}

static void __exit fa_switch_brcmhdr_persist_exit(void)
{
	if (srab_base) {
		srab_write16(PAGE_MMR, REG_BRCM_HDR, brcmhdr_before);
		pr_info("fa_switch_brcmhdr_persist: reverted BRCM_HDR to 0x%04x\n",
			brcmhdr_before);
		iounmap(srab_base);
		srab_base = NULL;
	}
	pr_info("fa_switch_brcmhdr_persist: unloaded\n");
}

module_init(fa_switch_brcmhdr_persist_init);
module_exit(fa_switch_brcmhdr_persist_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hwoffload-research");
MODULE_DESCRIPTION("REG_BRCM_HDR set-and-hold (revert on rmmod) - the missing half of robo_fa_enable()");
