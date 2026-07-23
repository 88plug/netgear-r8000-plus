// SPDX-License-Identifier: GPL-2.0
/*
 * fa_macc_test.c - Indirect memory-access WRITE + READ-BACK test for the
 * Broadcom BCM4709 FA/CTF hardware block's NEXT-HOP table, following
 * Broadcom's own SDK6 etc_fa.c fa_write_nhop_entry()/fa_read_nhop_entry()
 * pattern exactly (graveyard-vendor/extracted-source/etc_fa.c lines 494-521,
 * 523-549), including the WAR777 HWQ_THRESHLD workaround.
 *
 * =====================================================================
 * SCOPE
 * =====================================================================
 *
 * This writes a known, arbitrary test pattern (0x11111111 / 0x22222222 /
 * 0x33333333) to Next-Hop table index 0 via the indirect mem_acc_ctl /
 * m_accdata[] path, then reads it back via the same path, and compares.
 *
 * Table index 0 is INERT: nothing currently references next-hop index 0
 * from any NAPT flow-table row (the flow table was just table-init'd by
 * fa_bringup.ko, all rows cleared), so no live packet lookup path reads
 * this slot. This tests the WRITE MECHANISM itself - the indirect
 * table-select + data-register + busy-poll + WAR777 protocol - without
 * creating any flow entry that would actually intercept real traffic.
 * That is a separate, further step (constructing a real, semantically
 * valid NAPT flow row) not attempted here.
 *
 * Still GMAC-side only. Still no switch/SRAB access.
 *
 * =====================================================================
 * RISK ASSESSMENT
 * =====================================================================
 *
 * fa_bringup.ko already proved a write to this register block's CONTROL
 * register is safe (clean response, zero adverse system effect, twice
 * verified). This test writes to a DIFFERENT register in the same block
 * (mem_acc_ctl, m_accdata[]) using the exact indirect-access protocol
 * Broadcom's own driver uses for real table programming, including the
 * WAR777 workaround (force HWQ_THRESHLD to 0 during the access, restore
 * after, 1ms delay each side - applied unconditionally here since it's
 * cheap and the erratum's applicability to this exact chip rev is not
 * fully proven either way, only "does not literally match" per
 * GO_NOGO_BRINGUP.md).
 *
 * Bounded poll on dbg_status.MEM_ACC_BUSY (matches Broadcom's own
 * SPINWAIT(...,10000) - here as an explicit, logged step-count loop, same
 * approach as fa_bringup.c's INIT_DONE poll).
 *
 * Mitigations in place: panic_on_oops=0 (already set this session,
 * reconfirmed before load), recovery net already reconfirmed staged this
 * session, read-back verification (not just "the write didn't crash
 * anything" - actually checking the data matches), single table slot
 * touched, no flow-table (NAPT) row created.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/delay.h>

#define FA_PHYS_BASE	0x18027c00UL
#define FA_MAP_SIZE	0x100UL

/* Struct offsets, fa_core.h */
#define FA_REG_CONTROL		0x00
#define FA_REG_MEM_ACC_CTL	0x04
#define FA_REG_DBG_STATUS	0x70
#define FA_REG_M_ACCDATA	0xa0

/* Control register bits needed for the WAR777 workaround */
#define CTF_CTL_HWQ_THRESHLD_MASK	(0x1FFU << 4)
#define CTF_CTL_HWQ_DEF_THRESHLD_VAL	(0x140U << 4)	/* fa_core.h CTF_CTL_HWQ_DEF_THRESHLD << 4 */

/* mem_acc_ctl bits, fa_core.h lines 97-110 */
#define CTF_MEMACC_TAB_INDEX_MASK	(0x3FFU << 0)
#define CTF_MEMACC_NEXT_HOP_TAB		(2U << 10)
#define CTF_MEMACC_RD_WR_N		(1U << 12)

#define CTF_MEMACC_WR_TABLE(t, i)	(((t) << 10) | ((i) & CTF_MEMACC_TAB_INDEX_MASK))
#define CTF_MEMACC_RD_TABLE(t, i)	(CTF_MEMACC_RD_WR_N | ((t) << 10) | ((i) & CTF_MEMACC_TAB_INDEX_MASK))

#define CTF_DBG_MEM_ACC_BUSY	(1U << 0)

#define NH_TEST_INDEX	0
#define NH_ROW_WORDS	3

#define MEM_ACC_POLL_STEPS	20	/* ~20ms bound, driver's own SPINWAIT is 10000 (unit unverified) */

static void __iomem *fa_base;

