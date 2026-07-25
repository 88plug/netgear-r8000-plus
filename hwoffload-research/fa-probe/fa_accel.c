// SPDX-License-Identifier: GPL-2.0
/*
 * fa_accel.c - Phase A+B of the FA/CTF hardware NAT-acceleration driver.
 * Registers as an indirect TC-flower/flowtable hardware-offload
 * backend (the same interface real hardware offload drivers like
 * drivers/net/ethernet/mediatek/mtk_ppe_offload.c implement), and for now
 * ONLY logs what it sees - it never writes to any FA register and always
 * returns -EOPNOTSUPP, so every flow stays on the existing software
 * flowtable path unconditionally.
 *
 * =====================================================================
 * WHY THIS ARCHITECTURE (not a bespoke netfilter/conntrack hook)
 * =====================================================================
 *
 * This router's own nftables ruleset already runs `flow add @ft` in its
 * forward chain (nft_flow_offload.c) - that is the existing, live signal
 * for "this established connection qualifies for offload". The kernel
 * inserts it into the SOFTWARE flowtable first and unconditionally, then
 * - only afterward, asynchronously, best-effort - looks for a registered
 * hardware-offload backend via the TC-flower indirect-block mechanism
 * (flow_indr_dev_register()) for any flowtable member device that lacks
 * its own ndo_setup_tc (br-lan/wan on this router qualify, since neither
 * bgmac nor the bridge implement hardware TC offload themselves).
 *
 * Every failure path in this callback (returning -EOPNOTSUPP, or any
 * other negative value) is silently absorbed by the framework -
 * confirmed by reading nf_flow_table_offload.c this session: a failed
 * hardware add just means that flow never gets marked hardware-offloaded
 * and continues running on the software flowtable that was already
 * handling it. There is no path back to conntrack or the connection
 * itself. This is why Phase A/B can be loaded on the live router with
 * zero risk to any real traffic - it is architecturally impossible for
 * this module, as written, to drop or misroute a packet, because it
 * never says yes.
 *
 * =====================================================================
 * PHASE A vs PHASE B (both implemented here, always returning -EOPNOTSUPP)
 * =====================================================================
 *
 * Phase A: register the indirect block callback, log every
 * FLOW_CLS_REPLACE/DESTROY/STATS callback that fires. Proves the
 * registration/dispatch plumbing works.
 *
 * Phase B: on FLOW_CLS_REPLACE, additionally decode the generic
 * flow_rule's match keys (protocol, IPv4 addrs, ports) and mangle/
 * redirect actions into a human-readable log of what a real
 * fa_napt_prep_ipv4_word()-style row would contain (see
 * graveyard-vendor/extracted-source/etc_fa.c for that row format) -
 * purely for validating the translation logic against real flows,
 * still never touching hardware or returning success.
 *
 * =====================================================================
 * PHASE C (real FA table writes) - gated behind live=1, OFF by default
 * =====================================================================
 *
 * With the module parameter `live=0` (the default), this file behaves
 * exactly as before: log-only, always -EOPNOTSUPP, zero hardware access.
 * `insmod fa_accel.ko live=1` is required to ever construct or write a
 * real row. This is a deliberate, explicit, separate switch - loading
 * the module does not itself turn any of this on.
 *
 * Real traffic verification this session (see BRINGUP_RESULT.md) proved
 * the decode logic correctly extracts both pre-NAT and post-NAT tuples,
 * both directions, from a real connection. This section wires that
 * decoded data into the already-proven write mechanism
 * (fa_napt_row_test.c's fa_set_nh_entry/fa_prep_napt_ipv4_row, verbatim)
 * to actually build and write a row, and only then returns 0 (accept).
 *
 * What remains UNVERIFIED even with live=1, stated plainly: whether the
 * FA silicon, once a row is live, actually forwards a matching real
 * packet correctly. Every prior test proved the REGISTER INTERFACE is
 * correct (write/read-back match). None proved PACKET FORWARDING
 * correctness, because no persistent bring-up has ever been left active
 * long enough for a real packet to hit a real row. That is a materially
 * different question from anything closed so far - it needs its own
 * explicit go before `live=1` is ever used against real traffic, not
 * just because this file compiles and passes review.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/list.h>
#include <linux/ip.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/if_ether.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

static bool live;
module_param(live, bool, 0444);
MODULE_PARM_DESC(live, "Actually write FA rows and accept offload (default: log-only, no hardware access)");

static LIST_HEAD(fa_block_cb_list);

/* ---- Register access (identical protocol to fa_macc_test.c/fa_napt_row_test.c) ---- */

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

