// SPDX-License-Identifier: GPL-2.0
/*
 * fa_bringup.c - First WRITE to the Broadcom BCM4709 on-chip Flow-
 * Accelerator (FA / "CTF") register block, scoped to exactly the
 * fa_setmode()-equivalent table-init sequence from Broadcom's own SDK6
 * etc_fa.c (graveyard-vendor/extracted-source/etc_fa.c), gated by
 * hwoffload-research/fa-probe/GO_NOGO_BRINGUP.md.
 *
 * =====================================================================
 * SCOPE - read before loading this module
 * =====================================================================
 *
 * This module performs ONLY the GMAC-side FA control-register bring-up:
 * write the 5 table-init bits + CRC_OWRT + DSBL_MAC_DA_CHECK to the
 * control register (matching fa_setmode(), mode == CTF_FA_NORMAL branch,
 * i.e. neither CTF_CTL_SW_ACC_MODE nor CTF_CTL_BYPASS_CTF is set), then
 * polls the status register for CTF_INTSTAT_INIT_DONE with a bounded
 * timeout, exactly as fa_setmode() does with its own SPINWAIT.
 *
 * It deliberately does NOT:
 *   - touch mem_acc_ctl / m_accdata[] (NAPT/next-hop table row writes) -
 *     that requires the WAR777 HWQ_THRESHLD workaround and table-select
 *     logic (etc_fa.c fa_write_nhop_entry() etc.) and is a separate,
 *     further escalation not covered by this pass.
 *   - call anything switch-side (robo_fa_enable() over SRAB) - reading
 *     etc_fa.c's fa_down() showed a complete fa_up() also toggles the
 *     switch's own FA enable over the SRAB bus, which is a materially
 *     different, additional risk surface (touches ALL LAN ports via the
 *     DSA switch, not just this isolated GMAC-3 sub-block) discovered
 *     only while writing this module, and out of scope for what was
 *     actually reviewed and approved before this file was written.
 *
 * Physical address / register offsets: identical to fa_probe.c (already
 * loaded and read-verified twice this session, byte-identical across a
 * full reboot - see GO_NOGO_BRINGUP.md's stability re-check). Control-
 * bit definitions and INIT_DONE mask are copied verbatim from Broadcom's
 * own fa_core.h (graveyard-vendor/extracted-source/fa_core.h lines 82-179).
 *
 * =====================================================================
 * RISK ASSESSMENT - why this differs from the read-only probe
 * =====================================================================
 *
 * fa_probe.c's risk assessment (external abort vs. clean readback from an
 * unclocked-but-decoded wrapper) covered READS only. A WRITE to a real,
 * backplane-decoded, but internally unclocked/held-in-reset block is not
 * guaranteed to behave the same way a read does - some hardware wrappers
 * silently swallow writes to an unclocked core (no observable effect,
 * write is simply lost), others may latch garbage into flops that are
 * mid-reset, and in the worst case a write can trigger internal logic
 * (the very INIT strobe bits being set are, by definition, meant to kick
 * off internal state-machine activity) that the read-only probe never
 * exercised at all. This is a genuinely different, unproven risk class,
 * not a bigger version of the same one - which is exactly why
 * GO_NOGO_BRINGUP.md treats it as needing its own separate operator
 * go-ahead rather than following automatically from the read probe's.
 *
 * Mitigations actually in place for this specific write:
 *   - panic_on_oops=0 set on the live router immediately before loading
 *     this module (confirmed, not assumed - checked live 2026-07-23).
 *   - Recovery net re-confirmed staged immediately before this build:
 *     nmrpflash installed and sees the dongle interface, every OpenWrt
 *     version through v8 plus the stock Netgear .chk on hand.
 *   - The control register's PRE-write value is read back and logged
 *     before the write, and the module attempts to restore it (best
 *     effort - INIT bits are typically self-clearing pulse strobes in
 *     this class of hardware, so a literal write-back of the pre-write
 *     value is a reasonable but NOT guaranteed-correct revert) on
 *     module unload, so a human reviewing dmesg can compare before/after
 *     even if the auto-revert isn't perfect.
 *   - The status-register poll uses a bounded loop (not an unbounded
 *     spin) - if INIT_DONE never asserts, this module gives up cleanly
 *     and logs a clear "did not complete" result rather than hanging.
 *   - No NAPT table access, no switch/SRAB access - if this write goes
 *     wrong, the blast radius is limited to whatever this one register
 *     write can affect on its own, not a whole additional subsystem.
 *
 * Net call: this is the actual go/no-go escalation GO_NOGO_BRINGUP.md
 * flagged as needing its own explicit ask - operator go-ahead obtained
 * 2026-07-23 for exactly this scope (control-register bring-up only, not
 * NAPT writes, not switch/SRAB).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/delay.h>

/* Identical to fa_probe.c - GMAC-3 corereg base + Broadcom's FA_BASE_OFFSET */
#define FA_PHYS_BASE	0x18027c00UL
#define FA_MAP_SIZE	0x100UL

/* Offsets within faregs_t (fa_core.h) */
#define FA_REG_CONTROL	0x00
#define FA_REG_STATUS	0x30

/* Control-register bits (fa_core.h lines 82-94), mode == CTF_FA_NORMAL
 * branch of fa_setmode(): neither SW_ACC_MODE nor BYPASS_CTF is set.
 */