static void war777_on(void)
{
	u32 val = readl(fa_base + FA_REG_CONTROL);

	val &= ~CTF_CTL_HWQ_THRESHLD_MASK;
	writel(val, fa_base + FA_REG_CONTROL);
	mdelay(1);
}

static void war777_off(void)
{
	u32 val = readl(fa_base + FA_REG_CONTROL);

	val &= ~CTF_CTL_HWQ_THRESHLD_MASK;
	val |= CTF_CTL_HWQ_DEF_THRESHLD_VAL;
	writel(val, fa_base + FA_REG_CONTROL);
	mdelay(1);
}

static bool wait_macc_idle(void)
{
	int i;

	for (i = 0; i < MEM_ACC_POLL_STEPS; i++) {
		if (!(readl(fa_base + FA_REG_DBG_STATUS) & CTF_DBG_MEM_ACC_BUSY))
			return true;
		mdelay(1);
	}
	return false;
}

static int __init fa_macc_test_init(void)
{
	u32 wr_pattern[NH_ROW_WORDS] = { 0x11111111U, 0x22222222U, 0x33333333U };
	u32 rd_back[NH_ROW_WORDS] = { 0, 0, 0 };
	int i;
	bool idle, all_match = true;

	pr_info("fa_macc_test: mapping FA register block at phys=0x%08lx size=0x%lx\n",
		FA_PHYS_BASE, FA_MAP_SIZE);

	fa_base = ioremap(FA_PHYS_BASE, FA_MAP_SIZE);
	if (!fa_base) {
		pr_err("fa_macc_test: ioremap failed\n");
		return -ENOMEM;
	}

	pr_info("fa_macc_test: WRITE phase - NH table index %d, pattern 0x%08x 0x%08x 0x%08x\n",
		NH_TEST_INDEX, wr_pattern[0], wr_pattern[1], wr_pattern[2]);

	war777_on();
	writel(CTF_MEMACC_WR_TABLE(CTF_MEMACC_NEXT_HOP_TAB >> 10, NH_TEST_INDEX),
	       fa_base + FA_REG_MEM_ACC_CTL);
	for (i = NH_ROW_WORDS; i > 0; i--)
		writel(wr_pattern[i - 1], fa_base + FA_REG_M_ACCDATA + (i - 1) * 4);
	idle = wait_macc_idle();
	war777_off();

	if (!idle) {
		pr_err("fa_macc_test: WRITE mem-access still busy after poll timeout - "
			"aborting before read-back, NOT treating this as a pass\n");
		goto out_unmap;
	}
	pr_info("fa_macc_test: write mem-access completed (busy cleared)\n");

	pr_info("fa_macc_test: READ-BACK phase - NH table index %d\n", NH_TEST_INDEX);

	war777_on();
	writel(CTF_MEMACC_RD_TABLE(CTF_MEMACC_NEXT_HOP_TAB >> 10, NH_TEST_INDEX),
	       fa_base + FA_REG_MEM_ACC_CTL);
	idle = wait_macc_idle();
	for (i = 0; i < NH_ROW_WORDS; i++)
		rd_back[i] = readl(fa_base + FA_REG_M_ACCDATA + i * 4);
	/* Broadcom's own CTF_FA_MACC_RD clears mem_acc_ctl to 0 after reading */
	writel(0, fa_base + FA_REG_MEM_ACC_CTL);
	war777_off();

	if (!idle)
		pr_err("fa_macc_test: READ mem-access still busy after poll timeout - "
			"read-back data below may be unreliable\n");

	for (i = 0; i < NH_ROW_WORDS; i++) {
		bool match = (rd_back[i] == wr_pattern[i]);

		pr_info("fa_macc_test: word[%d] wrote=0x%08x read=0x%08x %s\n",
			i, wr_pattern[i], rd_back[i], match ? "MATCH" : "MISMATCH");
		all_match = all_match && match;
	}

	if (all_match)
		pr_info("fa_macc_test: RESULT = PASS - all 3 words round-tripped correctly "
			"through the indirect mem_acc_ctl/m_accdata write+read path\n");
	else
		pr_info("fa_macc_test: RESULT = FAIL - data did not round-trip correctly, "
			"indirect access mechanism is not behaving as documented\n");

out_unmap:
	iounmap(fa_base);
	fa_base = NULL;
	pr_info("fa_macc_test: unmapped, test complete\n");

	return 0;
}

static void __exit fa_macc_test_exit(void)
{
	pr_info("fa_macc_test: module unloaded\n");
}

module_init(fa_macc_test_init);
module_exit(fa_macc_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hwoffload-research");
MODULE_DESCRIPTION("FA indirect mem-access write/read-back test (Next-Hop table, inert slot)");