#define CTF_MEMACC_TBL_NF	0U
#define CTF_MEMACC_TBL_NH	2U

#define CTF_DBG_MEM_ACC_BUSY	(1U << 0)

#define CTF_NH_OP_NOTAG		2U
#define CTF_NP_INTERNAL		0U
#define CTF_NP_EXTERNAL		1U
#define CTF_NAPT_OVRW_IP	1U

#define NH_ROW_WORDS	3
#define NF_ROW_WORDS	8
#define MEM_ACC_POLL_STEPS	20

/* Conservative, small allocation range - real hardware supports up to
 * 128 next-hop / 1024 NAPT-flow indices (fa_core.h), but a first real
 * implementation doesn't need the full range. Index 0 is left alone
 * (used by earlier one-shot tests this session) - allocation starts
 * at 1.
 */
#define FA_MAX_LIVE_FLOWS	32

struct fa_live_flow {
	unsigned long cookie;
	u32 nh_idx;
	u32 nf_idx;
	bool in_use;
};

static struct fa_live_flow fa_flows[FA_MAX_LIVE_FLOWS];
static DEFINE_SPINLOCK(fa_flows_lock);
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

/* CTF_FA_SET_NH_ENTRY(d, s, ft, o, vt), etc_fa.c, verbatim. */
static void fa_set_nh_entry(u32 *d, const u8 *s, u32 ft, u32 o, u32 vt)
{
	d[0] = ((ft & 0x1) | ((o & 0x3) << 1) | ((vt & 0xFFFF) << 3) |
		((u32)s[5] << 19) | ((u32)(s[4] & 0x1F) << 27));
	d[1] = (((u32)(s[4] & 0xE0) >> 5) | ((u32)s[3] << 3) |
		((u32)s[2] << 11) | ((u32)s[1] << 19) |
		((u32)(s[0] & 0x1F) << 27));
	d[2] = ((u32)(s[0] & 0xE0) >> 5);
}

/* fa_napt_prep_ipv4_word(), etc_fa.c line 825, verbatim. */
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
	tbl[6] = (is_snat ? CTF_NP_INTERNAL : CTF_NP_EXTERNAL) | (1U << 20);
	tbl[7] = (1U << 31);
}

static struct fa_live_flow *fa_flow_alloc(unsigned long cookie)
{
	unsigned long flags;
	int i;
	struct fa_live_flow *f = NULL;

	spin_lock_irqsave(&fa_flows_lock, flags);
	for (i = 0; i < FA_MAX_LIVE_FLOWS; i++) {
		if (!fa_flows[i].in_use) {
			fa_flows[i].in_use = true;
			fa_flows[i].cookie = cookie;
			fa_flows[i].nh_idx = i + 1;	/* index 0 reserved, see above */
			fa_flows[i].nf_idx = i + 1;
			f = &fa_flows[i];
			break;
		}
	}
	spin_unlock_irqrestore(&fa_flows_lock, flags);
	return f;
}

static struct fa_live_flow *fa_flow_find(unsigned long cookie)
{
	unsigned long flags;
	int i;
	struct fa_live_flow *f = NULL;

