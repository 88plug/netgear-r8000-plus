// SPDX-License-Identifier: GPL-2.0
/*
 * fa_switch_oobpause_persist.c - Same SRAB REG_FC_OOBPAUSE write already
 * proven safe in fa_switch_oobpause_test.c, but the revert happens on
 * module EXIT (rmmod) instead of automatically within init() - so this
 * can be left persistently set for the duration of the live=1 real-
 * forwarding test, then cleanly reverted with a single `rmmod` when
 * the test is done. Same protocol, same registers, same safety
 * reasoning already documented in fa_switch_oobpause_test.c and
 * BRINGUP_RESULT.md - this file only changes *when* the revert happens.
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

#define PAGE_FC			0x0aU
#define REG_FC_OOBPAUSE		0xe0U
#define FC_OOBPAUSE_BIT		(1U << 8)

#define SRAB_GRANT_POLL_STEPS	20
#define SRAB_OP_POLL_STEPS	5

static void __iomem *srab_base;
static u16 oobpause_before;

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

static int __init fa_switch_oobpause_persist_init(void)
{
	u16 val;
	int ret;

	srab_base = ioremap(SRAB_PHYS_BASE, SRAB_MAP_SIZE);
	if (!srab_base)
		return -ENOMEM;

	ret = srab_read16(PAGE_FC, REG_FC_OOBPAUSE, &val);
	if (ret) {
		pr_err("fa_switch_oobpause_persist: initial read failed (%d)\n", ret);
		iounmap(srab_base);
		srab_base = NULL;
		return ret;
	}
	oobpause_before = val;
	pr_info("fa_switch_oobpause_persist: OOBPAUSE before = 0x%04x\n", val);

	ret = srab_write16(PAGE_FC, REG_FC_OOBPAUSE, val | FC_OOBPAUSE_BIT);
	if (ret) {
		pr_err("fa_switch_oobpause_persist: write failed (%d)\n", ret);
		iounmap(srab_base);
		srab_base = NULL;
		return ret;
	}

	pr_info("fa_switch_oobpause_persist: OOBPAUSE set to 0x%04x - staying active "
		"until this module is rmmod'd (unlike fa_switch_oobpause_test.c, "
		"which self-reverts within its own init)\n", val | FC_OOBPAUSE_BIT);

	return 0;
}

static void __exit fa_switch_oobpause_persist_exit(void)
{
	if (srab_base) {
		srab_write16(PAGE_FC, REG_FC_OOBPAUSE, oobpause_before);
		pr_info("fa_switch_oobpause_persist: reverted OOBPAUSE to 0x%04x\n",
			oobpause_before);
		iounmap(srab_base);
		srab_base = NULL;
	}
	pr_info("fa_switch_oobpause_persist: unloaded\n");
}

module_init(fa_switch_oobpause_persist_init);
module_exit(fa_switch_oobpause_persist_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hwoffload-research");
MODULE_DESCRIPTION("REG_FC_OOBPAUSE set-and-hold (revert on rmmod), for the live=1 test window");
