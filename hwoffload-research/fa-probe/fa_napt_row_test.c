// SPDX-License-Identifier: GPL-2.0
/*
 * fa_napt_row_test.c - Phase C proof: construct and write a COMPLETE,
 * realistic NAPT flow-table row (not just an inert Next-Hop test slot
 * like fa_macc_test.c) using the exact bit-packing from Broadcom's own
 * fa_napt_prep_ipv4_word()/CTF_FA_SET_NH_ENTRY (etc_fa.c), then read it
 * back, verify, then mark it invalid (delete) and verify that too.
 *
 * =====================================================================
 * SCOPE
 * =====================================================================
 *
 * This does NOT hook into real traffic. It builds one synthetic-but-
 * realistic test flow entirely in this module (not derived from any
 * real connection) - simulating a LAN client (192.168.1.50:34567)
 * talking to a public IP (93.184.216.34:80, example.com's real address,
 * used only as a realistic-looking constant, no packet is ever sent to
 * it) via a synthetic next-hop MAC (de:ad:be:ef:00:01). Table index 0 is
 * used for both the next-hop and NAPT-flow tables, same inert-slot
 * reasoning as fa_macc_test.c: nothing in the currently-running kernel
 * (fa_accel.c always returns -EOPNOTSUPP) or switch forwarding path
 * references this index, so no live packet lookup ever consults it.
 *
 * This proves the COMPLETE data-plane lifecycle Phase C needs: next-hop
 * entry construction+write, full 8-word NAPT row construction+write
 * (not a partial/synthetic pattern - the real bit-packing macros,
 * verbatim), read-back verification, and delete (clear the valid bit,
 * per _fa_napt_del()'s actual mechanism) + verify the delete stuck.
 *
 * Deliberately still NOT done here: hash-based table index allocation
 * (fa_napt_add()'s FA_NAPT_HASH/linked-list bucket management - that's
 * Broadcom's in-memory software bookkeeping, not a hardware requirement,
 * and irrelevant to proving the hardware write mechanism itself) and
 * wiring this into fa_accel.c's real FLOW_CLS_REPLACE handler (that
 * remains a separate step, gated on this test passing first).
 *
 * Same safety posture as every prior fa-probe module: WAR777-wrapped
 * indirect access, bounded polls, read-back verification (not assumed),
 * and an explicit revert (delete) at the end - nothing left active.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/if_ether.h>

#define FA_PHYS_BASE	0x18027c00UL
#define FA_MAP_SIZE	0x100UL

#define FA_REG_CONTROL		0x00
#define FA_REG_MEM_ACC_CTL	0x04
#define FA_REG_DBG_STATUS	0x70
#define FA_REG_M_ACCDATA	0xa0

#define CTF_CTL_HWQ_THRESHLD_MASK	(0x1FFU << 4)
#define CTF_CTL_HWQ_DEF_THRESHLD_VAL	(0x140U << 4)

#define CTF_MEMACC_TAB_INDEX_MASK	(0x3FFU << 0)
#define CTF_MEMACC_RD_WR_N		(1U << 12)
#define CTF_MEMACC_WR_TABLE(t, i)	(((t) << 10) | ((i) & CTF_MEMACC_TAB_INDEX_MASK))
#define CTF_MEMACC_RD_TABLE(t, i)	(CTF_MEMACC_RD_WR_N | ((t) << 10) | ((i) & CTF_MEMACC_TAB_INDEX_MASK))

#define CTF_MEMACC_TBL_NF	0U	/* NAPT flow table */
#define CTF_MEMACC_TBL_NH	2U	/* Next-hop table */

#define CTF_DBG_MEM_ACC_BUSY	(1U << 0)

/* fa_core.h constants used in the row-packing macros below */
#define CTF_NH_OP_NOTAG		2U
#define CTF_NP_INTERNAL		0U
#define CTF_NAPT_OVRW_IP	1U

#define NH_TEST_INDEX	0U
#define NF_TEST_INDEX	0U
#define NH_ROW_WORDS	3
#define NF_ROW_WORDS	8

#define MEM_ACC_POLL_STEPS	20

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

static bool fa_macc_write(u32 table, u32 idx, const u32 *words, int n)
{
	int i;
	bool idle;

	war777_on();
	writel(CTF_MEMACC_WR_TABLE(table, idx), fa_base + FA_REG_MEM_ACC_CTL);
	for (i = n; i > 0; i--)
		writel(words[i - 1], fa_base + FA_REG_M_ACCDATA + (i - 1) * 4);
	idle = wait_macc_idle();
	war777_off();

	return idle;
}