#define CTF_CTL_CRC_OWRT		(1U << 3)
#define CTF_CTL_NAPT_FLOW_INIT		(1U << 13)
#define CTF_CTL_NEXT_HOP_INIT		(1U << 14)
#define CTF_CTL_HWQ_INIT		(1U << 15)
#define CTF_CTL_LAB_INIT		(1U << 16)
#define CTF_CTL_HB_INIT		(1U << 17)
#define CTF_CTL_DSBL_MAC_DA_CHECK	(1U << 18)

#define FA_BRINGUP_BITS \
	(CTF_CTL_DSBL_MAC_DA_CHECK | CTF_CTL_NAPT_FLOW_INIT | \
	 CTF_CTL_NEXT_HOP_INIT | CTF_CTL_HWQ_INIT | \
	 CTF_CTL_LAB_INIT | CTF_CTL_HB_INIT | CTF_CTL_CRC_OWRT)

/* Interrupt status bits (fa_core.h lines 165-179) */
#define CTF_INTSTAT_HB_INIT_DONE		(1U << 9)
#define CTF_INTSTAT_LAB_INIT_DONE		(1U << 8)
#define CTF_INTSTAT_HWQ_INIT_DONE		(1U << 7)
#define CTF_INTSTAT_NXT_HOP_INIT_DONE		(1U << 6)
#define CTF_INTSTAT_NAPT_FLOW_INIT_DONE		(1U << 5)
#define CTF_INTSTAT_INIT_DONE \
	(CTF_INTSTAT_HB_INIT_DONE | CTF_INTSTAT_LAB_INIT_DONE | \
	 CTF_INTSTAT_HWQ_INIT_DONE | CTF_INTSTAT_NXT_HOP_INIT_DONE | \
	 CTF_INTSTAT_NAPT_FLOW_INIT_DONE)

/* fa_setmode()'s own SPINWAIT is 50000 (its SDK's spin unit); bound this
 * poll generously at 100ms in 1ms steps rather than trust an exact unit
 * conversion we can't verify against SDK6's SPINWAIT macro definition.
 */
#define FA_INIT_POLL_STEPS	100

static void __iomem *fa_base;
static u32 fa_control_before;

static int __init fa_bringup_init(void)
{
	u32 control_after, status;
	int i;
	bool init_done = false;

	pr_info("fa_bringup: mapping FA register block at phys=0x%08lx size=0x%lx\n",
		FA_PHYS_BASE, FA_MAP_SIZE);

	fa_base = ioremap(FA_PHYS_BASE, FA_MAP_SIZE);
	if (!fa_base) {
		pr_err("fa_bringup: ioremap(0x%08lx, 0x%lx) failed\n",
			FA_PHYS_BASE, FA_MAP_SIZE);
		return -ENOMEM;
	}

	fa_control_before = readl(fa_base + FA_REG_CONTROL);
	pr_info("fa_bringup: control BEFORE write = 0x%08x\n", fa_control_before);

	control_after = fa_control_before | FA_BRINGUP_BITS;
	pr_info("fa_bringup: writing control = 0x%08x (bringup bits 0x%08x)\n",
		control_after, (u32)FA_BRINGUP_BITS);

	/*
	 * THE WRITE. Everything above and below is preparation/observation;
	 * this is the one line that changes state on real hardware.
	 */
	writel(control_after, fa_base + FA_REG_CONTROL);

	/* Bounded poll for CTF_INTSTAT_INIT_DONE, matching fa_setmode()'s
	 * own SPINWAIT on regs->status, but with an explicit, logged upper
	 * bound instead of trusting an unverified SDK spin-unit conversion.
	 */
	for (i = 0; i < FA_INIT_POLL_STEPS; i++) {
		status = readl(fa_base + FA_REG_STATUS);
		if ((status & CTF_INTSTAT_INIT_DONE) == CTF_INTSTAT_INIT_DONE) {
			init_done = true;
			break;
		}
		mdelay(1);
	}

	status = readl(fa_base + FA_REG_STATUS);
	pr_info("fa_bringup: status AFTER write/poll = 0x%08x (waited ~%dms)\n",
		status, i);

	if (init_done)
		pr_info("fa_bringup: CTF_INTSTAT_INIT_DONE asserted (all 5 init bits set) "
			"- table-init bring-up completed as fa_setmode() would report success\n");
	else
		pr_info("fa_bringup: CTF_INTSTAT_INIT_DONE did NOT assert within ~%dms - "
			"bring-up did not complete; treat as a clean non-result, not a hang "
			"(this module never blocks unboundedly)\n", FA_INIT_POLL_STEPS);

	pr_info("fa_bringup: NOT proceeding to NAPT/next-hop table writes or switch-side "
		"robo_fa_enable() - both out of scope for this pass, see file header\n");

	return 0;
}

static void __exit fa_bringup_exit(void)
{
	if (fa_base) {
		u32 control_now = readl(fa_base + FA_REG_CONTROL);

		pr_info("fa_bringup: control before revert-attempt = 0x%08x, "
			"restoring pre-write value 0x%08x (best effort - INIT bits "
			"are typically self-clearing pulse strobes, this write-back "
			"is not guaranteed to be a true undo)\n",
			control_now, fa_control_before);
		writel(fa_control_before, fa_base + FA_REG_CONTROL);

		iounmap(fa_base);
		fa_base = NULL;
	}

	pr_info("fa_bringup: module unloaded\n");
}

module_init(fa_bringup_init);
module_exit(fa_bringup_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hwoffload-research");
MODULE_DESCRIPTION("FA control-register bring-up (fa_setmode()-equivalent), GMAC-side only");
