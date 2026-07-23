// SPDX-License-Identifier: GPL-2.0
/*
 * fa_switch_oobpause_test.c - Test the switch-side REG_FC_OOBPAUSE bit
 * from Broadcom's robo_fa_enable() (bcmrobo.c), via the SRAB bus, on the
 * BCM53012 switch integrated into this R8000's SoC.
 *
 * =====================================================================
 * SCOPE
 * =====================================================================
 *
 * robo_fa_enable() touches two registers: REG_BRCM_HDR (tag-mode on the
 * IMP/CPU port) and REG_FC_OOBPAUSE (flow-control signaling mode). This
 * module ONLY tests REG_FC_OOBPAUSE. REG_BRCM_HDR is deliberately NOT
 * touched: research this session confirmed mainline's b53_common.c
 * (b53_brcm_hdr_setup(), called from b53_enable_cpu_port() at every
 * boot) already sets the equivalent tag mode today - this SSH session's
 * own traffic already rides that format, so writing it again adds
 * nothing and only adds risk for no informational gain.
 *
 * REG_FC_OOBPAUSE (bcmrobo.c PAGE_FC=0x0a, offset 0xe0) selects between
 * synthesized in-band 802.3x PAUSE frames (default/clear) and a
 * dedicated out-of-band sideband signal (set) for switch<->SoC flow
 * control. Research this session assessed this as comparatively low
 * risk even if wrong - graceful flow-control degradation under
 * congestion, not a frame-parsing break, and the bit is close to inert
 * for a light, non-saturating traffic pattern like this bench session.
 *
 * =====================================================================
 * WHY THIS IS A DIFFERENT RISK CLASS FROM THE FA/CTF WORK
 * =====================================================================
 *
 * fa_probe.c/fa_bringup.c/fa_macc_test.c all touched a register block
 * (0x18027c00) that NOTHING else in the running kernel uses - no
 * concurrency question was even possible. The SRAB bus (0x18007000) is
 * DIFFERENT: it is actively used right now by the already-loaded,
 * already-load-bearing b53_srab kernel driver, which manages this exact
 * switch. This module maps the same physical SRAB register block
 * independently and issues its own grant-request/transact/release
 * sequence, replicating mainline b53_srab.c's exact protocol
 * (B53_SRAB_CTRLS request/grant handshake, B53_SRAB_CMDSTAT page/reg
 * encoding + busy-bit poll, RD_L/WD_L data registers) rather than
 * inventing a new one.
 *
 * The RCAREQ/RCAGNT handshake is itself a HARDWARE arbitration
 * mechanism (not a software convention) specifically designed to let
 * multiple independent requesters share this bus correctly - that is
 * its entire purpose, and it is the same protocol the live b53_srab
 * driver itself relies on for its own internal correctness. Following
 * it faithfully here does not bypass the live driver's safety, it
 * participates in the same arbitration the live driver already trusts.
 *
 * Confirmed live via devicetree before writing this module:
 * /sys/firmware/devicetree/base/ethernet-switch@18007000, compatible
 * "brcm,bcm53012-srab brcm,bcm5301x-srab", reg 0x18007000 size 0x1000,
 * status okay - this is the real, active SRAB node on this exact board.
 *
 * Reads current value, flips only bit 8, reads back to confirm, then
 * reverts and confirms the revert - same discipline as fa_bringup.c.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/delay.h>

#define SRAB_PHYS_BASE	0x18007000UL
#define SRAB_MAP_SIZE	0x1000UL

/* Offsets from mainline drivers/net/dsa/b53/b53_srab.c */
#define B53_SRAB_CMDSTAT	0x2c
#define B53_SRAB_WD_H		0x30
#define B53_SRAB_WD_L		0x34
#define B53_SRAB_RD_H		0x38
#define B53_SRAB_RD_L		0x3c
#define B53_SRAB_CTRLS		0x40

#define B53_SRAB_CMDSTAT_WRITE		(1U << 1)
#define B53_SRAB_CMDSTAT_GORDYN		(1U << 0)
#define B53_SRAB_CMDSTAT_PAGE_SHIFT	24
#define B53_SRAB_CMDSTAT_REG_SHIFT	16

#define B53_SRAB_CTRLS_RCAREQ		(1U << 3)
#define B53_SRAB_CTRLS_RCAGNT		(1U << 4)