static bool fa_macc_read(u32 table, u32 idx, u32 *words, int n)
{
	int i;
	bool idle;

	war777_on();
	writel(CTF_MEMACC_RD_TABLE(table, idx), fa_base + FA_REG_MEM_ACC_CTL);
	idle = wait_macc_idle();
	for (i = 0; i < n; i++)
		words[i] = readl(fa_base + FA_REG_M_ACCDATA + i * 4);
	writel(0, fa_base + FA_REG_MEM_ACC_CTL);
	war777_off();

	return idle;
}

/*
 * CTF_FA_SET_NH_ENTRY(d, s, ft, o, vt), etc_fa.c, verbatim - s[0..5] are
 * MAC address bytes in transmission order (dhost.octet convention).
 */
static void fa_set_nh_entry(u32 *d, const u8 *s, u32 ft, u32 o, u32 vt)
{
	d[0] = ((ft & 0x1) | ((o & 0x3) << 1) | ((vt & 0xFFFF) << 3) |
		((u32)s[5] << 19) | ((u32)(s[4] & 0x1F) << 27));
	d[1] = (((u32)(s[4] & 0xE0) >> 5) | ((u32)s[3] << 3) |
		((u32)s[2] << 11) | ((u32)s[1] << 19) |
		((u32)(s[0] & 0x1F) << 27));
	d[2] = ((u32)(s[0] & 0xE0) >> 5);
}

/*
 * fa_napt_prep_ipv4_word(), etc_fa.c line 825, verbatim - simplified
 * parameter list for a synthetic test (no ctf_ipc_t/fa_napt_t structs).
 * action/tgt_dma/pool_idx/nh_idx match napt->nfi.*; nat_ip/nat_port
 * match ipc->nat.*; tuple_* match ipc->tuple.*; is_snat matches
 * (ipc->action & CTF_ACTION_SNAT).
 */
static void fa_prep_napt_ipv4_row(u32 *tbl, u32 action, u32 tgt_dma, u32 pool_idx,
				   u32 nh_idx, __be32 nat_ip, __be16 nat_port,
				   __be32 tuple_dip, __be32 tuple_sip,
				   __be16 tuple_dp, __be16 tuple_sp, u8 proto,
				   bool is_snat)
{
	memset(tbl, 0, sizeof(u32) * NF_ROW_WORDS);

	tbl[1] = (action << 3) | (tgt_dma << 5) | (pool_idx << 6) |
		 (nh_idx << 8) | ((ntohl(nat_ip) & 0x1FFFF) << 15);
	tbl[2] = ((ntohl(nat_ip) & 0xFFFE0000) >> 17) | (ntohs(nat_port) << 15) |
		 ((ntohs(tuple_dp) & 0x1) << 31);
	tbl[3] = ((ntohs(tuple_dp) & 0xFFFE) >> 1) | (ntohs(tuple_sp) << 15) |
		 ((proto == 6) << 31);
	tbl[4] = ntohl(tuple_dip);
	tbl[5] = ntohl(tuple_sip);
	tbl[6] = (is_snat ? CTF_NP_INTERNAL : 1U /* CTF_NP_EXTERNAL */) | (1U << 20);
	tbl[7] = (1U << 31);
}