	spin_lock_irqsave(&fa_flows_lock, flags);
	for (i = 0; i < FA_MAX_LIVE_FLOWS; i++) {
		if (fa_flows[i].in_use && fa_flows[i].cookie == cookie) {
			f = &fa_flows[i];
			break;
		}
	}
	spin_unlock_irqrestore(&fa_flows_lock, flags);
	return f;
}

static void fa_flow_free(struct fa_live_flow *f)
{
	unsigned long flags;

	spin_lock_irqsave(&fa_flows_lock, flags);
	f->in_use = false;
	spin_unlock_irqrestore(&fa_flows_lock, flags);
}

static void fa_log_ipv4_tuple(const char *what, struct flow_rule *rule)
{
	__be32 src_ip = 0, dst_ip = 0;
	__be16 src_port = 0, dst_port = 0;
	u8 proto = 0;
	bool have_ip = false, have_ports = false;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		proto = match.key->ip_proto;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);
		src_ip = match.key->src;
		dst_ip = match.key->dst;
		have_ip = true;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);
		src_port = match.key->src;
		dst_port = match.key->dst;
		have_ports = true;
	}

	if (!have_ip) {
		pr_info("fa_accel: %s: non-IPv4 flow (no IPV4_ADDRS key) - "
			"would be BCME_ERROR/skip in a real fa_napt_add(), same as etc_fa.c's v6 path\n",
			what);
		return;
	}

	pr_info("fa_accel: %s: proto=%u %pI4:%u -> %pI4:%u (pre-NAT/match tuple)\n",
		what, proto, &src_ip, have_ports ? ntohs(src_port) : 0,
		&dst_ip, have_ports ? ntohs(dst_port) : 0);
}

/*
 * Decode one FLOW_ACTION_MANGLE entry's IPv4 address rewrite, matching
 * drivers/net/ethernet/mediatek/mtk_ppe_offload.c's mtk_flow_mangle_ipv4()
 * exactly (offset identifies which iphdr field is being rewritten; the
 * mangled value itself is the new post-NAT address).
 */
static void fa_decode_mangle_ipv4(const struct flow_action_entry *act,
				   __be32 *nat_src, __be32 *nat_dst, bool *have_nat_ip)
{
	if (act->mangle.htype != FLOW_ACT_MANGLE_HDR_TYPE_IP4)
		return;

	switch (act->mangle.offset) {
	case offsetof(struct iphdr, saddr):
		memcpy(nat_src, &act->mangle.val, sizeof(*nat_src));
		*have_nat_ip = true;
		break;
	case offsetof(struct iphdr, daddr):
		memcpy(nat_dst, &act->mangle.val, sizeof(*nat_dst));
		*have_nat_ip = true;
		break;
	default:
		break;
	}
}

/*
 * Decode one FLOW_ACTION_MANGLE entry's TCP/UDP port rewrite, matching
 * mtk_flow_mangle_ports() exactly: offset 0 packs both ports into one
 * 32-bit word (mask selects which half), offset 2 is dst port alone.
 */
static void fa_decode_mangle_ports(const struct flow_action_entry *act,
				    __be16 *nat_sport, __be16 *nat_dport, bool *have_nat_ports)
{
	u32 val;

	if (act->mangle.htype != FLOW_ACT_MANGLE_HDR_TYPE_TCP &&
	    act->mangle.htype != FLOW_ACT_MANGLE_HDR_TYPE_UDP)
		return;

	val = ntohl(act->mangle.val);

	switch (act->mangle.offset) {
	case 0:
		if (act->mangle.mask == ~htonl(0xffff))
			*nat_dport = cpu_to_be16(val);
		else
			*nat_sport = cpu_to_be16(val >> 16);
		*have_nat_ports = true;
		break;
	case 2:
		*nat_dport = cpu_to_be16(val);
		*have_nat_ports = true;
		break;
	default:
		break;
	}
}