/* bcmrobo.c PAGE_FC / REG_FC_OOBPAUSE */
#define PAGE_FC			0x0aU
#define REG_FC_OOBPAUSE		0xe0U
#define FC_OOBPAUSE_BIT		(1U << 8)

#define SRAB_GRANT_POLL_STEPS	20
#define SRAB_OP_POLL_STEPS	5

static void __iomem *srab_base;

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
	pr_err("fa_switch_oobpause_test: grant request timed out\n");
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
	pr_err("fa_switch_oobpause_test: op page=0x%02x reg=0x%02x timed out\n",
		page, reg);
	return -EIO;
}

static int srab_read16(u8 page, u8 reg, u16 *val)
{
	int ret;

	ret = srab_request_grant();
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
	int ret;

	ret = srab_request_grant();
	if (ret)
		return ret;

	writel(val, srab_base + B53_SRAB_WD_L);
	ret = srab_op(page, reg, B53_SRAB_CMDSTAT_WRITE);

	srab_release_grant();
	return ret;
}

static u16 oobpause_before;
static bool have_before;

static int __init fa_switch_oobpause_test_init(void)
{
	u16 val, readback;
	int ret;

	pr_info("fa_switch_oobpause_test: mapping SRAB block at phys=0x%08lx size=0x%lx\n",
		SRAB_PHYS_BASE, SRAB_MAP_SIZE);

	srab_base = ioremap(SRAB_PHYS_BASE, SRAB_MAP_SIZE);
	if (!srab_base) {
		pr_err("fa_switch_oobpause_test: ioremap failed\n");
		return -ENOMEM;
	}

	ret = srab_read16(PAGE_FC, REG_FC_OOBPAUSE, &val);
	if (ret) {
		pr_err("fa_switch_oobpause_test: initial read failed (%d), aborting - "
			"not touching anything further\n", ret);
		goto out_unmap;
	}
	oobpause_before = val;
	have_before = true;
	pr_info("fa_switch_oobpause_test: OOBPAUSE before = 0x%04x (bit8=%d)\n",
		val, !!(val & FC_OOBPAUSE_BIT));

	ret = srab_write16(PAGE_FC, REG_FC_OOBPAUSE, val | FC_OOBPAUSE_BIT);
	if (ret) {
		pr_err("fa_switch_oobpause_test: write failed (%d)\n", ret);
		goto out_unmap;
	}

	ret = srab_read16(PAGE_FC, REG_FC_OOBPAUSE, &readback);
	if (ret) {
		pr_err("fa_switch_oobpause_test: read-back after write failed (%d)\n", ret);
		goto out_unmap;
	}
	pr_info("fa_switch_oobpause_test: OOBPAUSE after write = 0x%04x (bit8=%d) %s\n",
		readback, !!(readback & FC_OOBPAUSE_BIT),
		(readback & FC_OOBPAUSE_BIT) ? "SET-CONFIRMED" : "DID-NOT-STICK");

	pr_info("fa_switch_oobpause_test: reverting to original value 0x%04x\n",
		oobpause_before);
	ret = srab_write16(PAGE_FC, REG_FC_OOBPAUSE, oobpause_before);
	if (ret) {
		pr_err("fa_switch_oobpause_test: revert write failed (%d) - "
			"register may be left in the modified state\n", ret);
		goto out_unmap;
	}

	ret = srab_read16(PAGE_FC, REG_FC_OOBPAUSE, &readback);
	if (!ret)
		pr_info("fa_switch_oobpause_test: OOBPAUSE after revert = 0x%04x %s\n",
			readback,
			(readback == oobpause_before) ? "REVERT-CONFIRMED" : "REVERT-MISMATCH");

out_unmap:
	iounmap(srab_base);
	srab_base = NULL;
	pr_info("fa_switch_oobpause_test: unmapped, test complete\n");

	return 0;
}

static void __exit fa_switch_oobpause_test_exit(void)
{
	pr_info("fa_switch_oobpause_test: module unloaded\n");
}

module_init(fa_switch_oobpause_test_init);
module_exit(fa_switch_oobpause_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hwoffload-research");
MODULE_DESCRIPTION("Switch-side SRAB REG_FC_OOBPAUSE test (BRCM_HDR deliberately not touched)");