static int __init fa_napt_row_test_init(void)
{
	static const u8 test_dst_mac[ETH_ALEN] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01 };
	u32 nh_row[NH_ROW_WORDS];
	u32 nf_row[NF_ROW_WORDS];
	u32 nh_readback[NH_ROW_WORDS];
	u32 nf_readback[NF_ROW_WORDS];
	__be32 lan_ip = htonl(0xC0A80132);	/* 192.168.1.50 - synthetic LAN client */
	__be32 wan_ip = htonl(0x5DB8D822);	/* 93.184.216.34 - example.com, unused as a real dest */
	__be16 lan_port = htons(34567);
	__be16 wan_port = htons(80);
	bool ok;
	int i;

	pr_info("fa_napt_row_test: mapping FA register block at phys=0x%08lx\n", FA_PHYS_BASE);
	fa_base = ioremap(FA_PHYS_BASE, FA_MAP_SIZE);
	if (!fa_base) {
		pr_err("fa_napt_row_test: ioremap failed\n");
		return -ENOMEM;
	}

	/* --- Step 1: real next-hop entry (synthetic dest MAC) --- */
	fa_set_nh_entry(nh_row, test_dst_mac, 0, CTF_NH_OP_NOTAG, 0);
	pr_info("fa_napt_row_test: next-hop row (dst mac de:ad:be:ef:00:01) = "
		"0x%08x 0x%08x 0x%08x\n", nh_row[0], nh_row[1], nh_row[2]);

	ok = fa_macc_write(CTF_MEMACC_TBL_NH, NH_TEST_INDEX, nh_row, NH_ROW_WORDS);
	if (!ok) {
		pr_err("fa_napt_row_test: next-hop write busy-timeout, aborting\n");
		goto out_unmap;
	}
	ok = fa_macc_read(CTF_MEMACC_TBL_NH, NH_TEST_INDEX, nh_readback, NH_ROW_WORDS);
	for (i = 0; i < NH_ROW_WORDS; i++)
		pr_info("fa_napt_row_test: next-hop word[%d] wrote=0x%08x read=0x%08x %s\n",
			i, nh_row[i], nh_readback[i],
			(nh_row[i] == nh_readback[i]) ? "MATCH" : "differs (see fa_macc_test.c word2-width note)");

	/* --- Step 2: real, complete NAPT flow row --- */
	fa_prep_napt_ipv4_row(nf_row, CTF_NAPT_OVRW_IP, 0, 0, NH_TEST_INDEX,
			      wan_ip, wan_port, wan_ip, lan_ip, wan_port, lan_port,
			      6 /* TCP */, true /* SNAT, LAN->WAN */);

	pr_info("fa_napt_row_test: NAPT flow row (192.168.1.50:34567 -> 93.184.216.34:80, "
		"synthetic, no packet sent) tbl[0..7] = "
		"0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n",
		nf_row[0], nf_row[1], nf_row[2], nf_row[3],
		nf_row[4], nf_row[5], nf_row[6], nf_row[7]);

	ok = fa_macc_write(CTF_MEMACC_TBL_NF, NF_TEST_INDEX, nf_row, NF_ROW_WORDS);
	if (!ok) {
		pr_err("fa_napt_row_test: NAPT flow write busy-timeout, aborting\n");
		goto out_unmap;
	}

	ok = fa_macc_read(CTF_MEMACC_TBL_NF, NF_TEST_INDEX, nf_readback, NF_ROW_WORDS);
	for (i = 0; i < NF_ROW_WORDS; i++)
		pr_info("fa_napt_row_test: NAPT flow word[%d] wrote=0x%08x read=0x%08x %s\n",
			i, nf_row[i], nf_readback[i],
			(nf_row[i] == nf_readback[i]) ? "MATCH" : "MISMATCH");

	pr_info("fa_napt_row_test: valid bit (tbl[6] bit20) after write = %d\n",
		!!(nf_readback[6] & (1U << 20)));

	/* --- Step 3: delete (mark invalid), matching _fa_napt_del() exactly --- */
	nf_readback[6] &= ~(1U << 20);
	ok = fa_macc_write(CTF_MEMACC_TBL_NF, NF_TEST_INDEX, nf_readback, NF_ROW_WORDS);
	if (!ok) {
		pr_err("fa_napt_row_test: delete write busy-timeout\n");
		goto out_unmap;
	}

	ok = fa_macc_read(CTF_MEMACC_TBL_NF, NF_TEST_INDEX, nf_readback, NF_ROW_WORDS);
	pr_info("fa_napt_row_test: valid bit (tbl[6] bit20) after delete = %d %s\n",
		!!(nf_readback[6] & (1U << 20)),
		!(nf_readback[6] & (1U << 20)) ? "DELETE-CONFIRMED" : "DELETE-DID-NOT-STICK");

out_unmap:
	iounmap(fa_base);
	fa_base = NULL;
	pr_info("fa_napt_row_test: unmapped, test complete\n");

	return 0;
}

static void __exit fa_napt_row_test_exit(void)
{
	pr_info("fa_napt_row_test: module unloaded\n");
}

module_init(fa_napt_row_test_init);
module_exit(fa_napt_row_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hwoffload-research");
MODULE_DESCRIPTION("FA Phase C proof: real next-hop + NAPT flow row write/read/delete, synthetic data");