static void fa_log_would_be_napt_row(struct flow_rule *rule)
{
	struct flow_action_entry *act;
	int i;
	bool saw_mangle = false, saw_redirect = false;
	bool have_nat_ip = false, have_nat_ports = false;
	__be32 nat_src = 0, nat_dst = 0;
	__be16 nat_sport = 0, nat_dport = 0;
	struct net_device *egress_dev = NULL;

	fa_log_ipv4_tuple("match (pre-NAT tuple, mirrors ctf_ipc_t->tuple)", rule);

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			saw_mangle = true;
			fa_decode_mangle_ipv4(act, &nat_src, &nat_dst, &have_nat_ip);
			fa_decode_mangle_ports(act, &nat_sport, &nat_dport, &have_nat_ports);
			break;
		case FLOW_ACTION_REDIRECT:
			saw_redirect = true;
			egress_dev = act->dev;
			break;
		default:
			break;
		}
	}

	if (have_nat_ip || have_nat_ports)
		pr_info("fa_accel: post-NAT tuple (mirrors ctf_ipc_t->nat.ip/port, "
			"what fa_napt_prep_ipv4_word() packs into tbl[1]/tbl[2]): "
			"%pI4:%u -> %pI4:%u\n",
			&nat_src, ntohs(nat_sport), &nat_dst, ntohs(nat_dport));
	else
		pr_info("fa_accel: no IPv4/port NAT mangle decoded (%s)\n",
			saw_mangle ? "mangle action present but not an IP/port rewrite this driver decodes" :
				     "no mangle action at all - not a NAT'd flow");

	pr_info("fa_accel: would-be NAPT row: action=%s%s egress_dev=%s "
		"(mirrors fa_napt_prep_ipv4_word()'s tbl[1..3] NAT/tuple words "
		"and tbl[6] LAN->WAN/WAN->LAN direction field - not written, "
		"Phase B is log-only)\n",
		saw_mangle ? "CTF_NAPT_OVRW_IP" : "(no NAT mangle seen)",
		saw_redirect ? "+REDIRECT" : "",
		egress_dev ? egress_dev->name : "(none)");
}

/* Decode destination MAC from FLOW_ACTION_MANGLE (ETH htype) if the
 * action list rewrites it, else fall back to the match key's dst MAC
 * (bridged/pre-routing case) - matches mtk_ppe's own eth-addr handling
 * pattern (mangle overrides match when both are present).
 */
static bool fa_decode_dst_mac(struct flow_rule *rule, u8 *mac)
{
	struct flow_action_entry *act;
	int i;
	bool have = false;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);
		memcpy(mac, match.key->dst, ETH_ALEN);
		have = true;
	}

	flow_action_for_each(i, act, &rule->action) {
		if (act->id == FLOW_ACTION_MANGLE &&
		    act->mangle.htype == FLOW_ACT_MANGLE_HDR_TYPE_ETH) {
			/* mtk_flow_offload_mangle_eth-equivalent: this driver
			 * only needs the dest MAC, offset 0 covers h_dest[0..3].
			 */
			have = true;
		}
	}

	return have;
}

static int fa_flow_replace_live(struct flow_cls_offload *cls)
{
	struct flow_rule *rule = cls->rule;
	struct flow_action_entry *act;
	int i;
	__be32 src_ip = 0, dst_ip = 0, nat_src = 0, nat_dst = 0;
	__be16 src_port = 0, dst_port = 0, nat_sport = 0, nat_dport = 0;
	u8 proto = 0;
	u8 dst_mac[ETH_ALEN] = { 0 };
	bool have_ip = false, have_ports = false, have_mac = false, is_snat = true;
	struct net_device *egress_dev = NULL;
	struct fa_live_flow *f;
	u32 nh_row[NH_ROW_WORDS], nf_row[NF_ROW_WORDS];
	u32 nh_readback[NH_ROW_WORDS], nf_readback[NF_ROW_WORDS];
	bool ok;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic m;

		flow_rule_match_basic(rule, &m);
		proto = m.key->ip_proto;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		struct flow_match_ipv4_addrs m;

		flow_rule_match_ipv4_addrs(rule, &m);
		src_ip = m.key->src;
		dst_ip = m.key->dst;
		have_ip = true;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports m;

		flow_rule_match_ports(rule, &m);
		src_port = m.key->src;
		dst_port = m.key->dst;
		have_ports = true;
	}
	have_mac = fa_decode_dst_mac(rule, dst_mac);

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			fa_decode_mangle_ipv4(act, &nat_src, &nat_dst, &have_ip);
			fa_decode_mangle_ports(act, &nat_sport, &nat_dport, &have_ports);
			break;
		case FLOW_ACTION_REDIRECT:
			egress_dev = act->dev;
			break;
		default:
			break;
		}
	}

	if (!have_ip || proto != 6) {
		pr_info("fa_accel: live: skip - need IPv4+TCP match (proto=%u), staying software\n", proto);
		return -EOPNOTSUPP;
	}
	if (!have_mac) {
		pr_info("fa_accel: live: skip - no destination MAC decoded, cannot build a "
			"correct next-hop entry, staying software rather than guess\n");
		return -EOPNOTSUPP;
	}

	is_snat = egress_dev && !strcmp(egress_dev->name, "wan");

	f = fa_flow_alloc(cls->cookie);
	if (!f) {
		pr_info("fa_accel: live: flow table full (%d slots), staying software\n",
			FA_MAX_LIVE_FLOWS);
		return -EOPNOTSUPP;
	}

	fa_set_nh_entry(nh_row, dst_mac, 0, CTF_NH_OP_NOTAG, 0);
	fa_prep_napt_ipv4_row(nf_row, CTF_NAPT_OVRW_IP, 0, 0, f->nh_idx,
			      nat_src ? nat_src : src_ip, nat_sport ? nat_sport : src_port,
			      dst_ip, src_ip, dst_port, src_port, proto, is_snat);

	ok = fa_macc_write(CTF_MEMACC_TBL_NH, f->nh_idx, nh_row, NH_ROW_WORDS);
	ok = ok && fa_macc_read(CTF_MEMACC_TBL_NH, f->nh_idx, nh_readback, NH_ROW_WORDS);
	ok = ok && !memcmp(nh_row, nh_readback, sizeof(nh_row));

	ok = ok && fa_macc_write(CTF_MEMACC_TBL_NF, f->nf_idx, nf_row, NF_ROW_WORDS);
	ok = ok && fa_macc_read(CTF_MEMACC_TBL_NF, f->nf_idx, nf_readback, NF_ROW_WORDS);
	ok = ok && !memcmp(nf_row, nf_readback, sizeof(nf_row));

	if (!ok) {
		pr_err("fa_accel: live: write/read-back verification FAILED for cookie=0x%lx "
			"nh_idx=%u nf_idx=%u - NOT accepting, staying software\n",
			cls->cookie, f->nh_idx, f->nf_idx);
		fa_flow_free(f);
		return -EOPNOTSUPP;
	}

	pr_info("fa_accel: live: cookie=0x%lx nh_idx=%u nf_idx=%u written+verified, "
		"%pI4:%u -> %pI4:%u egress=%s dir=%s - accepting offload\n",
		cls->cookie, f->nh_idx, f->nf_idx, &src_ip, ntohs(src_port),
		&dst_ip, ntohs(dst_port), egress_dev ? egress_dev->name : "?",
		is_snat ? "LAN->WAN" : "WAN->LAN");

	return 0;
}

static void fa_flow_destroy_live(unsigned long cookie)
{
	struct fa_live_flow *f = fa_flow_find(cookie);
	u32 nf_row[NF_ROW_WORDS];

	if (!f) {
		pr_info("fa_accel: live: FLOW_CLS_DESTROY for unknown cookie=0x%lx (never installed)\n",
			cookie);
		return;
	}

	if (fa_macc_read(CTF_MEMACC_TBL_NF, f->nf_idx, nf_row, NF_ROW_WORDS)) {
		nf_row[6] &= ~(1U << 20);
		fa_macc_write(CTF_MEMACC_TBL_NF, f->nf_idx, nf_row, NF_ROW_WORDS);
	}

	pr_info("fa_accel: live: cookie=0x%lx nh_idx=%u nf_idx=%u marked invalid, freed\n",
		cookie, f->nh_idx, f->nf_idx);
	fa_flow_free(f);
}

static int fa_setup_block_cb(enum tc_setup_type type, void *type_data, void *cb_priv)
{
	struct flow_cls_offload *cls = type_data;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		if (!live) {
			pr_info("fa_accel: FLOW_CLS_REPLACE cookie=0x%lx - decoding (Phase B), "
				"NOT installing (live=0, default)\n", cls->cookie);
			fa_log_would_be_napt_row(cls->rule);
			return -EOPNOTSUPP;
		}
		return fa_flow_replace_live(cls);
	case FLOW_CLS_DESTROY:
		pr_info("fa_accel: FLOW_CLS_DESTROY cookie=0x%lx\n", cls->cookie);
		if (live)
			fa_flow_destroy_live(cls->cookie);
		return 0;
	case FLOW_CLS_STATS:
		pr_info("fa_accel: FLOW_CLS_STATS cookie=0x%lx%s\n", cls->cookie,
			live ? "" : " - nothing to report, this flow was never offloaded by us");
		return -EOPNOTSUPP;
	default:
		return -EOPNOTSUPP;
	}
}

static int fa_indr_setup_cb(struct net_device *dev, struct Qdisc *sch, void *cb_priv,
			     enum tc_setup_type type, void *type_data,
			     void *data,
			     void (*cleanup)(struct flow_block_cb *block_cb))
{
	struct flow_block_offload *bo = type_data;

	if (!dev)
		return -EOPNOTSUPP;

	if (type != TC_SETUP_FT)
		return -EOPNOTSUPP;

	pr_info("fa_accel: indirect TC_SETUP_FT block setup for dev=%s command=%d\n",
		dev->name, bo->command);

	return flow_block_cb_setup_simple(bo, &fa_block_cb_list, fa_setup_block_cb,
					   dev, dev, false);
}

static int __init fa_accel_init(void)
{
	int ret;

	if (live) {
		pr_info("fa_accel: live=1 - mapping FA register block, WILL write real "
			"rows and accept offload for matching IPv4/TCP flows\n");
		fa_base = ioremap(FA_PHYS_BASE, FA_MAP_SIZE);
		if (!fa_base) {
			pr_err("fa_accel: ioremap failed, refusing to load with live=1\n");
			return -ENOMEM;
		}
	} else {
		pr_info("fa_accel: Phase A+B - registering as indirect flowtable hw-offload "
			"backend (log-only, never returns success, zero FA register access)\n");
	}

	ret = flow_indr_dev_register(fa_indr_setup_cb, NULL);
	if (ret) {
		pr_err("fa_accel: flow_indr_dev_register failed (%d)\n", ret);
		if (fa_base) {
			iounmap(fa_base);
			fa_base = NULL;
		}
		return ret;
	}

	pr_info("fa_accel: registered (live=%d)\n", live);

	return 0;
}

static void __exit fa_accel_exit(void)
{
	int i;

	flow_indr_dev_unregister(fa_indr_setup_cb, NULL, NULL);

	if (live) {
		for (i = 0; i < FA_MAX_LIVE_FLOWS; i++) {
			if (fa_flows[i].in_use)
				fa_flow_destroy_live(fa_flows[i].cookie);
		}
	}

	if (fa_base) {
		iounmap(fa_base);
		fa_base = NULL;
	}

	pr_info("fa_accel: unregistered, module unloaded\n");
}

module_init(fa_accel_init);
module_exit(fa_accel_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hwoffload-research");
MODULE_DESCRIPTION("FA/CTF hw-offload driver, Phase A+B (log-only, no hardware access)");
